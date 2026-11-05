#include <autoattach/ctx.h>
#include <autoattach/lua_module.h>
#include <hook/watchdog.h>
#include <util/log.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luadebug::autoattach {
    static std::vector<watch_point> get_watch_points(work_mode mode, lua_version version) {
        if (mode == work_mode::launch) {
            // `lua_newstate` is the ideal moment (the state exists, no script has
            // run), but a launcher that is injected into an already spawned
            // process can arrive after it: the fast interpreters (LuaJIT) finish
            // their whole startup before the injection lands. The per-call points
            // below keep that case observable, and attaching is idempotent.
            auto points = get_watch_points(work_mode::attach, version);
            points.insert(points.begin(), { watch_point::type::ret, "lua_newstate" });
            return points;
        }
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
                { watch_point::type::common, "luaL_openlibs" },
                { watch_point::type::common, "lua_newthread" },
                { watch_point::type::common, "lua_close" },
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
        // The version list comes first (internal VM helpers fire most often), then
        // the exported C API. Both are one candidate *set*: an interpreter may
        // resolve some entries of a list and not others (LuaJIT keeps a dispatch
        // symbol name that resolves to unusable data and nothing else of `lj_*`),
        // so a partial hit must not stop the remaining candidates from counting.
        std::vector<watch_point> points = get_watch_points(mode, version);
        if (version != lua_version::unknown) {
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
