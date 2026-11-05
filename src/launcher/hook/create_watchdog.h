#pragma once

#include <autoattach/autoattach.h>
#include <autoattach/lua_module.h>

#include <memory>

namespace luadebug::autoattach {
    struct watchdog;
    // The watchdog belongs to one module: it keeps that module's API and symbol
    // resolver, so a second Lua implementation in the process stays separate.
    // The ABI comes from the module itself, so the watchdog and the profile it
    // later hands to the bridge cannot disagree.
    watchdog* create_watchdog(work_mode mode, const lua_module& module);
}
