#pragma once
#include <cstdio>
#include <cstdarg>

namespace sp {

enum class LogLevel { Debug, Info, Warn, Error };

inline LogLevel g_log_level = LogLevel::Info;

inline void set_log_level(LogLevel level) { g_log_level = level; }

inline void log(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level < g_log_level) return;
    static const char* level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    fprintf(stderr, "[%s][%s] ", level_str[static_cast<int>(level)], tag);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

#define SP_LOGD(tag, ...) sp::log(sp::LogLevel::Debug, tag, __VA_ARGS__)
#define SP_LOGI(tag, ...) sp::log(sp::LogLevel::Info,  tag, __VA_ARGS__)
#define SP_LOGW(tag, ...) sp::log(sp::LogLevel::Warn,  tag, __VA_ARGS__)
#define SP_LOGE(tag, ...) sp::log(sp::LogLevel::Error, tag, __VA_ARGS__)

} // namespace sp
