#include <autoattach/lua_module.h>

#include <autoattach/ctx.h>
#include <common/vm_version.h>
#include <resolver/lua_api.h>
#include <resolver/module_lookup.h>
#include <fmt/format.h>
#include <hook/create_watchdog.h>
#include <hook/watchdog.h>
#include <util/log.h>
#include <vm/mapping.h>

#include <charconv>
#include <gumpp.hpp>

namespace luadebug::autoattach {
    static lua_version lua_version_from_string [[maybe_unused]](const std::string_view& v) {
        if (v == "luajit")
            return lua_version::luajit;
        if (v == "lua51")
            return lua_version::lua51;
        if (v == "lua52")
            return lua_version::lua52;
        if (v == "lua53")
            return lua_version::lua53;
        if (v == "lua54")
            return lua_version::lua54;
        if (v == "lua55")
            return lua_version::lua55;
        return lua_version::unknown;
    }

    const char* lua_version_to_string(lua_version v) {
        switch (v) {
        case lua_version::lua51:
            return "lua51";
        case lua_version::lua52:
            return "lua52";
        case lua_version::lua53:
            return "lua53";
        case lua_version::lua54:
            return "lua54";
        case lua_version::lua55:
            return "lua55";
        case lua_version::luajit:
            return "luajit";
        default:
            return "unknown";
        }
    }

    static bool in_module(const lua_module& m, void* addr) {
        return addr > m.memory_address && addr <= (void*)((intptr_t)m.memory_address + m.memory_size);
    }

    static lua_version get_lua_version(const lua_module& m) {
        /*
            luaJIT_version_2_1_0_beta3
            luaJIT_version_2_1_0_beta2
            luaJIT_version_2_1_0_beta1
            luaJIT_version_2_1_0_alpha
        */
        for (void* addr : Gum::SymbolUtil::find_matching_functions("luaJIT_version_2_1_0*", true)) {
            if (in_module(m, addr))
                return lua_version::luajit;
        }
        // Not `find_symbol_by_name`: it returns whichever entry the module's
        // symbol list yields first, and a binary can carry an *undefined*
        // reference to the same name next to the definition (no address, type
        // GUM_SYMBOL_UNDEFINED). Interpreting that entry as a `const char*` gave
        // "unknown" for 5.1/5.3/5.5, whose symbol order differs from 5.2/5.4.
        void* p = nullptr;
        with_module(m.path, [&](const Gum::Module& module) {
            module.enumerate_symbols([&](const Gum::SymbolDetails& details) -> bool {
                if (!details.name || std::string_view(details.name) != "lua_ident") {
                    return true;
                }
                if (!details.address || details.type == Gum::SymbolType::UNDEFINED) {
                    return true;
                }
                p = details.address;
                return false;
            });
        });
        const char* lua_ident = (const char*)p;
        if (!lua_ident)
            return lua_version::unknown;
        auto id = std::string_view(lua_ident);
        using namespace std::string_view_literals;
        constexpr auto key = "$Lua"sv;
        if (id.substr(0, key.length()) != key) {
            return lua_version::unknown;
        }
        if (id[key.length()] == ':') {
            //$Lua: Lua 5.1.*
            return lua_version::lua51;
        }
        id = id.substr("$LuaVersion: Lua 5."sv.length());
        int patch;
        auto res = std::from_chars(id.data(), id.data() + 3, patch, 10);
        if (res.ec != std::errc()) {
            return lua_version::unknown;
        }
        switch (patch) {
        case 1:
            return lua_version::lua51;
        case 2:
            return lua_version::lua52;
        case 3:
            return lua_version::lua53;
        case 4:
            return lua_version::lua54;
        case 5:
            return lua_version::lua55;
        default:
            return lua_version::unknown;
        }
    }

