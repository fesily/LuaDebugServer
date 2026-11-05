#pragma once

#include <autoattach/lua_version.h>
#include <resolver/lua_api.h>
#include <resolver/lua_delayload.h>

namespace luadebug::autoattach {
    enum class attach_status {
        success,
        fatal,
        wait,
    };
    enum class work_mode {
        launch,
        attach,
    };
    // `version` is the ABI of the module the hit belongs to. The bridge handoff
    // needs it because an explicit profile is a constraint the binding checks.
    attach_status attach_lua(lua::state L, const lua_api& api, lua_version version);
    void initialize(work_mode mode);

    // Selects, copies and validates the bootstrap carrier (frida's `data`
    // argument first, then LUA_DEBUG_CONFIG), publishes the snapshot, and then
    // installs hooks for the configured mode. On a configuration error nothing
    // is hooked: the failure is logged and reported to the session handshake.
    //
    // `data` is only read here: the caller's buffer is never retained, and later
    // VMs reuse the snapshot even if the buffer is overwritten or the
    // environment changes.
    void bootstrap(const char* data);

    // True once a bootstrap snapshot was published (the preload constructor and
    // the frida entry point can both be present in one process, and only the
    // first one may install hooks).
    bool already_bootstrapped();
}
