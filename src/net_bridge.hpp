/**
 * @file net_bridge.hpp
 * @brief TCP network bridge for the HiveShock crowd-control mod.
 *
 * Provides a lightweight, non-blocking TCP server that listens for incoming
 * text-based commands from an external overlay/bot application. Commands are
 * parsed into `CommandEvent` structs and placed into a lock-free-ish
 * thread-safe queue that the game-thread side can drain every frame.
 *
 * Protocol (one command per TCP connection):
 *   COMMAND_ID|SENDER_NAME|AMOUNT|PAYLOAD\n
 *
 * @copyright Copyright (c) 2026 HiveShock Contributors
 * @license MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Platform-specific socket type aliases
// ---------------------------------------------------------------------------
#ifdef _WIN32
    // Forward-declare just enough so callers don't need <WinSock2.h> in every
    // translation unit that includes this header.  The .cpp pulls in the real
    // headers.
    using SocketHandle = unsigned long long; // matches SOCKET (UINT_PTR on x64)
    constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~0);
#else
    using SocketHandle = int;
    constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace hiveshock {

// ===========================================================================
// CommandEvent — a single parsed command received over the wire
// ===========================================================================

/**
 * @struct CommandEvent
 * @brief Represents a single crowd-control command received from the network.
 *
 * Fields mirror the wire format:
 *   COMMAND_ID | SENDER_NAME | AMOUNT | PAYLOAD
 *
 * The @c timestamp is stamped server-side at the moment the message is parsed,
 * so the game thread can implement staleness checks or throttling.
 */
struct CommandEvent {
    /// Canonical command identifier (e.g. "spawn_bomb", "give_rupee").
    std::string commandId;

    /// Human-readable name of the viewer / sender who triggered this command.
    std::string senderName;

    /// Numeric amount associated with the command (e.g. rupee count, strength).
    /// Parsed from "value"/"amount"/"duration"/"strength", in that priority.
    uint32_t amount = 0;

    /// Secondary numeric field, parsed only from an explicit "damage" key.
    /// Kept independent from `amount` so effects like explode/bomb_explode
    /// can carry strength (amount) and damage separately over the wire —
    /// they used to collide because "damage" was just another fallback in
    /// the same amount-parsing chain.
    uint32_t damage = 0;

    /// Free-form payload string for commands that need extra data.
    std::string payload;

    /// Monotonic timestamp of when this event was received & parsed.
    std::chrono::steady_clock::time_point timestamp =
        std::chrono::steady_clock::now();
};

// ===========================================================================
// ThreadSafeQueue<T> — bounded-less MPSC queue for cross-thread delivery
// ===========================================================================

/**
 * @class ThreadSafeQueue
 * @brief A simple mutex-guarded FIFO queue safe for multi-producer,
 *        single-consumer usage.
 *
 * The network listener thread pushes events, and the game thread drains them
 * once per frame via @c tryPopAll().  The underlying storage is a
 * `std::deque` so individual pushes/pops are O(1) amortised.
 *
 * @tparam T  Element type (must be move-constructible).
 */
