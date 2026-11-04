// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// net_bridge.cpp — Asynchronous TCP command bridge (implementation)
//
// Implements a non-blocking TCP server on a background thread.  Commands are
// received as single-line text packets (one per connection), parsed, and
// deposited into a ThreadSafeQueue for main-thread consumption in mod_update().
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================
#include "net_bridge.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Platform-specific includes and helpers
// ---------------------------------------------------------------------------
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX // avoid Windows.h's min()/max() macros shadowing std::min/std::max —
                          // see the matching comment in crash_handler.hpp
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "Iphlpapi.lib")
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <ifaddrs.h>
    #include <sys/select.h>
#endif

namespace hiveshock {

// =============================================================================
// Construction / Destruction
// =============================================================================

NetBridge::NetBridge(uint16_t port)
    : m_port(port)
{}

NetBridge::~NetBridge() {
    stop();
}

// =============================================================================
// closeSocket — platform-safe socket close
// =============================================================================
void NetBridge::closeSocket(SocketHandle sock) {
    if (sock == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
}

// =============================================================================
// getLocalIPAddress — Cross-platform LAN IP discovery
//
// Windows:  Uses GetAdaptersAddresses() to enumerate NICs and picks the first
//           non-loopback IPv4 unicast address.
// POSIX:    Uses getifaddrs() for the same purpose.
// Fallback: "127.0.0.1"
// =============================================================================
std::string NetBridge::getLocalIPAddress() {
#ifdef _WIN32
    // --- Windows: GetAdaptersAddresses ---
    ULONG bufLen = 15000;
    std::vector<uint8_t> buf(bufLen);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufLen);
    }
    if (ret == NO_ERROR) {
        for (auto* a = adapters; a != nullptr; a = a->Next) {
            // Skip loopback and non-operational adapters
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->OperStatus != IfOperStatusUp) continue;

            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
                auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                if (sa->sin_family != AF_INET) continue;

                char ipStr[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

                std::string ip(ipStr);
                if (ip != "127.0.0.1" && ip != "0.0.0.0") {
                    return ip;
                }
            }
        }
    }
#else
    // --- POSIX: getifaddrs ---
    struct ifaddrs* ifList = nullptr;
    if (getifaddrs(&ifList) == 0) {
        for (auto* ifa = ifList; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;

            auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            char ipStr[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

            std::string ip(ipStr);
            if (ip != "127.0.0.1" && ip != "0.0.0.0") {
                freeifaddrs(ifList);
                return ip;
            }
        }
        freeifaddrs(ifList);
    }
#endif
    return "127.0.0.1";
}

