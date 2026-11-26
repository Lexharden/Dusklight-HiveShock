// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// crash_handler.hpp — crash handler & unbuffered forensic logger
//
// Provides three safety layers, on every platform:
//   1. ForensicLog: Unbuffered file logger that survives crashes.
//   2. CrashHandler: Dumps state on fatal faults, then hands the crash on to
//      whatever handler was installed before it (Crashpad, WER, default):
//        - Windows:     unhandled-exception (SEH) filter.
//        - Linux/macOS: sigaction() on SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT.
//      It only *reports* — it never resumes execution after a fault.
//   3. Command ring buffer: Records last 16 commands for post-mortem analysis.
//
// Where the logs go:
//   - Windows:     next to the game executable.
//   - Linux/macOS: the mod's persistent data folder (HostService::data_dir),
//                  passed in through configureLogDirectory() before init().
//                  The folder next to the executable is read-only there (a
//                  signed .app bundle, /usr, an AppImage). If the host does
//                  not provide a folder, the Dusklight user folder is used.
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================
#pragma once

// ---------------------------------------------------------------------------
// Platform headers
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

#else // POSIX (Linux, macOS)

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h> // <ucontext.h> is deprecated on macOS and needs _XOPEN_SOURCE
#else
#include <ucontext.h>
#endif

#endif // _WIN32

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace hiveshock {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int kCommandRingSize = 16;
static constexpr int kFmtBufSize      = 1024;
#ifdef _WIN32
static constexpr int kPathBufSize     = MAX_PATH;
#else
static constexpr int kPathBufSize     = 1024;
#endif
static constexpr const char* kCrashLogName    = "hiveshock_crash.log";
static constexpr const char* kForensicLogName = "hiveshock_forensic.log";

// ---------------------------------------------------------------------------
// monotonicMs — milliseconds since an arbitrary start point (boot on most
// systems). Async-signal-safe on POSIX: clock_gettime() is on the safe list.
// ---------------------------------------------------------------------------
inline uint64_t monotonicMs() noexcept {
#ifdef _WIN32
    return ::GetTickCount64();
#else
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
#endif
}

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
        entries[slot].tickCount = monotonicMs();
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
// Helpers: local-time timestamp for normal (non-crash) log lines.
// Not async-signal-safe on POSIX (localtime_r takes the tz lock) — the
// signal handler uses detail::writeUtcTimestamp() instead.
// ---------------------------------------------------------------------------
inline void formatTimestamp(char* buf, size_t bufLen) noexcept {
#ifdef _WIN32
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    std::snprintf(buf, bufLen,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    const time_t secs = ts.tv_sec;
    std::tm tmv{};
    ::localtime_r(&secs, &tmv);
    std::snprintf(buf, bufLen,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
        static_cast<int>(ts.tv_nsec / 1000000));
#endif
}

// ---------------------------------------------------------------------------
// Log file location
// ---------------------------------------------------------------------------
namespace detail {

/// Directory the logs go to, always ending in a separator; empty = default.
/// Only used on POSIX (see the header comment).
inline char g_logDir[kPathBufSize] = {};

/// Appends src to the NUL-terminated dst. Leaves dst untouched and returns
/// false if the result would not fit.
inline bool appendPath(char* dst, size_t cap, const char* src) noexcept {
    const size_t len = std::strlen(dst);
    const size_t add = std::strlen(src);
    if (len + add + 1 > cap) return false;
    std::memcpy(dst + len, src, add + 1);
    return true;
}

#ifndef _WIN32
/// The Dusklight user folder — the parent of the mods folder from the README.
inline bool defaultUserDir(char* out, size_t cap) noexcept {
    out[0] = '\0';
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    if (home == nullptr || home[0] == '\0') return false;
    return appendPath(out, cap, home) &&
           appendPath(out, cap, "/Library/Application Support/TwilitRealm/Dusklight/");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] == '/') { // XDG spec: relative paths are invalid, ignore them
        return appendPath(out, cap, xdg) && appendPath(out, cap, "/TwilitRealm/Dusklight/");
    }
    if (home == nullptr || home[0] == '\0') return false;
    return appendPath(out, cap, home) &&
           appendPath(out, cap, "/.local/share/TwilitRealm/Dusklight/");
#endif
}
#endif

/// Builds "<log dir><fileName>" into out. Returns false (out = "") when no
/// directory could be resolved.
inline bool buildLogPath(char* out, size_t cap, const char* fileName) noexcept {
    out[0] = '\0';
#ifdef _WIN32
    const DWORD len = ::GetModuleFileNameA(nullptr, out, static_cast<DWORD>(cap));
    if (len == 0 || len >= cap) { out[0] = '\0'; return false; }
    char* lastSlash = std::strrchr(out, '\\');
    if (lastSlash == nullptr) { out[0] = '\0'; return false; }
    *(lastSlash + 1) = '\0'; // strip the executable name, keep the directory
#else
    if (g_logDir[0] != '\0') {
        appendPath(out, cap, g_logDir); // always fits: same capacity
    } else if (!defaultUserDir(out, cap)) {
        out[0] = '\0';
        return false;
    }
#endif
    if (!appendPath(out, cap, fileName)) { out[0] = '\0'; return false; }
    return true;
}

/// Opens a log in append mode, fully unbuffered. pathOut receives the path
/// actually used. Falls back to the current directory only when no log
/// directory could be resolved at all.
inline std::FILE* openLogFile(const char* fileName, char* pathOut, size_t cap) noexcept {
    if (!buildLogPath(pathOut, cap, fileName)) {
        pathOut[0] = '\0';
        appendPath(pathOut, cap, fileName);
    }
    std::FILE* f = std::fopen(pathOut, "a");
    if (f != nullptr) {
        // Fully unbuffered — every write goes straight to the OS
        std::setvbuf(f, nullptr, _IONBF, 0);
#ifndef _WIN32
        const int fd = ::fileno(f);
        if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC); // don't leak into child processes
#endif
    }
    return f;
}

} // namespace detail

