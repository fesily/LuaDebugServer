#pragma once

// The Lua C API a launcher hook needs, resolved *per module*.
//
// The old launcher kept one set of function pointers in a process-global table:
// the first module that answered "lua_newstate" won, and every later call used
// its symbols even for another VM. Here each `lua_module` owns its own table.

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace luadebug {
    struct lua_resolver;

    namespace lua {
        using state     = uintptr_t;
        using cfunction = int (*)(state);
        using debug     = uintptr_t;
        using hook      = void (*)(state, debug);
    }

    struct lua_api {
        using state = lua::state;
        using hook  = lua::hook;

        // Resolves what the launcher needs. Returns false and fills `missing`
        // with the first symbol the module does not export.
        bool resolve(const lua_resolver& resolver, std::string& missing);

    private:
        bool resolve_pcall(const lua_resolver& resolver, std::string& missing);
        bool resolve_loadbuffer(const lua_resolver& resolver, std::string& missing);
        bool resolve_pcall_placeholder();

    public:
        // stack
        int (*gettop)(state)                          = nullptr;
        void (*settop)(state, int)                    = nullptr;
        void (*createtable)(state, int, int)          = nullptr;
        const char* (*tolstring)(state, int, size_t*) = nullptr;
        // push / set
        void (*pushlstring)(state, const char*, size_t) = nullptr;
        void (*pushstring)(state, const char*)          = nullptr;
        void (*pushboolean)(state, int)                 = nullptr;
        void (*setfield)(state, int, const char*)       = nullptr;
        // calls. 5.4+ spells `lua_pcall` as a macro over `lua_pcallk`, so the
        // continuation form is what a module can actually export.
        int (*pcallk)(state, int, int, int, intptr_t, void*) = nullptr;
        int (*pcall_plain)(state, int, int, int)             = nullptr;
        // Likewise `luaL_loadbuffer` is a macro over `luaL_loadbufferx` since 5.2.
        int (*loadbufferx)(state, const char*, size_t, const char*, const char*) = nullptr;
        int (*loadbuffer_plain)(state, const char*, size_t, const char*)         = nullptr;

        int loadbuffer(state L, const char* buff, size_t size, const char* name) const {
            if (loadbufferx) {
                return loadbufferx(L, buff, size, name, nullptr);
            }
            return loadbuffer_plain(L, buff, size, name);
        }

        int pcall(state L, int nargs, int nresults, int errfunc) const {
            if (pcallk) {
                return pcallk(L, nargs, nresults, errfunc, 0, nullptr);
            }
            return pcall_plain(L, nargs, nresults, errfunc);
        }
        // hooks (per state, never one slot for the whole module)
        void (*sethook)(state, hook, int, int) = nullptr;
        hook (*gethook)(state, int*)           = nullptr;
        int (*gethookmask)(state)              = nullptr;
        int (*gethookcount)(state)             = nullptr;
    };
}
