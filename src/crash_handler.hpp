// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// crash_handler.hpp — SEH crash handler & unbuffered forensic logger
//
// Provides three safety layers:
//   1. ForensicLog: Unbuffered file logger that survives crashes.
//   2. CrashHandler: Windows SEH filter that dumps state on fatal exceptions.
//   3. Command ring buffer: Records last 16 commands for post-mortem analysis.
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================
#pragma once

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // Windows.h's min()/max() macros shadow std::min/std::max wherever they're
                 // called without an explicit template arg (std::min<int>(...) dodges it by
                 // accident, but plain std::min(a, b) doesn't) — this is the root fix instead
                 // of patching each call site (found via C2589 on effects.cpp's
                 // truncateDonorName()).
#endif
#include <Windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <atomic>
#include <mutex>

namespace hiveshock {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int kCommandRingSize  = 16;
static constexpr int kFmtBufSize       = 1024;
static constexpr int kPathBufSize      = MAX_PATH;

// ---------------------------------------------------------------------------
// CommandRecord — stored in the ring buffer (POD, no allocations)
// ---------------------------------------------------------------------------
struct CommandRecord {
    char        commandId[64];
    char        sender[64];
    uint64_t    tickCount;
};

// ---------------------------------------------------------------------------
// CommandRing — lock-free(ish) ring buffer of the last N commands
// ---------------------------------------------------------------------------
struct CommandRing {
    CommandRecord   entries[kCommandRingSize];
    std::atomic<uint32_t> head{0};