template <typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;
    ~ThreadSafeQueue() = default;

    // Non-copyable, non-movable (mutex is neither).
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    /**
     * @brief Push an element to the back of the queue.
     * @param item  The item to enqueue (moved in).
     */
    void push(T item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deque.push_back(std::move(item));
    }

    /**
     * @brief Try to pop a single element from the front.
     * @param[out] out  Receives the popped element on success.
     * @return `true` if an element was dequeued, `false` if the queue is empty.
     */
    [[nodiscard]] bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_deque.empty()) {
            return false;
        }
        out = std::move(m_deque.front());
        m_deque.pop_front();
        return true;
    }

    /**
     * @brief Drain the entire queue into @p out in one lock acquisition.
     *
     * This is the preferred consumption method on the game thread — it
     * minimises lock contention by grabbing everything at once.
     *
     * @param[out] out  Vector that receives all queued elements (appended).
     */
    void tryPopAll(std::vector<T>& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.reserve(out.size() + m_deque.size());
        for (auto& elem : m_deque) {
            out.push_back(std::move(elem));
        }
        m_deque.clear();
    }

    /**
     * @brief Return the current number of queued elements.
     * @note  This is inherently racy; use only for diagnostics/logging.
     */
    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deque.size();
    }

    /**
     * @brief Discard all queued elements.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deque.clear();
    }

private:
    mutable std::mutex m_mutex;   ///< Guards all access to @c m_deque.
    std::deque<T>      m_deque;   ///< Underlying FIFO storage.
};

// ===========================================================================
// NetBridge — the TCP listener that feeds CommandEvents to the game thread
// ===========================================================================

/**
 * @class NetBridge
 * @brief Non-blocking TCP server that accepts one-shot command connections.
 *
 * Lifecycle:
 *   1. Construct with a logger and optional port.
 *   2. Call @c start() — creates a listening socket and spawns a background
 *      thread.
 *   3. Each frame, call @c pollEvents() to drain pending commands.
 *   4. Call @c stop() (or let the destructor handle it) to tear down.
 *
 * The server intentionally keeps things simple: each client connection
 * delivers exactly one text command, then the connection is closed. This
 * avoids the complexity of persistent connection management and makes the
 * overlay integration trivial (fire-and-forget TCP writes).
 */
class NetBridge {
public:
    /**
     * @brief Construct the network bridge.
     * @param port    TCP port to listen on (default 43000).
     */
    explicit NetBridge(uint16_t port = 43000);

    /**
     * @brief Destructor — ensures the listener thread is joined and sockets
     *        are closed.
     */
    ~NetBridge();

    // Non-copyable, non-movable.
    NetBridge(const NetBridge&) = delete;
    NetBridge& operator=(const NetBridge&) = delete;
    NetBridge(NetBridge&&) = delete;
    NetBridge& operator=(NetBridge&&) = delete;

    /**
     * @brief Start the TCP listener.
     * @return `true` on success, `false` if socket creation/bind/listen fails.
     */
    bool start();

    /**
     * @brief Gracefully shut down the listener thread and release the socket.
     */
    void stop();

    /**
     * @brief Check whether the listener thread is active.
     * @return `true` if currently listening for connections.
     */
    [[nodiscard]] bool isRunning() const noexcept { return m_running.load(); }

    /**
     * @brief Drain all pending command events into @p outEvents.
     *
     * Intended to be called once per game frame from the main thread.
     *
     * @param[out] outEvents  Vector that receives the drained events.
     */
    void pollEvents(std::vector<CommandEvent>& outEvents);

    /**
     * @brief Discover this machine's first non-loopback LAN IPv4 address.
     * @return The IP address as a dotted-quad string, or "127.0.0.1" if
     *         discovery fails.
     */
    static std::string getLocalIPAddress();

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    static void closeSocket(SocketHandle sock);

    /**
     * @brief Main loop executed on the listener thread.
     *
     * Uses `select()` with a 500 ms timeout so the thread can periodically
     * check @c m_running and exit cleanly without being blocked in `accept()`.
     */
    void listenerLoop();

    /**
     * @brief Handle data from a single accepted client connection.
     * @param clientSocket  The connected client socket descriptor.
     */
    void handleClient(SocketHandle clientSocket);

    /**
     * @brief Parse a raw text line into a @c CommandEvent.
     * @param raw  The raw bytes received (up to the trailing newline).
     * @param[out] out  The filled CommandEvent on success.
     * @return `true` if valid and parsed, `false` otherwise.
     */
    static bool parseCommand(const std::string& raw, CommandEvent& out);

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    uint16_t                      m_port;          ///< TCP listen port.
    std::atomic<bool>             m_running{false}; ///< Listener active flag.
    SocketHandle                  m_listenSock = kInvalidSocket;
    std::thread                   m_thread;        ///< Background listener.
    ThreadSafeQueue<CommandEvent> m_queue;         ///< Inbound event queue.
};

} // namespace hiveshock
