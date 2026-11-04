#pragma once

#include <mods/svc/log.h>
#include <cstdio>

namespace hiveshock {

template <typename... Args>
inline void logMessage(LogLevel level, const char* fmt_str, Args... args) {
    if constexpr (sizeof...(Args) == 0) {
        svc_log->write(mod_ctx, level, fmt_str);
    } else {
        char buf[1024];
        std::snprintf(buf, sizeof(buf), fmt_str, args...);
        svc_log->write(mod_ctx, level, buf);
    }
}

} // namespace hiveshock

#define DUSK_LOG_INFO(...)  ::hiveshock::logMessage(LOG_LEVEL_INFO, __VA_ARGS__)
#define DUSK_LOG_WARN(...)  ::hiveshock::logMessage(LOG_LEVEL_WARN, __VA_ARGS__)
#define DUSK_LOG_ERROR(...) ::hiveshock::logMessage(LOG_LEVEL_ERROR, __VA_ARGS__)