/// Sets where the logs go. Must be called before CrashHandler::init() and the
/// first ForensicLog use. No-op on Windows, where the logs stay next to the
/// executable.
inline void configureLogDirectory([[maybe_unused]] const char* dir) noexcept {
#ifndef _WIN32
    detail::g_logDir[0] = '\0';
    if (dir == nullptr || dir[0] == '\0') return;
    if (!detail::appendPath(detail::g_logDir, sizeof(detail::g_logDir), dir)) {
        detail::g_logDir[0] = '\0';
        return;
    }
    const size_t len = std::strlen(detail::g_logDir);
    if (detail::g_logDir[len - 1] != '/' &&
        !detail::appendPath(detail::g_logDir, sizeof(detail::g_logDir), "/")) {
        detail::g_logDir[0] = '\0';
    }
#endif
}

// ---------------------------------------------------------------------------
// CrashContext
// ---------------------------------------------------------------------------
// Externally settable pointers for post-mortem inspection. The game layer
// should update these as state changes; the crash handler reads them with no
// locking (plain pointer reads are safe enough during a crash dump — we only
// need a best-effort snapshot).
// ---------------------------------------------------------------------------
struct CrashContext {
    const void*   playerPtr   = nullptr;
    const char*   stageName   = "(unknown)";
};

inline CrashContext& crashContext() noexcept {
    static CrashContext ctx;
    return ctx;
}

#ifdef _WIN32
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

#else // POSIX

namespace detail {

// ---------------------------------------------------------------------------
// SignalSafeWriter — formats into a stack buffer and write()s it out.
// Uses no stdio, no locale, no heap: everything here is async-signal-safe,
// which a handler running *inside* a crashed process needs (the crash may
// have happened while malloc's or stdio's lock was held).
// ---------------------------------------------------------------------------
struct SignalSafeWriter {
    int    fd;
    char   buf[512];
    size_t len = 0;

    explicit SignalSafeWriter(int f) noexcept : fd(f) {}

