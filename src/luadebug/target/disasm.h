#pragma once
namespace luadebug::vm {
    class State;
}

// Target-side bytecode access and disassembly.
//
// Both the stack frame's `instructionPointerReference` and the `disassemble`
// request address a bytecode position inside a specific Prototype; the listing
// is read straight from that Prototype (backend/worker/disassemble.lua worked on
// the undump representation instead).

#include <string>
#include <vector>

namespace luadebug::target {

    // Program counter of the innermost frame at `level`; -1 for frames without
    // Lua bytecode.
    int frame_pc(vm::State* L, int level);

    // Identity of a prototype handle: the parts shared by every instruction
    // reference of that function. The handle is profile-private, so the state
    // that produced it names the backend.
    bool proto_identity_with(vm::State* L, void* proto, int& linedefined, int& lastlinedefined, std::string& source);

    // Reference for a prototype handle at program counter `pc`.
    std::string instruction_reference_for(vm::State* L, void* proto, int pc);

    // Reference for the function at `func_index` at program counter `pc`:
    // "bp_<linedefined>_<lastlinedefined>_<pc>_<source>". The runtime prefixes
    // "inst_<threadId>x".
    std::string instruction_reference(vm::State* L, int func_index, int pc);

    // Named `DisassembledInstruction` because Lua's own headers already define
    // `Instruction` (the 32-bit encoded opcode).
    struct DisassembledInstruction {
        int pc = 0;
        std::string op_name;
        std::string operands;
        std::string comment;
        bool has_line = false;
        int line      = 0;
        std::string address;
        std::string bytes;
        bool invalid = false;  // EXTRAARG / VARARGPREP are not stoppable
        std::string symbol;
    };

    struct Disassembly {
        std::vector<DisassembledInstruction> instructions;
    };

    // True when this Lua version has a disassembler (the old matrix: 5.3-5.5).
    bool disassembly_supported(vm::State* L);

    // `func_index` is an absolute stack index holding a Lua function.
    bool disassemble_function(vm::State* L, int func_index, Disassembly& out, std::string& error);

    // Same, from a prototype handle (see proto_view.h's of_proto()).
    bool disassemble_proto(vm::State* L, void* proto, Disassembly& out, std::string& error);

}  // namespace luadebug::target