// =============================================================================
// start — Create socket, bind, listen, and launch the background thread.
// =============================================================================
bool NetBridge::start() {
    if (m_running.load()) return true; // already started

#ifdef _WIN32
    // Initialise Winsock (idempotent — safe to call more than once).
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
#endif

    // Create TCP socket
    m_listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == kInvalidSocket) {
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // SO_REUSEADDR — allows immediate rebind after a crash / fast restart.
    int optVal = 1;
    ::setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&optVal), sizeof(optVal));

    // Bind to 0.0.0.0:<port>
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(m_port);

    if (::bind(m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(m_listenSock);
        m_listenSock = kInvalidSocket;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    if (::listen(m_listenSock, 5) != 0) {
        closeSocket(m_listenSock);
        m_listenSock = kInvalidSocket;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // Set non-blocking so select() can be used with a timeout for clean
    // shutdown without needing platform-specific cancel mechanisms.
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(m_listenSock, FIONBIO, &mode);
#else
    int flags = fcntl(m_listenSock, F_GETFL, 0);
    fcntl(m_listenSock, F_SETFL, flags | O_NONBLOCK);
#endif

    // Announce the bind explicitly: 0.0.0.0 means "all interfaces", so also
    // resolve and log the concrete LAN-reachable address for convenience —
    // this is the line to check first if an overlay app can't connect.
    {
        std::string localIp = getLocalIPAddress();
        DUSK_LOG_INFO("[HiveShock Net] Bound 0.0.0.0:%u (all interfaces) — reachable at 127.0.0.1:%u locally, %s:%u on the LAN/VPN",
                      m_port, m_port, localIp.c_str(), m_port);
    }

    m_running.store(true);
    m_thread = std::thread(&NetBridge::listenerLoop, this);

    return true;
}

// =============================================================================
// stop — Shut down the listener, join the thread, clean up.
// =============================================================================
void NetBridge::stop() {
    if (!m_running.exchange(false)) return;

    // Close the socket so select() in the listener returns immediately.
    closeSocket(m_listenSock);
    m_listenSock = kInvalidSocket;

    if (m_thread.joinable()) {
        m_thread.join();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

// =============================================================================
// pollEvents — Drain all pending events (called from main thread each frame).
// =============================================================================
void NetBridge::pollEvents(std::vector<CommandEvent>& out) {
    m_queue.tryPopAll(out);
}

// =============================================================================
// listenerLoop — Background accept-loop.
//
// Uses select() with a 500 ms timeout so we can periodically check the
// m_running flag and exit cleanly without platform cancellation primitives.
// =============================================================================
void NetBridge::listenerLoop() {
    while (m_running.load(std::memory_order_relaxed)) {
        fd_set readSet;
        FD_ZERO(&readSet);

        // Guard against invalid socket (may have been closed during stop()).
        if (m_listenSock == kInvalidSocket) break;

        FD_SET(m_listenSock, &readSet);

        // 500 ms timeout — gives us ~2 checks/sec for the shutdown flag.
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 500'000;

#ifdef _WIN32
        int nfds = 0; // ignored on Windows
#else
        int nfds = static_cast<int>(m_listenSock) + 1;
#endif

        int sel = ::select(nfds, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue; // timeout or error — loop back

        // Accept a new client connection.
        sockaddr_in clientAddr{};
        socklen_t   addrLen = sizeof(clientAddr);
        SocketHandle clientSock =
            ::accept(m_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);

        if (clientSock == kInvalidSocket) continue;

        handleClient(clientSock);
    }
}

// =============================================================================
// handleClient — Read the command payload, parse it, enqueue, and close.
//
// We read up to 4 KiB (plenty for our text-based protocol).  The socket is
// set to blocking for the client read since we expect a tiny payload.
// =============================================================================
void NetBridge::handleClient(SocketHandle clientSock) {
    // 1. Explicitly switch client socket to blocking mode.
    // On Windows, accept() on a non-blocking listening socket creates a non-blocking
    // client socket! Calling recv() immediately would return WSAEWOULDBLOCK (-1)
    // and drop the connection before data arrives.
#ifdef _WIN32
    u_long nonBlocking = 0; // 0 = blocking mode
    ::ioctlsocket(clientSock, FIONBIO, &nonBlocking);

    DWORD timeout = 2000; // 2 seconds
    ::setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    int flags = fcntl(clientSock, F_GETFL, 0);
    fcntl(clientSock, F_SETFL, flags & ~O_NONBLOCK);

    timeval tv{};
    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    ::setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    // 2. Wait with select() up to 2 seconds for initial data to arrive.
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(clientSock, &readSet);

    timeval waitTv{};
    waitTv.tv_sec  = 2;
    waitTv.tv_usec = 0;

#ifdef _WIN32
    int nfds = 0;
#else
    int nfds = static_cast<int>(clientSock) + 1;
#endif

    int sel = ::select(nfds, &readSet, nullptr, nullptr, &waitTv);
    if (sel <= 0) {
        DUSK_LOG_WARN("[HiveShock Net] Client connection timed out waiting for data");
        closeSocket(clientSock);
        return;
    }

    // 3. Read the payload until newline or EOF.
    constexpr size_t kBufSize = 4096;
    char buf[kBufSize]{};

    int totalRead = 0;
    while (totalRead < static_cast<int>(kBufSize) - 1) {
        int n = ::recv(clientSock, buf + totalRead,
                       static_cast<int>(kBufSize - 1 - totalRead), 0);
        if (n <= 0) break; // EOF, error, or timeout
        totalRead += n;

        // Stop as soon as we see the terminating newline.
        if (std::memchr(buf, '\n', totalRead) != nullptr) break;
    }

    closeSocket(clientSock);

    if (totalRead <= 0) {
        DUSK_LOG_WARN("[HiveShock Net] Client connected but sent 0 bytes before disconnect");
        return;
    }

    // Trim trailing whitespace / newlines.
    std::string raw(buf, totalRead);
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' ')) {
        raw.pop_back();
    }
    if (raw.empty()) return;

    DUSK_LOG_INFO("[HiveShock Net] Received payload: %s", raw.c_str());

    CommandEvent evt;
    if (parseCommand(raw, evt)) {
        DUSK_LOG_INFO("[HiveShock Net] Enqueued command: '%s' (sender='%s', amount=%u)",
                      evt.commandId.c_str(), evt.senderName.c_str(), evt.amount);
        m_queue.push(std::move(evt));
    } else {
        DUSK_LOG_WARN("[HiveShock Net] Failed to parse command: %s", raw.c_str());
    }
}

// =============================================================================
// Minimal flat-JSON object reader.
//
// Reads the top-level keys of one JSON object into key -> text. String values
// are unescaped; numbers/true/false/null are kept as their raw text (so both
// {"amount":5} and {"amount":"5"} work). Nested objects/arrays are skipped, so
// a key inside a nested value or inside another string can never be mistaken
// for a top-level one. Returns false on malformed input.
// =============================================================================
static bool readJsonString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) return false;
        char e = s[i++];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            if (i + 4 > s.size()) return false;
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
                char h = s[i + k];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= h - '0';
                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                else return false;
            }
            i += 4;
            out.push_back(cp < 128 ? static_cast<char>(cp) : '?'); // names are shown as printable ASCII only
            break;
        }
        default: out.push_back(e); break; // \" \\ \/
        }
    }
    return false;
}