    void flush() noexcept {
        size_t off = 0;
        while (off < len) {
            const ssize_t n = ::write(fd, buf + off, len - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        len = 0;
    }

    void ch(char c) noexcept {
        if (len == sizeof(buf)) flush();
        buf[len++] = c;
    }

    void str(const char* s) noexcept {
        if (s == nullptr) s = "(null)";
        while (*s != '\0') ch(*s++);
    }

    /// Left-aligned string, space-padded to width (like printf's %-Ns).
    void strPadded(const char* s, int width) noexcept {
        if (s == nullptr) s = "(null)";
        int n = 0;
        while (s[n] != '\0') ch(s[n++]);
        while (n++ < width) ch(' ');
    }

    void dec(uint64_t v, int minDigits = 1) noexcept {
        char tmp[20];
        int n = 0;
        do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0 && n < 20);
        while (n < minDigits && n < 20) tmp[n++] = '0';
        while (n > 0) ch(tmp[--n]);
    }

    void decSigned(int64_t v) noexcept {
        if (v < 0) { ch('-'); dec(static_cast<uint64_t>(-(v + 1)) + 1); }
        else       { dec(static_cast<uint64_t>(v)); }
    }

    void hex(uint64_t v, int minDigits = 1) noexcept {
        char tmp[16];
        int n = 0;
        do { tmp[n++] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; } while (v != 0 && n < 16);
        while (n < minDigits && n < 16) tmp[n++] = '0';
        str("0x");
        while (n > 0) ch(tmp[--n]);
    }
};

/// "YYYY-MM-DD HH:MM:SS.mmm UTC" — computed by hand because localtime_r()
/// and gmtime_r() are not async-signal-safe.
inline void writeUtcTimestamp(SignalSafeWriter& w) noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    const int64_t secs = static_cast<int64_t>(ts.tv_sec);
    int64_t days       = secs / 86400;
    int64_t rem        = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }

    // Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm).
    days += 719468;
    const int64_t  era = (days >= 0 ? days : days - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(days - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp  = (5 * doy + 2) / 153;
    const uint64_t day = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t mon = mp < 10 ? mp + 3 : mp - 9;
    const int64_t  yr  = static_cast<int64_t>(yoe) + era * 400 + (mon <= 2 ? 1 : 0);

    w.decSigned(yr);             w.ch('-');
    w.dec(mon, 2);               w.ch('-');
    w.dec(day, 2);               w.ch(' ');
    w.dec(static_cast<uint64_t>(rem / 3600), 2);        w.ch(':');
    w.dec(static_cast<uint64_t>(rem % 3600 / 60), 2);   w.ch(':');
    w.dec(static_cast<uint64_t>(rem % 60), 2);          w.ch('.');
    w.dec(static_cast<uint64_t>(ts.tv_nsec / 1000000), 3);
    w.str(" UTC");
}

inline const char* signalName(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default:      return "UNKNOWN";
    }
}

/// One "NAME=0x0000000000000000" cell; three per line.
inline void writeReg(SignalSafeWriter& w, const char* name, uint64_t value, int& column) noexcept {
    w.strPadded(name, 3);
    w.ch('=');
    w.hex(value, 16);
    if (++column % 3 == 0) w.ch('\n');
    else                   w.str("  ");
}

/// Register dump from the signal's ucontext. Architectures not listed here
/// simply get no register section.
inline void writeRegisters(SignalSafeWriter& w, void* uctxRaw) noexcept {
    if (uctxRaw == nullptr) return;
    [[maybe_unused]] auto* uc = static_cast<ucontext_t*>(uctxRaw);
    int col = 0;

#if defined(__linux__) && defined(__x86_64__)
    const greg_t* g = uc->uc_mcontext.gregs;
    static constexpr struct { const char* name; int idx; } kRegs[] = {
        {"RAX", REG_RAX}, {"RBX", REG_RBX}, {"RCX", REG_RCX},
        {"RDX", REG_RDX}, {"RSI", REG_RSI}, {"RDI", REG_RDI},
        {"RSP", REG_RSP}, {"RBP", REG_RBP}, {"RIP", REG_RIP},
        {"R8",  REG_R8},  {"R9",  REG_R9},  {"R10", REG_R10},
        {"R11", REG_R11}, {"R12", REG_R12}, {"R13", REG_R13},
        {"R14", REG_R14}, {"R15", REG_R15},
    };
    w.str("\n--- Registers (x86_64) ---\n");
    for (const auto& r : kRegs) writeReg(w, r.name, static_cast<uint64_t>(g[r.idx]), col);

#elif defined(__linux__) && defined(__aarch64__)
    const auto& mc = uc->uc_mcontext;
    static constexpr const char* kNames[29] = {
        "X0",  "X1",  "X2",  "X3",  "X4",  "X5",  "X6",  "X7",  "X8",  "X9",
        "X10", "X11", "X12", "X13", "X14", "X15", "X16", "X17", "X18", "X19",
        "X20", "X21", "X22", "X23", "X24", "X25", "X26", "X27", "X28",
    };
    w.str("\n--- Registers (arm64) ---\n");
    for (int i = 0; i < 29; ++i) writeReg(w, kNames[i], static_cast<uint64_t>(mc.regs[i]), col);
    writeReg(w, "FP", static_cast<uint64_t>(mc.regs[29]), col);
    writeReg(w, "LR", static_cast<uint64_t>(mc.regs[30]), col);
    writeReg(w, "SP", static_cast<uint64_t>(mc.sp), col);
    writeReg(w, "PC", static_cast<uint64_t>(mc.pc), col);

#elif defined(__APPLE__) && defined(__x86_64__)
    const auto& ss = uc->uc_mcontext->__ss;
    w.str("\n--- Registers (x86_64) ---\n");
    writeReg(w, "RAX", ss.__rax, col); writeReg(w, "RBX", ss.__rbx, col); writeReg(w, "RCX", ss.__rcx, col);
    writeReg(w, "RDX", ss.__rdx, col); writeReg(w, "RSI", ss.__rsi, col); writeReg(w, "RDI", ss.__rdi, col);
    writeReg(w, "RSP", ss.__rsp, col); writeReg(w, "RBP", ss.__rbp, col); writeReg(w, "RIP", ss.__rip, col);
    writeReg(w, "R8",  ss.__r8,  col); writeReg(w, "R9",  ss.__r9,  col); writeReg(w, "R10", ss.__r10, col);
    writeReg(w, "R11", ss.__r11, col); writeReg(w, "R12", ss.__r12, col); writeReg(w, "R13", ss.__r13, col);
    writeReg(w, "R14", ss.__r14, col); writeReg(w, "R15", ss.__r15, col);

#elif defined(__APPLE__) && defined(__aarch64__)
    const auto& ss = uc->uc_mcontext->__ss;
    static constexpr const char* kNames[29] = {
        "X0",  "X1",  "X2",  "X3",  "X4",  "X5",  "X6",  "X7",  "X8",  "X9",
        "X10", "X11", "X12", "X13", "X14", "X15", "X16", "X17", "X18", "X19",
        "X20", "X21", "X22", "X23", "X24", "X25", "X26", "X27", "X28",
    };
    w.str("\n--- Registers (arm64) ---\n");
    for (int i = 0; i < 29; ++i) writeReg(w, kNames[i], static_cast<uint64_t>(ss.__x[i]), col);
    // fp/lr/sp/pc may be pointer-authenticated/opaque; the SDK accessors handle that.
    writeReg(w, "FP", static_cast<uint64_t>(__darwin_arm_thread_state64_get_fp(ss)), col);
    writeReg(w, "LR", static_cast<uint64_t>(__darwin_arm_thread_state64_get_lr(ss)), col);
    writeReg(w, "SP", static_cast<uint64_t>(__darwin_arm_thread_state64_get_sp(ss)), col);
    writeReg(w, "PC", static_cast<uint64_t>(__darwin_arm_thread_state64_get_pc(ss)), col);
#endif

    if (col % 3 != 0) w.ch('\n');
}

} // namespace detail

#endif // _WIN32

// ---------------------------------------------------------------------------
// CrashHandler — singleton
// ---------------------------------------------------------------------------
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
#ifdef _WIN32
        file_ = detail::openLogFile(kCrashLogName, path_, sizeof(path_));
        prevFilter_ = ::SetUnhandledExceptionFilter(&CrashHandler::sehFilter);
