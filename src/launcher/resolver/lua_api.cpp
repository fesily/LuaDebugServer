#include <resolver/lua_api.h>

#include <resolver/lua_resolver.h>

namespace luadebug {
    template <typename T>
    static bool bind(const lua_resolver& resolver, T& fn, const char* name, std::string& missing) {
        fn = reinterpret_cast<T>(resolver.find(name));
        if (!fn) {
            missing = name;
            return false;
        }
        return true;
    }

    bool lua_api::resolve_pcall(const lua_resolver& resolver, std::string& missing) {
        pcallk = reinterpret_cast<decltype(pcallk)>(resolver.find("lua_pcallk"));
        if (pcallk) {
            return true;
        }
        pcall_plain = reinterpret_cast<decltype(pcall_plain)>(resolver.find("lua_pcall"));
        if (!pcall_plain) {
            missing = "lua_pcall";
            return false;
        }
        return true;
    }

    bool lua_api::resolve_loadbuffer(const lua_resolver& resolver, std::string& missing) {
        loadbufferx = reinterpret_cast<decltype(loadbufferx)>(resolver.find("luaL_loadbufferx"));
        if (loadbufferx) {
            return true;
        }
        loadbuffer_plain =
            reinterpret_cast<decltype(loadbuffer_plain)>(resolver.find("luaL_loadbuffer"));
        if (!loadbuffer_plain) {
            missing = "luaL_loadbuffer";
            return false;
        }
        return true;
    }

    bool lua_api::resolve(const lua_resolver& resolver, std::string& missing) {
        return bind(resolver, gettop, "lua_gettop", missing) && bind(resolver, settop, "lua_settop", missing) && bind(resolver, createtable, "lua_createtable", missing) && bind(resolver, tolstring, "lua_tolstring", missing) && bind(resolver, pushlstring, "lua_pushlstring", missing) && bind(resolver, pushstring, "lua_pushstring", missing) && bind(resolver, pushboolean, "lua_pushboolean", missing) && bind(resolver, setfield, "lua_setfield", missing) && resolve_pcall(resolver, missing) && resolve_loadbuffer(resolver, missing) && bind(resolver, sethook, "lua_sethook", missing) && bind(resolver, gethook, "lua_gethook", missing) && bind(resolver, gethookmask, "lua_gethookmask", missing) && bind(resolver, gethookcount, "lua_gethookcount", missing);
    }
}
