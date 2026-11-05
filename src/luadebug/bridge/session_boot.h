#pragma once

// Target-side session bootstrap: joins the runtime's session hub from inside the
// debuggee's process.
//
// One process may host several Lua states (a service-per-VM runtime such as
// skynet creates one per service), and each of them is a separate *worker* in the
// same session. The bridge therefore keeps one session per target state.
//
// A session is bound to exactly one VM group (one ABI/layout profile). States of
// another group may keep running in the same process, but they never join the
// session: they get an explicit conflict error, no hook, and no worker.

#include <string>
#include <vector>

#include "vm/vm.h"

namespace luadebug::bridge {

    struct BootOptions {
        // Session config (the adapter's initialize/launch payload) used when the
        // process is the one creating the session (standalone host, no adapter).
        std::string config_json;
        bool attach_only = false;
    };

    // Binds the runtime that sits next to this bridge, starts the session when
    // needed, registers `target` as a worker and installs its hooks. Must be
    // called on that state's thread. Idempotent per state.
    bool startup(vm::State* target, const BootOptions& options, std::string& error);

    // Leaves the session with `target`'s worker and stops serving its commands.
    void shutdown(vm::State* target);

    // Stops every worker this process registered (last-resort cleanup).
    void shutdown_all();

    bool running(vm::State* target);

    // Id of `target`'s worker in the runtime's registry (0 when not registered).
    unsigned long long worker_id(vm::State* target);

    // Drains queued commands for every worker registered by this process,
    // dispatching each on the thread that calls this. `wait_ms` bounds the whole
    // call; returns false once no worker is left.
    bool pump(unsigned wait_ms);

    // Pumps only `target`'s worker (used by a stop loop).
    bool pump_state(vm::State* target, unsigned wait_ms);

    // Group (ABI/layout profile) the session is fixed to, or -1 before the first
    // worker joins. Exposed so discovery/injection can filter candidates without
    // installing anything.
    int session_group();

    // True when `profile` may join the current session: no group chosen yet, or
    // it is the chosen one.
    bool group_compatible(vm::Profile profile);

}  // namespace luadebug::bridge
