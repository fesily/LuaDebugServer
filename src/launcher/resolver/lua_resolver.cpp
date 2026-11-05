#include <resolver/lua_resolver.h>

#include <gumpp.hpp>
#include <resolver/module_lookup.h>
#include <string>
#include <string_view>

namespace luadebug {
    static int (*_lua_pcall)(intptr_t L, int nargs, int nresults, int errfunc);
    static int _lua_pcallk(intptr_t L, int nargs, int nresults, int errfunc, intptr_t ctx, intptr_t k) {
        return _lua_pcall(L, nargs, nresults, errfunc);
    }
    static int (*_luaL_loadbuffer)(intptr_t L, const char* buff, size_t size, const char* name);
    static int _luaL_loadbufferx(intptr_t L, const char* buff, size_t size, const char* name, const char* mode) {
        return _luaL_loadbuffer(L, buff, size, name);
    }

    intptr_t lua_resolver::find_export(std::string_view name) const {
        // The symbol name is not NUL-terminated (it comes from a string_view), and
        // the C API takes a C string.
        const std::string symbol(name);
        intptr_t address = 0;
        with_module(module_name, [&](const Gum::Module& module) {
            address = (intptr_t)module.find_export_by_name(symbol.c_str());
        });
        return address;
    }

    intptr_t lua_resolver::find_symbol(std::string_view name) const {
        const std::string symbol(name);
        intptr_t address = 0;
        with_module(module_name, [&](const Gum::Module& module) {
            address = (intptr_t)module.find_symbol_by_name(symbol.c_str());
        });
        return address;
    }

    intptr_t lua_resolver::find(std::string_view name) const {
        using namespace std::string_view_literals;
        if (external) {
            if (const intptr_t hit = external->find(name)) {
                return hit;
            }
        }
        for (auto& finder : { &lua_resolver::find_export, &lua_resolver::find_symbol }) {
            if (auto result = (this->*finder)(name)) {
                return result;
            }
            if (name == "lua_pcallk"sv) {
                _lua_pcall = (decltype(_lua_pcall))(this->*finder)("lua_pcall"sv);
                if (_lua_pcall) {
                    return (intptr_t)_lua_pcallk;
                }
            } else if (name == "luaL_loadbufferx"sv) {
                _luaL_loadbuffer = (decltype(_luaL_loadbuffer))(this->*finder)("luaL_loadbuffer"sv);
                if (_luaL_loadbuffer) {
                    return (intptr_t)_luaL_loadbufferx;
                }
            }
        }
        return 0;
    }
}
