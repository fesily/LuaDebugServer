#pragma once
#include "vm/vm.h"

// Compiling target source without running it.
//
// Used by breakpoint verification (conditions, hit conditions, and the chunk
// itself) so the debugger sees exactly the syntax and Protos the debuggee would
// produce, with no second Lua implementation involved.

#include <string>
#include <string_view>

namespace luadebug::target {

    enum class CompileStatus {
        ok,
        error,
    };

    struct CompileResult {
        CompileStatus status = CompileStatus::ok;
        // Message rendered by the target compiler, empty on success.
        std::string message;
    };

    // Compiles `source` as a chunk and leaves the function on the stack when it
    // succeeds (the caller pops it). On failure the stack is unchanged.
    CompileResult compile_chunk(vm::State* L, std::string_view source, const char* chunkname);

    // Compiles and immediately discards: the shape verification needs.
    CompileResult compile_check(vm::State* L, std::string_view source, const char* chunkname);

}  // namespace luadebug::target
