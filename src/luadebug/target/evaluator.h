#pragma once

// Target-side expression evaluation at a stop.
//
// This is the C++ replacement for backend/worker/eval/{readonly,readwrite}.lua:
// the same algorithm (declare the frame's live names as locals of a short
// wrapper chunk, compile it with the target's own compiler, bind the wrapper's
// upvalues to the *actual* values, run it) but without building user-facing
// machinery in Lua.
//
// Semantics kept from the Lua implementation:
//   * expression mode never writes back (readonly),
//   * statement mode writes back only the names the user's code directly
//     referenced, and only when the statement completed without error,
//   * `_ENV` / `getfenv` is the frame's environment, varargs are forwarded.

#include <functional>
#include <string>
#include <string_view>

#include "util/refvalue.h"

#include "target/protected.h"
#include "vm/vm.h"

namespace luadebug::target {

    enum class EvalMode {
        // `return <source>`: no side effects on bindings.
        expression,
        // `<source>` as statements, then write referenced bindings back.
        statement,
    };

    struct EvalOutcome {
        CallStatus status = CallStatus::ok;
        // Compile error or runtime error text; empty on success.
        std::string error;
        // Number of results left on the target stack (only meaningful on ok).
        int nresults = 0;
    };

    // Write-back sink for statement mode: the caller owns the value store and
    // assigns `value_index` (on `from`) into the given resolution path.
    using write_back_fn = std::function<bool(const refvalue::value* path, std::size_t path_size, vm::State* from, int value_index)>;

    class Evaluator {
    public:
        // `frame_level` is the *absolute* lua_getstack level of the frame the
        // user is looking at, measured in the caller's own stack context. The
        // evaluator inspects the frame before entering its protected trampoline,
        // so it never has to compensate for its own frames. Note that a suspended
        // coroutine's innermost frame is usually `coroutine.yield`, so the frame a
        // client calls "0" is not necessarily level 0.
        Evaluator(vm::State* target, int frame_level);

        // Installs the statement-mode write-back.
        void set_write_back(write_back_fn fn) {
            write_back_ = std::move(fn);
        }

        // Runs `source` in `mode`. Results stay on the target stack.
        EvalOutcome run(std::string_view source, EvalMode mode);

        // Convenience: expression mode, discards results except the first,
        // which is left on the stack.
        EvalOutcome evaluate(std::string_view source) {
            return run(source, EvalMode::expression);
        }

    private:
        vm::State* target_ = nullptr;
        int frame_level_   = 0;
        write_back_fn write_back_;
    };

}  // namespace luadebug::target
