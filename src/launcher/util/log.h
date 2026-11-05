#pragma once

// Launcher logging. Messages go to stderr (never to the DAP stdout) and, for
// fatal failures, to the frontend through the debuggee's session socket.
//
// Formatting is {fmt}: the launcher still runs on macOS 10.12, whose libc++ has
// no usable <format>/<print>, and nothing here may inject names into namespace
// std.

#include <fmt/format.h>

#include <string>
#include <utility>

#if !defined(NDEBUG)
#    define LUADEBUG_ENABLE_LOG 1
#endif

namespace luadebug::log {
    void init(bool attach);
    void notify_frontend(const std::string& msg);

    // Publishes the directory that holds `tmp/pid_<pid>` so a fatal error can be
    // reported to the frontend even when the launcher was loaded from a path dyld
    // does not know (a frida-mapped image). Called once, outside the launcher
    // context lock: notify_frontend() must never take that lock, because fatal
    // errors are raised from code that already holds it.
    void set_root(const std::string& root);

    template <typename... T>
    inline void error(fmt::format_string<T...> format, T&&... args) {
        fmt::print(stderr, "[lua-debug][launcher]{}\n", fmt::format(format, std::forward<T>(args)...));
    }

#if defined(LUADEBUG_ENABLE_LOG)
    template <typename... T>
    inline void info(fmt::format_string<T...> format, T&&... args) {
        error(format, std::forward<T>(args)...);
    }
#else
    template <typename... T>
    inline void info(fmt::format_string<T...>, T&&...) {
    }
#endif

    template <typename... T>
    inline void fatal(fmt::format_string<T...> format, T&&... args) {
        auto msg = fmt::format(format, std::forward<T>(args)...);
#if defined(LUADEBUG_ENABLE_LOG)
        error("{}", msg);
#endif
        notify_frontend(msg);
    }
}  // namespace luadebug::log
