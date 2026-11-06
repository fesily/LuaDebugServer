#include <autoattach/ctx.h>
#include <autoattach/lua_module.h>
#include <hook/watchdog.h>
#include <util/log.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luadebug::autoattach {
    namespace {
        // The creation half of discovery, in both modes: every state begins at
        // these returns, so a state created while the watch is armed is seen
        // without a per-call hook. `luaL_newstate` is the portable creator: on
        // 64-bit LuaJIT without GC64 `lua_newstate` is a stub that prints an
        // error and returns NULL, and PUC hosts call `luaL_newstate` anyway (it
        // wraps `lua_newstate`).
        std::vector<watch_point> state_creation_points() {
            return {
                { watch_point::type::ret, "luaL_newstate" },
                { watch_point::type::ret, "lua_newstate" },
            };
        }

        // Launch-mode watch points, restricted to names every Lua build exports
        // (`LUA_API`/`LUALIB_API`).
        //
        // The internal helpers an attach session can use as an optimization
        // (`luaD_*`, `luaV_*`, `lj_*`) are `LUAI_FUNC`/`LJ_NOAPI`: hidden on ELF,
        // on Windows and everywhere in LuaJIT. A launch session cannot depend on
        // them, so it is built from exported API alone and works on every target.
        //
        // Two tiers, because launch mode has two timings:
        //
        //  * The launch carrier runs before `main`, so creation is the normal
        //    path and the returns below are the whole story.
        //  * When the Lua module appears later, the loader notification reaches
        //    us on `wait_module`'s worker thread (asynchronously, by design - the
        //    handler must not re-enter the loader), so the target can create the
        //    state and run the chunk before the watchdog is installed. The
        //    startup chain below covers exactly that window: open libraries,
        //    load, run. `lua_settop` is the one generic entry kept for hosts
        //    that drive the VM without a protected call.
        //
        // Every name the image does not export is skipped, so the spellings a
        // version does not have cost nothing.
        std::vector<watch_point> portable_watch_points() {
            std::vector<watch_point> points = state_creation_points();
            // Standard startup chain: open libraries, load a chunk, run it.
            // `lua_load` is the funnel of every `luaL_load*` in PUC Lua;
            // LuaJIT's `luaL_load*` call `lua_loadx` directly, so both are
            // listed. `lua_pcallk`/`lua_pcall` cover every protected call.
            for (auto& point : std::vector<watch_point> {
                     { watch_point::type::common, "luaL_openselectedlibs", true },  // 5.5
                     { watch_point::type::common, "luaL_openlibs", true },          // <= 5.4
                     { watch_point::type::common, "lua_loadx" },                    // LuaJIT
                     { watch_point::type::common, "lua_load" },
                     { watch_point::type::common, "lua_pcallk" },  // 5.2+
                     { watch_point::type::common, "lua_pcall" },   // 5.1, LuaJIT
                     // The generic host entry. Per-call points are released when
                     // the discovery window closes; creation and library setup
                     // stay armed (see `watchdog`).
                     { watch_point::type::common, "lua_settop" },
                 }) {
                points.push_back(point);
            }
            return points;
        }
    }  // namespace

    static std::vector<watch_point> version_watch_points(lua_version version);

    static std::vector<watch_point> get_watch_points(work_mode mode, lua_version version) {
        if (mode == work_mode::launch) {
            return portable_watch_points();
        }
        // Attach: the state being attached already exists, so creation alone sees
        // nothing; the per-call points below find it, and they also find states
        // that existed before the injection. Both modes release the whole watch
        // when the discovery window closes (see `watchdog`).
        std::vector<watch_point> points = state_creation_points();
        for (auto& point : version_watch_points(version)) {
            points.push_back(std::move(point));
        }
        return points;
    }

    static std::vector<watch_point> version_watch_points(lua_version version) {
        switch (version) {
        case lua_version::luajit:
            return {
                { watch_point::type::common, "lj_dispatch_ins" },
                { watch_point::type::common, "lj_dispatch_call" },
                { watch_point::type::common, "lj_vm_call" },
                { watch_point::type::common, "lj_vm_pcall" },
                { watch_point::type::common, "lj_vm_cpcall" },
                { watch_point::type::common, "lj_vm_resume" },
                { watch_point::type::common, "lj_meta_tget" },
                { watch_point::type::luajit_global, "lj_dispatch_update" },
                { watch_point::type::luajit_jit, "lj_dispatch_stitch" },
            };
        case lua_version::lua51:
            return {
                { watch_point::type::common, "luaD_precall" },
                { watch_point::type::common, "luaV_gettable" },
                { watch_point::type::common, "luaV_settable" },
            };
        case lua_version::lua54:
        case lua_version::lua55:
            return {
                { watch_point::type::common, "luaD_poscall" },
                { watch_point::type::common, "luaH_finishset" },
                { watch_point::type::common, "luaV_finishget" },
            };
        case lua_version::unknown:
        default:
            return {
                { watch_point::type::common, "lua_settop" },
                { watch_point::type::common, "luaL_openlibs", true },
                { watch_point::type::common, "lua_newthread" },
                { watch_point::type::common, "lua_type" },
                { watch_point::type::common, "lua_pushnil" },
                { watch_point::type::common, "lua_pushnumber" },
                { watch_point::type::common, "lua_pushlstring" },
                { watch_point::type::common, "lua_pushboolean" },
            };
        }
    }

    watchdog* create_watchdog(work_mode mode, const lua_module& module) {
        // One source of truth: the module's own detected ABI.
        const lua_version version = module.version;
        // The version is the module's own, so the watchdog, its watch points and the
        // profile handed to the bridge cannot disagree.
        auto context = std::make_unique<watchdog>(module.api, module.resolver, module.version);
        if (!context->init()) {
            return nullptr;
        }
        // An external table may name the launcher's two discovery points by role
        // (`lua_debug_server_newstate` / `lua_debug_server_call`) when the image exports no
        // symbol to point at. They replace the name lists entirely: one creation
        // entry (kept armed) and one per-call entry (released with the window).
        // The addresses belong to the selected image, so they only apply to it.
        const resolved_symbols& source = ctx::get()->resolved;
        const bool external_points     = source.has("lua_debug_server_newstate") && source.has("lua_debug_server_call") && source.module_base == module.memory_address;
        std::vector<watch_point> points;
        if (external_points) {
            points = {
                { watch_point::type::ret, "lua_debug_server_newstate", true },
                { watch_point::type::common, "lua_debug_server_call" },
            };
        } else {
            // The version list comes first (internal VM helpers fire most often),
            // then the exported C API. Both are one candidate *set*: an
            // interpreter may resolve some entries of a list and not others
            // (LuaJIT keeps a dispatch symbol name that resolves to unusable data
            // and nothing else of `lj_*`), so a partial hit must not stop the
            // remaining candidates from counting.
            // Launch mode is already the portable set and needs no second list.
            points = get_watch_points(mode, version);
        }
        if (!external_points && mode == work_mode::attach && version != lua_version::unknown) {
            for (auto& fallback : get_watch_points(mode, lua_version::unknown)) {
                bool present = false;
                for (const auto& existing : points) {
                    if (existing.funcname == fallback.funcname) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    points.push_back(std::move(fallback));
                }
            }
        }
        // `hook()` can fail per point (two distinct symbols resolving to one
        // address, for instance): reporting success there would leave the module
        // half-watched while the session believes it is attached.
        if (context->init_watch(std::move(points)) && context->hook()) {
            // TODO: fix other thread pc
            return context.release();
        }
        // TODO: more errmsg
        log::fatal("watchdog initialize failed");
        return nullptr;
    }
}