#else
        openLogFd();
        for (int i = 0; i < kNumSignals; ++i) {
            struct sigaction sa{};
            sa.sa_sigaction = &CrashHandler::signalHandler;
            ::sigemptyset(&sa.sa_mask);
            // SA_ONSTACK: use the thread's alternate stack if it has one (Crashpad
            // sets one up), so a stack overflow can still be reported.
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
            installed_[i] = ::sigaction(kSignals[i], &sa, &prev_[i]) == 0;
        }
#endif
        initialized_ = true;
    }

    // Puts back the handler(s) that were there before init(). Must run before
    // the mod's library is unloaded: a handler left installed would point into
    // unmapped memory and turn the next crash anywhere in the game into a
    // second, unreported one.
    inline void shutdown() noexcept {
        if (!initialized_) return;
#ifdef _WIN32
        LPTOP_LEVEL_EXCEPTION_FILTER current = ::SetUnhandledExceptionFilter(prevFilter_);
        if (current != &CrashHandler::sehFilter) {
            // Someone installed their own filter after us — leave theirs in place.
            ::SetUnhandledExceptionFilter(current);
        }
        prevFilter_ = nullptr;
        if (file_) { std::fclose(file_); file_ = nullptr; }
#else
        for (int i = 0; i < kNumSignals; ++i) {
            if (!installed_[i]) continue;
            struct sigaction current{};
            // Only restore if ours is still the active handler — never clobber
            // one installed after us.
            if (::sigaction(kSignals[i], nullptr, &current) == 0 &&
                (current.sa_flags & SA_SIGINFO) != 0 &&
                current.sa_sigaction == &CrashHandler::signalHandler) {
                ::sigaction(kSignals[i], &prev_[i], nullptr);
            }
            installed_[i] = false;
        }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
        initialized_ = false;
    }

    /// Path of the crash log, or nullptr if it could not be opened.
    const char* logPath() const noexcept {
#ifdef _WIN32
        return file_ ? path_ : nullptr;
#else
        return fd_ >= 0 ? path_ : nullptr;
#endif
    }

    CrashHandler(const CrashHandler&)            = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

private:
    CrashHandler()  = default;
    ~CrashHandler() { shutdown(); }

