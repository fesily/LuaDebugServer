#pragma once

// Diagnostics sink. Debugger logging never writes to the DAP stdout stream:
// it goes to stderr and, when configured, an append-only file.

#include <cstdarg>
#include <string>
#include <string_view>

namespace luadebug::debugger {

    enum class LogLevel {
        trace = 0,
        debug,
        info,
        warn,
        error,
        fatal,
    };

    class Log {
    public:
        static Log& instance();

        void set_file(std::string path);
        void set_level(LogLevel level);
        void set_level(std::string_view name);

        void write(LogLevel level, std::string_view message);

        void printf(LogLevel level, const char* fmt, ...);

    private:
        Log() = default;
        void flush_locked(LogLevel level, std::string_view message);

        std::string path_;
        LogLevel level_ = LogLevel::trace;
    };

    void log_info(const char* fmt, ...);
    void log_warn(const char* fmt, ...);
    void log_error(const char* fmt, ...);
    void log_debug(const char* fmt, ...);

}  // namespace luadebug::debugger
