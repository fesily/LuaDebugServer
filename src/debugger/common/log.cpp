#include "common/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace luadebug::debugger {

    namespace {

        const char* level_name(LogLevel level) {
            switch (level) {
            case LogLevel::trace:
                return "TRACE";
            case LogLevel::debug:
                return "DEBUG";
            case LogLevel::info:
                return "INFO";
            case LogLevel::warn:
                return "WARN";
            case LogLevel::error:
                return "ERROR";
            case LogLevel::fatal:
                return "FATAL";
            }
            return "INFO";
        }

        std::mutex& log_mutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::string timestamp() {
            using clock         = std::chrono::system_clock;
            auto now            = clock::now();
            auto ms             = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            std::time_t seconds = clock::to_time_t(now);
            std::tm local {};
#if defined(_WIN32)
            localtime_s(&local, &seconds);
#else
            localtime_r(&seconds, &local);
#endif
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(ms.count()));
            return buffer;
        }

    }  // namespace

    Log& Log::instance() {
        static Log logger;
        return logger;
    }

    void Log::set_file(std::string path) {
        std::lock_guard<std::mutex> guard(log_mutex());
        path_ = std::move(path);
    }

    void Log::set_level(LogLevel level) {
        std::lock_guard<std::mutex> guard(log_mutex());
        level_ = level;
    }

    void Log::set_level(std::string_view name) {
        LogLevel level = LogLevel::trace;
        if (name == "debug") {
            level = LogLevel::debug;
        } else if (name == "info") {
            level = LogLevel::info;
        } else if (name == "warn") {
            level = LogLevel::warn;
        } else if (name == "error") {
            level = LogLevel::error;
        } else if (name == "fatal") {
            level = LogLevel::fatal;
        }
        set_level(level);
    }

    void Log::flush_locked(LogLevel level, std::string_view message) {
        std::string line;
        line.reserve(message.size() + 64);
        line.append("[");
        line.append(timestamp());
        line.append("][");
        line.append(level_name(level));
        line.append("]");
        line.append(message.data(), message.size());
        line.append("\n");

        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);

        if (!path_.empty()) {
            if (auto file = std::fopen(path_.c_str(), "ab")) {
                std::fwrite(line.data(), 1, line.size(), file);
                std::fclose(file);
            }
        }
    }

    void Log::write(LogLevel level, std::string_view message) {
        std::lock_guard<std::mutex> guard(log_mutex());
        if (static_cast<int>(level) < static_cast<int>(level_)) {
            return;
        }
        flush_locked(level, message);
    }

    void Log::printf(LogLevel level, const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        int written = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        if (written < 0) {
            return;
        }
        std::string_view message(buffer, static_cast<std::size_t>(written) < sizeof(buffer) ? static_cast<std::size_t>(written) : sizeof(buffer) - 1);
        write(level, message);
    }

    namespace {
        void log_v(LogLevel level, const char* fmt, va_list args) {
            char buffer[2048];
            int written = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
            if (written < 0) {
                return;
            }
            std::size_t size = static_cast<std::size_t>(written) < sizeof(buffer)
                                   ? static_cast<std::size_t>(written)
                                   : sizeof(buffer) - 1;
            Log::instance().write(level, std::string_view(buffer, size));
        }
    }  // namespace

    void log_info(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::info, fmt, args);
        va_end(args);
    }

    void log_warn(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::warn, fmt, args);
        va_end(args);
    }

    void log_error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::error, fmt, args);
        va_end(args);
    }

    void log_debug(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::debug, fmt, args);
        va_end(args);
    }

}  // namespace luadebug::debugger