#ifdef _WIN32
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

        // Hand the crash on to the filter that was there before us (Dusklight's
        // crash reporter); with none, let Windows handle it (WER / crash dialog).
        if (self.prevFilter_ != nullptr) {
            return self.prevFilter_(ep);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    FILE*                         file_       = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER  prevFilter_ = nullptr;

#else // POSIX

    static constexpr int kNumSignals = 5;
    static constexpr int kSignals[kNumSignals] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

    inline void openLogFd() noexcept {
        if (!detail::buildLogPath(path_, sizeof(path_), kCrashLogName)) {
            path_[0] = '\0';
            detail::appendPath(path_, sizeof(path_), kCrashLogName);
        }
        fd_ = ::open(path_, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }

    // -----------------------------------------------------------------------
    // Signal handler — async-signal-safe only: no stdio, no heap, no locks.
    // -----------------------------------------------------------------------
    static void signalHandler(int sig, siginfo_t* info, void* uctx) noexcept {
        const int savedErrno = errno;
        CrashHandler& self = instance();

        // A crash while writing the report (or on a second thread at the same
        // time) skips straight to the previous handler.
        if (!inHandler_.exchange(true)) {
            self.writeReport(sig, info, uctx);
        }

        errno = savedErrno;
        self.chain(sig, info, uctx);
    }

    void writeReport(int sig, siginfo_t* info, void* uctx) const noexcept {
        detail::SignalSafeWriter w(fd_ >= 0 ? fd_ : STDERR_FILENO);

        // -- Header ----------------------------------------------------------
        w.str("\n========================================\n HIVESHOCK CRASH REPORT\n ");
        detail::writeUtcTimestamp(w);
        w.str("\n========================================\n");

        // -- Signal info -----------------------------------------------------
        w.str("Signal    : "); w.dec(static_cast<uint64_t>(sig));
        w.str(" (");          w.str(detail::signalName(sig)); w.str(")\n");
        if (info != nullptr) {
            w.str("Code      : "); w.decSigned(info->si_code); w.ch('\n');
            w.str("Fault addr: "); w.hex(reinterpret_cast<uintptr_t>(info->si_addr)); w.ch('\n');
        }
        w.str("Tick      : "); w.dec(monotonicMs()); w.ch('\n');

        // -- Context pointers ------------------------------------------------
        const CrashContext& ctx = crashContext();
        w.str("\n--- Key State ---\nPlayer ptr : ");
        w.hex(reinterpret_cast<uintptr_t>(ctx.playerPtr));
        w.str("\nStage      : "); w.str(ctx.stageName); w.ch('\n');

        // -- Register dump ---------------------------------------------------
        detail::writeRegisters(w, uctx);

        // -- Command ring buffer dump ----------------------------------------
        w.str("\n--- Last "); w.dec(kCommandRingSize); w.str(" Commands ---\n");
        const CommandRing& ring = commandRing();
        const uint32_t headVal = ring.head.load(std::memory_order_relaxed);
        // Walk the ring from oldest to newest
        const uint32_t start = (headVal >= kCommandRingSize) ? (headVal - kCommandRingSize) : 0;
        for (uint32_t i = start; i < headVal; ++i) {
            const CommandRecord& rec = ring.entries[i % kCommandRingSize];
            w.str("  [");
            if (i % kCommandRingSize < 10) w.ch(' ');
            w.dec(i % kCommandRingSize);
            w.str("] cmd=");     w.strPadded(rec.commandId, 40);
            w.str(" sender=");   w.strPadded(rec.sender, 20);
            w.str(" tick=");     w.dec(rec.tickCount);
            w.ch('\n');
        }

        w.str("========================================\n END CRASH REPORT\n"
              "========================================\n\n");
        w.flush();
    }

    // Hands the signal to whatever was installed before us (Dusklight's crash
    // reporter), or terminates with the default action if there was nothing.
    void chain(int sig, siginfo_t* info, void* uctx) const noexcept {
        int idx = -1;
        for (int i = 0; i < kNumSignals; ++i) {
            if (kSignals[i] == sig) { idx = i; break; }
        }

        if (idx >= 0 && installed_[idx]) {
            const struct sigaction prev = prev_[idx];
            // Restore it first, so a re-fault after we return goes straight to it.
            ::sigaction(sig, &prev, nullptr);

            if ((prev.sa_flags & SA_SIGINFO) != 0) {
                if (prev.sa_sigaction != nullptr) {
                    prev.sa_sigaction(sig, info, uctx);
                    return;
                }
            } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
                prev.sa_handler(sig);
                return;
            }
        }

        // Default disposition (SIG_IGN is treated the same: ignoring a fault
        // would just re-fault forever). The raised signal stays pending while
        // this handler runs and is delivered — terminating the process — as
        // soon as it returns.
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        ::sigemptyset(&dfl.sa_mask);
        ::sigaction(sig, &dfl, nullptr);
        ::raise(sig);
    }

    static inline std::atomic<bool> inHandler_{false};

    int              fd_ = -1;
    struct sigaction prev_[kNumSignals]{};
    bool             installed_[kNumSignals]{};

#endif // _WIN32

    char  path_[kPathBufSize]{};
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

    /// Path of the log file, or nullptr if it could not be opened.
    const char* path() const noexcept { return file_ ? path_ : nullptr; }

    ForensicLog(const ForensicLog&)            = delete;
    ForensicLog& operator=(const ForensicLog&) = delete;

private:
    ForensicLog() noexcept {
        file_ = detail::openLogFile(kForensicLogName, path_, sizeof(path_));
    }

    ~ForensicLog() {
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }

    FILE*       file_ = nullptr;
    char        path_[kPathBufSize]{};
    std::mutex  mutex_;
};

} // namespace hiveshock

// ---------------------------------------------------------------------------
// Convenience macros
// ---------------------------------------------------------------------------
#define HIVESHOCK_TRACE(fmt, ...) \
    ::hiveshock::ForensicLog::instance().trace(fmt, ##__VA_ARGS__)

#define HIVESHOCK_CHECKPOINT(label) \
    ::hiveshock::ForensicLog::instance().checkpoint(label)

#define HIVESHOCK_RECORD_CMD(commandId, senderName) \
    ::hiveshock::commandRing().record(commandId, senderName)