    inline void record(const char* cmdId, const char* senderName) noexcept {
        uint32_t slot = head.fetch_add(1, std::memory_order_relaxed) % kCommandRingSize;
        // Safe copy — always null-terminate
        std::strncpy(entries[slot].commandId, cmdId   ? cmdId       : "(null)", sizeof(entries[slot].commandId) - 1);
        entries[slot].commandId[sizeof(entries[slot].commandId) - 1] = '\0';
        std::strncpy(entries[slot].sender,    senderName ? senderName : "(null)", sizeof(entries[slot].sender) - 1);
        entries[slot].sender[sizeof(entries[slot].sender) - 1] = '\0';
        entries[slot].tickCount = ::GetTickCount64();
    }
};

// ---------------------------------------------------------------------------
// Global command ring — single static instance
// ---------------------------------------------------------------------------
inline CommandRing& commandRing() noexcept {
    static CommandRing ring{};
    return ring;
}

// ---------------------------------------------------------------------------
// Helpers: crash-safe timestamp (stack-only, no allocations)
// ---------------------------------------------------------------------------
inline void formatTimestamp(char* buf, size_t bufLen) noexcept {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::snprintf(buf, bufLen,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// ---------------------------------------------------------------------------
// Helpers: translate an exception code to a human-readable name
// ---------------------------------------------------------------------------
inline const char* exceptionCodeName(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        default:                                 return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// CrashHandler — Windows SEH singleton
// ---------------------------------------------------------------------------
// Externally settable pointers for post-mortem inspection. The game layer
// should update these as state changes; the crash handler reads them in the
// SEH callback with no locking (plain pointer reads are safe enough during a
// crash dump — we only need a best-effort snapshot).
// ---------------------------------------------------------------------------
struct CrashContext {
    const void*   playerPtr   = nullptr;
    const char*   stageName   = "(unknown)";
};

inline CrashContext& crashContext() noexcept {
    static CrashContext ctx;
    return ctx;
}

class CrashHandler {
public:
    // Meyers' singleton
    static CrashHandler& instance() noexcept {
        static CrashHandler inst;
        return inst;
    }

    // Call once at startup (idempotent).
    inline void init() noexcept {
        if (initialized_) return;
        openLogFile();
        ::SetUnhandledExceptionFilter(&CrashHandler::sehFilter);
        initialized_ = true;
    }

    CrashHandler(const CrashHandler&)            = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

private:
    CrashHandler()  = default;
    ~CrashHandler() {
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }

    // -----------------------------------------------------------------------
    // Open the crash log next to the running executable
    // -----------------------------------------------------------------------
    inline void openLogFile() noexcept {
        char path[kPathBufSize];
        DWORD len = ::GetModuleFileNameA(nullptr, path, kPathBufSize);
        if (len == 0 || len >= kPathBufSize) {
            // Fallback: write to CWD
            file_ = std::fopen("hiveshock_crash.log", "a");
        } else {
            // Strip the executable name to get the directory
            char* lastSlash = std::strrchr(path, '\\');
            if (lastSlash) *(lastSlash + 1) = '\0';
            std::strncat(path, "hiveshock_crash.log", kPathBufSize - std::strlen(path) - 1);
            file_ = std::fopen(path, "a");
        }
        if (file_) {
            // Fully unbuffered — every write goes straight to the OS
            std::setvbuf(file_, nullptr, _IONBF, 0);
        }
    }

    // -----------------------------------------------------------------------
    // SEH callback — must avoid ALL heap allocations
    // -----------------------------------------------------------------------
    static LONG WINAPI sehFilter(EXCEPTION_POINTERS* ep) noexcept {
        CrashHandler& self = instance();
        FILE* f = self.file_;
        if (!f) {
            // Last resort: try stderr
            f = stderr;
        }

        char ts[64];
        formatTimestamp(ts, sizeof(ts));

        // -- Header ----------------------------------------------------------
        std::fprintf(f,
            "\n"
            "========================================\n"
            " HIVESHOCK CRASH REPORT\n"
            " %s\n"
            "========================================\n",
            ts);

        // -- Exception info --------------------------------------------------
        if (ep && ep->ExceptionRecord) {
            DWORD code = ep->ExceptionRecord->ExceptionCode;
            void* addr = ep->ExceptionRecord->ExceptionAddress;
            std::fprintf(f,
                "Exception : 0x%08lX (%s)\n"
                "Address   : 0x%p\n",
                static_cast<unsigned long>(code),
                exceptionCodeName(code),
                addr);

            // For access violations, print the read/write target
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                ep->ExceptionRecord->NumberParameters >= 2)
            {
                ULONG_PTR rwFlag  = ep->ExceptionRecord->ExceptionInformation[0];
                ULONG_PTR target  = ep->ExceptionRecord->ExceptionInformation[1];
                std::fprintf(f,
                    "AV Type   : %s at 0x%llX\n",
                    (rwFlag == 0) ? "READ" : (rwFlag == 1) ? "WRITE" : "EXECUTE",
                    static_cast<unsigned long long>(target));
            }
        }

        // -- Context pointers ------------------------------------------------
        const CrashContext& ctx = crashContext();
        std::fprintf(f,
            "\n--- Key State ---\n"
            "Player ptr : 0x%p\n"
            "Stage      : %s\n",
            ctx.playerPtr,
            ctx.stageName ? ctx.stageName : "(null)");

        // -- Register dump (x64) ---------------------------------------------
#ifdef _M_X64
        if (ep && ep->ContextRecord) {
            const CONTEXT& cr = *ep->ContextRecord;
            std::fprintf(f,
                "\n--- Registers (x64) ---\n"
                "RAX=0x%016llX  RBX=0x%016llX  RCX=0x%016llX\n"
                "RDX=0x%016llX  RSI=0x%016llX  RDI=0x%016llX\n"
                "RSP=0x%016llX  RBP=0x%016llX  RIP=0x%016llX\n"
                "R8 =0x%016llX  R9 =0x%016llX  R10=0x%016llX\n"
                "R11=0x%016llX  R12=0x%016llX  R13=0x%016llX\n"
                "R14=0x%016llX  R15=0x%016llX\n",
                static_cast<unsigned long long>(cr.Rax),
                static_cast<unsigned long long>(cr.Rbx),
                static_cast<unsigned long long>(cr.Rcx),
                static_cast<unsigned long long>(cr.Rdx),
                static_cast<unsigned long long>(cr.Rsi),
                static_cast<unsigned long long>(cr.Rdi),
                static_cast<unsigned long long>(cr.Rsp),
                static_cast<unsigned long long>(cr.Rbp),
                static_cast<unsigned long long>(cr.Rip),
                static_cast<unsigned long long>(cr.R8),
                static_cast<unsigned long long>(cr.R9),
                static_cast<unsigned long long>(cr.R10),
                static_cast<unsigned long long>(cr.R11),
                static_cast<unsigned long long>(cr.R12),
                static_cast<unsigned long long>(cr.R13),
                static_cast<unsigned long long>(cr.R14),
                static_cast<unsigned long long>(cr.R15));
        }
#endif

        // -- Command ring buffer dump ----------------------------------------
        std::fprintf(f, "\n--- Last %d Commands ---\n", kCommandRingSize);
        const CommandRing& ring = commandRing();
        uint32_t headVal = ring.head.load(std::memory_order_relaxed);
        // Walk the ring from oldest to newest
        uint32_t start = (headVal >= kCommandRingSize) ? (headVal - kCommandRingSize) : 0;
        for (uint32_t i = start; i < headVal; ++i) {
            const CommandRecord& rec = ring.entries[i % kCommandRingSize];
            std::fprintf(f,
                "  [%2u] cmd=%-40s sender=%-20s tick=%llu\n",
                i % kCommandRingSize,
                rec.commandId,
                rec.sender,
                static_cast<unsigned long long>(rec.tickCount));
        }

        std::fprintf(f,
            "========================================\n"
            " END CRASH REPORT\n"
            "========================================\n\n");

        // Force flush — the whole point of this module
        std::fflush(f);

        // Let Windows handle the rest (usually WER / crash dialog)
        return EXCEPTION_CONTINUE_SEARCH;
    }

    FILE* file_        = nullptr;
    bool  initialized_ = false;
};

// ---------------------------------------------------------------------------
// ForensicLog — unbuffered, thread-safe text logger
// ---------------------------------------------------------------------------
class ForensicLog {
public:
    static ForensicLog& instance() noexcept {
        static ForensicLog inst;
        return inst;
    }

    // Formatted trace line — prepends timestamp
    inline void trace(const char* fmt, ...) noexcept {
        if (!file_) return;

        char ts[64];
        formatTimestamp(ts, sizeof(ts));

        char msg[kFmtBufSize];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::fprintf(file_, "[%s] %s\n", ts, msg);
            std::fflush(file_);
        }
    }

    // Named checkpoint — useful for "we got this far" breadcrumbs
    inline void checkpoint(const char* label) noexcept {
        if (!file_) return;

        char ts[64];
        formatTimestamp(ts, sizeof(ts));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::fprintf(file_, "[%s] ** CHECKPOINT: %s **\n", ts, label ? label : "(null)");
            std::fflush(file_);
        }
    }

    // Explicit flush (normally not needed since we are unbuffered)
    inline void flush() noexcept {
        if (!file_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::fflush(file_);
    }

    ForensicLog(const ForensicLog&)            = delete;
    ForensicLog& operator=(const ForensicLog&) = delete;

private:
    ForensicLog() noexcept {
        char path[kPathBufSize];
        DWORD len = ::GetModuleFileNameA(nullptr, path, kPathBufSize);
        if (len == 0 || len >= kPathBufSize) {
            file_ = std::fopen("hiveshock_forensic.log", "a");
        } else {
            char* lastSlash = std::strrchr(path, '\\');
            if (lastSlash) *(lastSlash + 1) = '\0';
            std::strncat(path, "hiveshock_forensic.log", kPathBufSize - std::strlen(path) - 1);
            file_ = std::fopen(path, "a");
        }
        if (file_) {
            std::setvbuf(file_, nullptr, _IONBF, 0);
        }
    }

    ~ForensicLog() {
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }

    FILE*       file_ = nullptr;
    std::mutex  mutex_;
};

} // namespace hiveshock

// ---------------------------------------------------------------------------
// Convenience macros — Windows
// ---------------------------------------------------------------------------
#define HIVESHOCK_TRACE(fmt, ...) \
    ::hiveshock::ForensicLog::instance().trace(fmt, ##__VA_ARGS__)

#define HIVESHOCK_CHECKPOINT(label) \
    ::hiveshock::ForensicLog::instance().checkpoint(label)

#define HIVESHOCK_RECORD_CMD(commandId, senderName) \
    ::hiveshock::commandRing().record(commandId, senderName)

#else // !_WIN32

// ---------------------------------------------------------------------------
// Non-Windows fallbacks — degrade gracefully
// ---------------------------------------------------------------------------
#include <cstdio>

#define HIVESHOCK_TRACE(fmt, ...) \
    std::fprintf(stderr, "[HiveShock] " fmt "\n", ##__VA_ARGS__)

#define HIVESHOCK_CHECKPOINT(label) \
    std::fprintf(stderr, "[HiveShock] ** CHECKPOINT: %s **\n", (label))

#define HIVESHOCK_RECORD_CMD(commandId, senderName) \
    ((void)0)

#endif // _WIN32
