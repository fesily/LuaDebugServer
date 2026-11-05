#pragma once
#include "vm/vm.h"

// Target-side error boundary.
//
// Every call that may raise a Lua error is routed through here so that:
//   * a Lua error (including OOM) becomes a status + message, never a longjmp
//     across a C++ frame that owns strings, vectors or locks,
//   * the target stack top is restored on every path,
//   * no C++ exception escapes into a Lua C callback (they are caught at the
//     trampoline and turned into a Lua error string).
//
// This replaces util/protected_area.h, which assumed a second (luadbg) VM on
// the other side of the boundary.

#include <string>

namespace luadebug::target {

    enum class CallStatus {
        ok,
        error,
    };

    struct CallResult {
        CallStatus status = CallStatus::ok;
        int nresults      = 0;
        // Error text as the target rendered it; empty on success.
        std::string message;
    };

    // Body of a protected call: returns the number of results it pushed.
    using protected_fn = int (*)(vm::State* L, void* user);

    // Restores the target stack top when it goes out of scope, including on
    // early return from a failed protected call.
    class StackGuard {
    public:
        explicit StackGuard(vm::State* L) noexcept;
        ~StackGuard();
        StackGuard(const StackGuard&)            = delete;
        StackGuard& operator=(const StackGuard&) = delete;

        int top() const noexcept {
            return top_;
        }

    private:
        vm::State* L_ = nullptr;
        int top_      = 0;
    };

    // Runs `fn` under a protected call with `nargs` arguments already pushed
    // (the top `nargs` values are consumed). Results are left on the stack with
    // `nresults` adjusted the way lua_pcall does it.
    CallResult pcall(vm::State* L, protected_fn fn, void* user, int nargs = 0, int nresults = 0);

}  // namespace luadebug::target