    // Version from an externally supplied table. `lua_ident` is exact; otherwise
    // only positive markers are used, and a table that cannot single one version
    // out stays unknown so the caller has to name it.
    lua_version version_from_source(const lua_module& m, const resolved_symbols& source) {
        debugger::ProfileEvidence evidence;
        evidence.luajit     = source.has("luaJIT_setmode");
        evidence.lua55_only = source.has("lua_numbertocstring") || source.has("luaL_openselectedlibs");
        evidence.lua54_plus = source.has("lua_resetthread") || source.has("lua_newuserdatauv");
        evidence.lua53_plus = source.has("lua_isinteger");
        evidence.lua52_plus = source.has("lua_rawlen") || source.has("lua_callk") || source.has("luaL_loadbufferx");
        // An image's own symbol list is complete; a user table may be partial, and
        // then a missing 5.5 symbol says nothing about the version.
        evidence.complete = source.complete_from_image;

        int decided = debugger::kProfileUnknown;
        if (const auto ident = source.find("lua_ident")) {
            std::string text;
            if (vm::read_cstring(reinterpret_cast<const void*>(ident), 128, text)) {
                decided = debugger::profile_from_lua_ident(text);
            }
        }
        if (decided == debugger::kProfileUnknown) {
            decided = debugger::profile_from_evidence(evidence);
        }
        switch (decided) {
        case debugger::kProfileLua51:
            return lua_version::lua51;
        case debugger::kProfileLua52:
            return lua_version::lua52;
        case debugger::kProfileLua53:
            return lua_version::lua53;
        case debugger::kProfileLua54:
            return lua_version::lua54;
        case debugger::kProfileLua55:
            return lua_version::lua55;
        case debugger::kProfileLuajit:
            return lua_version::luajit;
        default:
            break;
        }
        (void)m;
        return lua_version::unknown;
    }

    int lua_module::group() const {
        return lua_version_profile(version);
    }

    bool lua_module::prepare() {
        auto context = ctx::get();
        if (context->resolved_ready) {
            // The external description belongs to the image it named. Applying it
            // to every candidate would make each of them report that image's
            // version (so no sibling group is ever skipped) and would mix two
            // modules' function pointers in one table. A source that names no image
            // describes the process, so its addresses apply to every candidate.
            const bool names_this_module = context->resolved.module_path.empty() || context->resolved.module_path == path;
            if (names_this_module) {
                resolver.external = &context->resolved;
            }
        }
        resolver.module_name = path;
        std::string missing;
        if (!api.resolve(resolver, missing)) {
            log::fatal("lua initialize failed, can't find {}", missing);
            return false;
        }
        version = resolver.external ? version_from_source(*this, *resolver.external)
                                    : get_lua_version(*this);
        log::info("module {}: lua version {}", path, lua_version_to_string(version));
        if (version == lua_version::unknown) {
            // The supplied table may be partial, which is why an explicit version
            // is the documented way to settle it. Only a *decided* contradiction is
            // refused - here, the caller's value is the constraint.
            const int configured = debugger::profile_from_name(context->bootstrap.lua_version);
            if (configured >= 0) {
                version = lua_version_from_profile(configured);
                log::info(
                    "module {}: version {} taken from the configured luaVersion (the "
                    "supplied evidence is not decisive)",
                    path, lua_version_to_string(version)
                );
            } else {
                // Binding a VM whose layout is unknown would call through the wrong
                // ABI, so this candidate is skipped. It is not a terminal decision:
                // another module in the same process may still be identifiable, and
                // `start_impl` refuses the process only when no candidate survives.
                log::error(
                    "module {}: cannot determine the Lua ABI from the supplied evidence; "
                    "an explicit luaVersion is required",
                    path
                );
                return false;
            }
        }
        // A module of another ABI group is a legitimate sibling, not a conflict
        // (a process may host several Lua versions): selection happens in
        // `start_impl`, where the requested group is known. Only "nothing matches
        // what was asked for" is refused there.
        return true;
    }

    bool lua_module::attach() {
        if (!prepared()) {
            return false;
        }
        if (watchdog) {
            return true;
        }
        watchdog = create_watchdog(mode, *this);
        return watchdog != nullptr;
    }

    lua_module::~lua_module() {
        if (watchdog) {
            delete watchdog;
        }
    }
}
