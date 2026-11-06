#pragma once
#include "vm/vm.h"

// Hook events delivered to native consumers.
//
// The hook manager used to talk to the debugger VM by calling back into Lua
// (`push_callback` + `luadbg_pcall`). A sink replaces that: the bridge
// implements this interface with WorkerSession, so hook decisions are made in
// C++ on the target thread, with no second Lua state involved.
//
// Every callback runs inside the target's hook; implementations must not raise
// Lua errors and must keep the target stack balanced.

namespace luadebug::hook {

    enum class StepMode {
        none,
        over,
        in,
        out,
    };

    struct Sink {
        virtual ~Sink() = default;

        // A prototype is seen for the first time. `level` is the stack level of
        // the prototype's own frame: 0 for a call event, 1 when it just returned,
        // and the walked level when the whole stack is classified at attach.
        // Return true when the prototype needs line hooks.
        virtual bool new_proto(vm::State* L, void* proto, int level) = 0;

        // A call hook fired (function breakpoints).
        virtual void function_break(vm::State* L) = 0;

        // A count hook fired inside `proto` (instruction breakpoints).
        virtual void instruction_break(vm::State* L, void* proto) = 0;

        // The target raised an error; the error object is on top of `L`.
        virtual void exception(vm::State* L, int errcode) = 0;

        // A stop-worthy line. `stepping` is true when the line came from a
        // single-step request, false when it came from a breakpoint.
        virtual void line_stop(vm::State* L, bool stepping, int line, void* proto) = 0;

        // Periodic tick used to flush deferred updates.
        virtual void update(vm::State* L) = 0;
    };

}  // namespace luadebug::hook