static void skipJsonWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

static bool skipJsonNested(const std::string& s, size_t& i) {
    int depth = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            std::string tmp;
            if (!readJsonString(s, i, tmp)) return false;
            continue;
        }
        ++i;
        if (c == '{' || c == '[') ++depth;
        else if ((c == '}' || c == ']') && --depth == 0) return true;
    }
    return false;
}

static bool parseFlatJson(const std::string& s, std::unordered_map<std::string, std::string>& out) {
    size_t i = 0;
    skipJsonWs(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    for (;;) {
        skipJsonWs(s, i);
        if (i < s.size() && s[i] == '}') return true;
        std::string key;
        if (!readJsonString(s, i, key)) return false;
        skipJsonWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skipJsonWs(s, i);
        if (i >= s.size()) return false;
        std::string val;
        if (s[i] == '"') {
            if (!readJsonString(s, i, val)) return false;
        } else if (s[i] == '{' || s[i] == '[') {
            if (!skipJsonNested(s, i)) return false;
        } else {
            size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\t' &&
                   s[i] != '\r' && s[i] != '\n') ++i;
            val = s.substr(start, i - start);
        }
        out.emplace(std::move(key), std::move(val)); // first occurrence wins
        skipJsonWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        skipJsonWs(s, i);
        return i < s.size() && s[i] == '}';
    }
}

static bool jsonNumber(const std::unordered_map<std::string, std::string>& m, const char* key, double& out) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// =============================================================================
// parseCommand — Parse JSON (primary HiveShock protocol) or pipe-delimited text.
// =============================================================================
bool NetBridge::parseCommand(const std::string& raw, CommandEvent& out) {
    // Trim leading whitespace
    size_t start = 0;
    while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t' || raw[start] == '\r' || raw[start] == '\n')) {
        start++;
    }
    if (start >= raw.size()) return false;

    // Check if JSON format (starts with '{')
    if (raw[start] == '{') {
        std::string jsonStr = raw.substr(start);
        std::unordered_map<std::string, std::string> fields;
        if (!parseFlatJson(jsonStr, fields)) return false;

        std::string action;
        for (const char* k : {"action", "command", "code"}) {
            auto it = fields.find(k);
            if (it != fields.end() && !it->second.empty()) { action = it->second; break; }
        }
        if (action.empty()) return false;

        out.commandId = action;
        for (const char* k : {"user", "sender"}) {
            auto it = fields.find(k);
            if (it != fields.end()) { out.senderName = it->second; break; }
        }

        double val = 0.0;
        out.amount = 0;
        for (const char* k : {"value", "amount", "duration", "strength"}) {
            if (jsonNumber(fields, k, val)) {
                out.amount = static_cast<uint32_t>(std::clamp(val, 0.0, 4294967295.0));
                break;
            }
        }

        // Parsed independently - see the CommandEvent::damage comment.
        double dmgVal = 0.0;
        out.damage = jsonNumber(fields, "damage", dmgVal)
                         ? static_cast<uint32_t>(std::clamp(dmgVal, 0.0, 4294967295.0))
                         : 0;

        out.payload = jsonStr;
        out.timestamp = std::chrono::steady_clock::now();
        return true;
    }

    // Fallback: Legacy Pipe-delimited wire format: COMMAND_ID|SENDER_NAME|AMOUNT|PAYLOAD
    std::vector<std::string> parts;
    parts.reserve(4);
    std::istringstream stream(raw);
    std::string token;
    while (std::getline(stream, token, '|')) {
        parts.push_back(std::move(token));
    }

    if (parts.empty() || parts[0].empty()) return false;

    out.commandId  = parts[0];
    out.senderName = parts.size() > 1 ? parts[1] : "";

    if (parts.size() > 2) {
        try {
            out.amount = static_cast<uint32_t>(std::stoul(parts[2]));
        } catch (...) {
            out.amount = 0;
        }
    } else {
        out.amount = 0;
    }

    if (parts.size() > 3) {
        out.payload = parts[3];
        for (size_t i = 4; i < parts.size(); ++i) {
            out.payload += '|';
            out.payload += parts[i];
        }
    } else {
        out.payload = "";
    }

    out.timestamp = std::chrono::steady_clock::now();
    return true;
}

} // namespace hiveshock
