#pragma once

// ABI of a Lua implementation the launcher can attach to.
//
// Kept apart from `lua_module.h` so declarations that only need the enum (the
// watchdog, the attach entry point) do not pull the whole module type - which
// includes `autoattach.h` in turn.

namespace luadebug::autoattach {
    enum class lua_version {
        unknown,
        luajit,
        lua51,
        lua52,
        lua53,
        lua54,
        lua55,
    };

    // Id in the bridge's profile space (`vm::Profile`, also the session's group
    // id), or -1 for `unknown`.
    //
    // The two enums do NOT share a numeric layout (this one puts LuaJIT first and
    // has an `unknown` slot), so a `static_cast` between them silently selects the
    // wrong backend. Every crossing goes through this function.
    inline int lua_version_profile(lua_version version) {
        switch (version) {
        case lua_version::lua51:
            return 0;
        case lua_version::lua52:
            return 1;
        case lua_version::lua53:
            return 2;
        case lua_version::lua54:
            return 3;
        case lua_version::lua55:
            return 4;
        case lua_version::luajit:
            return 5;
        default:
            return -1;
        }
    }

    // Inverse of `lua_version_profile`, for callers that only have the id.
    inline lua_version lua_version_from_profile(int profile) {
        switch (profile) {
        case 0:
            return lua_version::lua51;
        case 1:
            return lua_version::lua52;
        case 2:
            return lua_version::lua53;
        case 3:
            return lua_version::lua54;
        case 4:
            return lua_version::lua55;
        case 5:
            return lua_version::luajit;
        default:
            return lua_version::unknown;
        }
    }
}  // namespace luadebug::autoattach
