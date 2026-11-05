#pragma once
namespace luadebug::vm {
    class State;
}

// Line information taken straight from the target's Proto.
//
// Replaces the Lua parser (backend/worker/parser.lua + undump.lua), which
// discovered lines by dumping the chunk to bytecode and "undumping" it with a
// bundled decoder that only understood 5.3-5.5. Here every version reads its
// own Proto layout, so 5.1/5.2/LuaJIT chunks get real line info too.
//
// Only plain data crosses this boundary: no Proto pointer is handed to the
// sharing runtime.

#include <map>
#include <string>
#include <vector>

namespace luadebug::target {

    struct FunctionLines {
        int start_line = 0;  // Proto.linedefined
        int end_line   = 0;  // Proto.lastlinedefined
        std::string key;     // "start-end"
        // Sorted, unique lines that can actually be stopped on.
        std::vector<int> active_lines;
    };

    class LineInfo {
    public:
        // Pre-order: index 0 is the chunk passed to build_line_info().
        const std::vector<FunctionLines>& functions() const {
            return functions_;
        }

        // Union of every function's active lines, sorted and unique.
        const std::vector<int>& active_lines() const {
            return active_lines_;
        }

        // "start-end" of the function that defines `line`, empty when unknown.
        std::string define_key(int line) const;

        bool is_active(int line) const;

        // parser.lua's nextActiveLine(): the first active line at or after
        // `line` that still belongs to the same function, 0 when there is none
        // (a request past a function's range must not spill into the next one).
        int next_active_line(int line) const;

        // parser.lua's normalize(): per line, the line a breakpoint on it really
        // lands on (0 = not remappable).
        int remap(int line) const {
            return next_active_line(line);
        }

        int max_line() const {
            return max_line_;
        }

        void add_function(FunctionLines lines);
        void finalize();

    private:
        std::vector<FunctionLines> functions_;
        std::vector<int> active_lines_;
        std::map<int, std::string> define_lines_;
        int max_line_ = 0;
    };

    // `func_index` is an absolute stack index holding a Lua function.
    // Returns false with `error` when the value is not a Lua function or the
    // chunk carries no line information (stripped).
    bool build_line_info(vm::State* L, int func_index, LineInfo& out, std::string& error);

    // Compiles `source` without running it and returns its line info. The
    // function stays on the stack while `keep_on_stack` is set, so the caller
    // can pop it after reading the bytecode; on failure nothing is left behind.
    bool build_line_info_from_source(vm::State* L, std::string_view source, LineInfo& out, std::string& error);

    // Per-instruction source line of the function at `func_index`: index = pc,
    // value = line (0 when unknown / stripped).
    bool function_lines(vm::State* L, int func_index, std::vector<int>& lines);

    // Opaque prototype handle for the disassembler (the concrete type is
    // per profile and stays inside the VM backends).
    void* detail_of_proto(vm::State* L, int func_index);

    // Per-instruction source lines of a prototype handle (0 = unknown).
    bool proto_lines(void* proto, std::vector<int>& lines);

}  // namespace luadebug::target
