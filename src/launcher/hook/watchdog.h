#pragma once

#include <autoattach/autoattach.h>
#include <hook/listener.h>
#include <hook/watch_point.h>
#include <resolver/lua_api.h>
#include <resolver/lua_resolver.h>

#include <gumpp.hpp>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace luadebug::autoattach {
    // Watches one Lua module: the interceptor points that tell us a state is in
    // use, plus the per-state hook bookkeeping.
    //
    // A module can serve any number of states, and every state's original hook,
    // mask and count belong to that state alone - never to one slot per module.
    struct watchdog {
    public:
        // `version` must be the module's own ABI: it is the profile the bridge
        // handoff needs, and it has to match the resolved `api`/`resolver`.
        watchdog(const lua_api& api, const lua_resolver& resolver, lua_version version);
        ~watchdog();
        watchdog(const watchdog&) = delete;

        bool init();
        bool init_watch(std::vector<watch_point>&& points);
        bool hook();
        void unhook();

        void watch_entry(lua::state L);
        void attach_lua(lua::state L, lua::debug ar);

        const lua_api& api() const {
            return api_;
        }
        const lua_resolver& resolver() const {
            return resolver_;
        }

        // Installs the pre-attach hook on one state (the C entry Lua calls).
        void install_luahook(lua::state L);

        struct hook_record {
            watchdog* owner = nullptr;
            // The target's own hook, captured exactly once - before anything of
            // ours is installed. Re-reading it later would adopt whatever the
            // debugger installed and then restore it with the wrong mask.
            lua::hook origin_hook = nullptr;
            int origin_mask       = 0;
            int origin_count      = 0;
            bool snapshot_taken   = false;
        };

        // True when the hook installed on `L` is ours (the pre-attach
        // trampoline). Anyone else's hook belongs to the debugger and must not
        // be replaced or restored.
        static bool owns_hook(const lua_api& api, lua::state L);

        // Lookup used by the C hook trampoline (which cannot capture anything).
        static hook_record* find_hook(lua::state L, bool erase);
        static int luahook_entry(lua::state L);

    private:
        const lua_api& api_;
        const lua_resolver& resolver_;
        // ABI of the module this watchdog belongs to: the bridge handoff needs it
        // to open the session with an explicit profile.
        lua_version version_;
        std::mutex mtx;
        std::vector<watch_point> watch_points;
        Gum::RefPtr<Gum::Interceptor> interceptor;
        common_listener listener_common;
        luajit_global_listener listener_luajit_global;
        luajit_jit_listener listener_luajit_jit;
        ret_listener listener_ret;
        std::vector<lua::state> state_hooked;
    };
}
