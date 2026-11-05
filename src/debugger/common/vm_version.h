#pragma once

// Pure ABI-identity decisions, shared by every place that has to name a VM.
//
// Three call sites need the same answer - the launcher's candidates, the bridge's
// native descriptor path, and the adapter's validation of a user-supplied
// version - so the mapping lives here rather than once per binary. Nothing in
// this file touches target memory: callers read the evidence, this file decides.
//
// The numeric ids match `luadebug::vm::Profile` and the launcher's group ids.

#include <string_view>

namespace luadebug::debugger {

    enum : int {
        kProfileUnknown = -1,
        kProfileLua51   = 0,
        kProfileLua52   = 1,
        kProfileLua53   = 2,
        kProfileLua54   = 3,
        kProfileLua55   = 4,
        kProfileLuajit  = 5,
    };

    // "lua51".."lua55", "luajit". `lua-latest` maps to lua55 (its sources are
    // identical). `auto` and "" mean "unspecified" and return kProfileUnknown.
    int profile_from_name(std::string_view name);

    // Canonical name of an id, "unknown" for anything else.
    const char* profile_id_name(int profile);

    // Version string carried by the target's `lua_ident` data symbol:
    //   "$LuaVersion: Lua 5.4.6  Copyright (C) 1994-2023 Lua.org, PUC-Rio"
    //   "$Lua: Lua 5.1.4 ..."
    //   "$LuaJITVersion: LuaJIT 2.1.0-beta3"
    int profile_from_lua_ident(std::string_view ident);

    // Which versions each positive marker is compatible with. A marker proves
    // the VM is *at least* its version; a missing marker proves nothing, because
    // an external table may simply be incomplete.
    struct ProfileEvidence {
        // `luaJIT_setmode`: no stock Lua has it.
        bool luajit = false;
        // Symbols only 5.5 defines (`lua_numbertocstring`, `luaL_openselectedlibs`).
        bool lua55_only = false;
        // Symbols 5.4 introduced (`lua_resetthread`, `lua_newuserdatauv`).
        bool lua54_plus = false;
        // 5.3 (`lua_isinteger`, ...), 5.2 (`lua_rawlen`, `lua_callk`, ...).
        bool lua53_plus = false;
        bool lua52_plus = false;
        // True when the evidence comes from the image's own export/symbol list,
        // which enumerates its public API completely. Only then does a *missing*
        // marker carry information; a user-supplied table may be partial and an
        // absent 5.5 symbol says nothing about the version.
        bool complete = false;
    };

    // kProfileUnknown when the evidence does not single one version out. The
    // caller then has to ask for an explicit version: picking one of several
    // candidates and calling `lua_gettop` through the wrong layout corrupts the
    // target.
    int profile_from_evidence(const ProfileEvidence& evidence);

}  // namespace luadebug::debugger
