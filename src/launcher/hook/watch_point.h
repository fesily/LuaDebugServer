#pragma once

#include <resolver/lua_delayload.h>

#include <string_view>

namespace luadebug::autoattach {
    struct watch_point {
        enum class type {
            common,
            luajit_global,
            luajit_jit,
            ret,
        };
        type listener;
        std::string_view funcname;
        // For `common` points: a cold entry (library setup) that can reveal a VM
        // that did not exist when the watch was installed. These stay armed after
        // the discovery window closes; the per-call points do not. Creation
        // returns use their own listener and are never released by the deadline.
        bool persistent = false;
        void* address   = nullptr;
        explicit operator bool() const;
        bool find_symbol(const lua::resolver& resolver);
    };
}
