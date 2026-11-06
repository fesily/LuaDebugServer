#pragma once

// Target-side hook machinery: mask selection, per-prototype breakpoint
// enablement, single stepping, exception/thread hooks.
//
// This is the luadbg-free half of the old `rdebug_hookmgr.cpp`: it decides
// *when* the target traps and reports every event to a hook::Sink. The Lua
// binding that used to live here now implements that sink
// (src/luadebug/rdebug_hookmgr.cpp), so the same policy serves both the old Lua
// worker and the native WorkerSession.

#include <memory>

#include "hook/sink.h"
#include "vm/vm.h"

namespace luadebug::hook {

    class Core {
    public:
        // `sink` receives every hook event; it must outlive the Core.
        Core(vm::State* target, Sink* sink);
        ~Core();

        Core(const Core&)            = delete;
        Core& operator=(const Core&) = delete;

        bool valid() const;
        vm::State* target() const;

        // Breakpoints keyed by prototype.
        // Prototypes are opaque handles here: the target's Proto type differs
        // per Lua version and must not leak into this header.
        void break_add(vm::State* L, void* proto);
        void break_del(vm::State* L, void* proto);
        void break_freeobj(void* proto);
        void break_open(vm::State* L, bool enable);

        // Forgets every cached per-prototype classification. The classification
        // is derived from the breakpoint table, so it must not survive a change
        // to it.
        void break_reset();

        // False only for a prototype that was already classified as having no
        // breakpoint in its line range. Unclassified prototypes answer true: the
        // caller must take the full path, so a stale or missing decision can only
        // cost work, never a missed stop.
        bool proto_wants_line_events(void* proto) const;

        // Function breakpoints (call hook).
        void funcbp_open(vm::State* L, bool enable);

        // Single stepping.
        void step_in(vm::State* L);
        void step_over(vm::State* L);
        void step_out(vm::State* L);
        void step_cancel(vm::State* L);
        // Convenience for consumers that work in terms of StepMode.
        void step(StepMode mode);

        // Instruction breakpoints (count hook).
        void instbreak_open(vm::State* L, bool enable);

        // Periodic updates.
        void update_open(vm::State* L, bool enable);

        // Exception hooks, where the target supports them.
        void exception_open(vm::State* L, bool enable);

        // Coroutine tracking hooks, where the target supports them.
        void thread_open(vm::State* L, bool enable);

        // Recomputes the trap mask of one state (used after host switches).
        void updatehookmask(vm::State* hL);

        struct Impl;

    private:
        std::unique_ptr<Impl> impl_;
    };

}  // namespace luadebug::hook
