#pragma once
#include <autoattach/autoattach.h>
#include <autoattach/lua_module.h>
#include <common/bootstrap.h>
#include <resolver/symbol_source.h>

#include <atomic>
#include <gumpp.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string>

namespace luadebug::autoattach {
    struct ctx {
        std::mutex mtx;
        // Written once under `mtx` before any hook exists, read lock-free from
        // fatal logging (which can run while the writer still holds `mtx`).
        std::atomic<work_mode> mode { work_mode::attach };
        // Every Lua module the process maps, in discovery order. A module whose
        // ABI/layout group is not the session's group is kept out of the session
        // but stays in the list (it may be the only candidate later).
        std::vector<std::unique_ptr<struct lua_module>> modules;
        // ABI/layout group the session is fixed to, chosen from the configured
        // Lua version or from the first module that joins. -1 until chosen.
        int selected_group = -1;
        // The launcher decided this process will not be debugged (several ABI
        // groups and no selection, or an unknown configured version). The decision
        // is final for the process: every later entry point must return without
        // re-running discovery, and no hook may stay installed.
        bool refused  = false;
        bool wait_dll = false;

        // Immutable bootstrap snapshot, published by the injection entry before
        // any hook is installed. Every VM (the first one and any later one) reads
        // this copy, never the entry's data buffer or the target environment.
        bool bootstrapped    = false;
        bool bootstrap_valid = false;
        debugger::BootstrapConfig bootstrap;
        // Non-empty when the selected carrier was rejected: hooks are not
        // installed and the session handshake fails with a bounded error.
        std::string bootstrap_error;

        // External VM description, resolved once from the bootstrap snapshot.
        // Every candidate and every later VM reuses this table; it is never
        // re-resolved per hook event, and it never changes after the snapshot.
        bool resolved_ready = false;
        resolved_symbols resolved;

        static ctx* get() {
            static ctx obj;
            return &obj;
        }
    };
}
