#pragma once

// ABI vocabulary of the target Lua, without any function pointer.
//
// The launcher used to resolve one process-global set of `lua_*` pointers at the
// first module it found; `lua_api` now holds a per-module set, so two Lua
// implementations in one process cannot be confused.

#include <stdint.h>

#include <string_view>

namespace luadebug::lua {
    using state     = uintptr_t;
    using cfunction = int (*)(state);
    using debug     = uintptr_t;
    using hook      = void (*)(state, debug);

    // Hook mask bits: identical in every supported Lua version.
    enum : int {
        mask_call  = 1,
        mask_ret   = 2,
        mask_line  = 4,
        mask_count = 8,
    };

    struct resolver {
        virtual intptr_t find(std::string_view name) const = 0;
        // Same lookup without the compatibility aliases (`lua_pcallk` resolved
        // through a launcher-local thunk over `lua_pcall`): a watch point must
        // name a function the target itself exports, or it hooks the launcher.
        virtual intptr_t find_exact(std::string_view name) const {
            return find(name);
        }
    };
}
