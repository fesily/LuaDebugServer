#pragma once

#include <autoattach/autoattach.h>
#include <autoattach/lua_version.h>
#include <resolver/lua_api.h>
#include <resolver/lua_resolver.h>

#include <string>

namespace luadebug::autoattach {
    struct watchdog;
    const char* lua_version_to_string(lua_version v);
    struct lua_module;
    // Version decided from an externally supplied symbol table.
    lua_version version_from_source(const lua_module& m, const resolved_symbols& source);

    struct lua_module {
        std::string path;
        std::string name;
        void* memory_address = 0;
        size_t memory_size   = 0;
        lua_version version  = lua_version::unknown;
        lua_resolver resolver;
        // Resolved from *this* module only: the launcher never falls back to a
        // process-wide Lua symbol.
        lua_api api;
        work_mode mode;
        struct watchdog* watchdog = nullptr;
        // No OS module stands behind this candidate: it exists because an
        // external symbol table describes the VM, which may live in an image that
        // exports nothing and therefore cannot be discovered as a Lua module.
        bool external_only = false;

        lua_module(work_mode mode)
            : mode(mode) {}
        lua_module(const lua_module&)            = delete;
        lua_module& operator=(const lua_module&) = delete;

        lua_module(lua_module&& rhs) {
            swap(std::forward<lua_module>(rhs));
        };
        lua_module& operator=(lua_module&& rhs) {
            swap(std::forward<lua_module>(rhs));
            return *this;
        };

        void swap(lua_module&& rhs) {
            std::swap(external_only, rhs.external_only);
            std::swap(path, rhs.path);
            std::swap(name, rhs.name);
            std::swap(memory_address, rhs.memory_address);
            std::swap(memory_size, rhs.memory_size);
            std::swap(version, rhs.version);
            std::swap(api, rhs.api);
            std::swap(resolver, rhs.resolver);
            std::swap(mode, rhs.mode);
            std::swap(watchdog, rhs.watchdog);
        }

        // Resolves this module's own API and detects its version. No hook is
        // installed here: the session's group is chosen first.
        bool prepare();
        // Installs the watchdog for this module (after the group was accepted).
        bool attach();
        // ABI/layout group of this module (see vm::profile_group).
        int group() const;
        bool prepared() const {
            return version != lua_version::unknown && api.gettop != nullptr;
        }
        ~lua_module();
    };
}
