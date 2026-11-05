#pragma once

// Minimal C API shims the native tests need when they are compiled against an
// older bundled Lua (5.1/5.2). The product itself does this per profile inside
// the VM backends; the tests only need the few helpers they call directly.

#include <cstdio>
#include <string>

#include <lua.hpp>

#include "vm/api.h"
#include "vm/test_bind.h"

#if LUA_VERSION_NUM == 501

inline int lua_absindex(lua_State* L, int idx) {
    if (idx > LUA_REGISTRYINDEX && idx < 0)
        return lua_gettop(L) + idx + 1;
    return idx;
}

inline size_t lua_rawlen(lua_State* L, int idx) {
    return lua_objlen(L, idx);
}

inline void lua_pushglobaltable(lua_State* L) {
    lua_pushvalue(L, LUA_GLOBALSINDEX);
}

#endif

// The profile of the interpreter this test is compiled against.
inline luadebug::vm::Profile test_profile() {
#if defined(LUAJIT_VERSION)
    return luadebug::vm::Profile::luajit;
#elif LUA_VERSION_NUM == 501
    return luadebug::vm::Profile::lua51;
#elif LUA_VERSION_NUM == 502
    return luadebug::vm::Profile::lua52;
#elif LUA_VERSION_NUM == 503
    return luadebug::vm::Profile::lua53;
#elif LUA_VERSION_NUM == 504
    return luadebug::vm::Profile::lua54;
#else
    return luadebug::vm::Profile::lua55;
#endif
}

// Binds this binary (which embeds the interpreter) once and returns the state.
inline luadebug::vm::State* test_bind(lua_State* L) {
    static luadebug::vm::Vm* vm = nullptr;
    static std::string error;
    if (!vm) {
        auto binding = luadebug::vm::bind_test_module(
            L, test_profile(), reinterpret_cast<const void*>(&lua_gettop));
        if (!binding.vm) {
            error = binding.error;
            std::fprintf(stderr, "cannot bind the test module: %s\n", error.c_str());
            return nullptr;
        }
        vm = binding.vm;
    }
    return vm->state(L);
}
