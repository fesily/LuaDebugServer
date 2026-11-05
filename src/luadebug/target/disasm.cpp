// Target-side bytecode access, routed through the VM binding.
//
// Prototype bytecode, per-instruction lines and the disassembler itself belong
// to a profile: 5.3-5.5 have an opcode table, 5.1/5.2 and LuaJIT do not. The
// shared side only names prototypes by opaque handles and asks the backend.

#include "target/disasm.h"

#include "vm/vm.h"

namespace luadebug::target {

    namespace {
        constexpr const char* kUnsupported = "Cannot dump function";
    }  // namespace

    int frame_pc(vm::State* L, int level) {
        if (!L) {
            return -1;
        }
        return L->frame_pc(level);
    }

    bool proto_identity_with(vm::State* L, void* proto, int& linedefined, int& lastlinedefined, std::string& source) {
        source.clear();
        if (!L || !proto) {
            return false;
        }
        std::string_view text;
        if (!L->vm()->api().proto_identity(proto, linedefined, lastlinedefined, text)) {
            return false;
        }
        source.assign(text);
        return true;
    }

    std::string instruction_reference_for(vm::State* L, void* proto, int pc) {
        if (!L || !proto) {
            return {};
        }
        int linedefined     = 0;
        int lastlinedefined = 0;
        std::string source;
        if (!proto_identity_with(L, proto, linedefined, lastlinedefined, source)) {
            return {};
        }
        return "bp_" + std::to_string(linedefined) + "_" + std::to_string(lastlinedefined) + "_" + std::to_string(pc) + "_" + source;
    }

    std::string instruction_reference(vm::State* L, int func_index, int pc) {
        if (!L) {
            return {};
        }
        return instruction_reference_for(L, L->proto_at(func_index), pc);
    }

    bool disassembly_supported(vm::State* L) {
        return L && L->vm()->api().disassembly_supported();
    }

    bool disassemble_function(vm::State* L, int func_index, Disassembly& out, std::string& error) {
        out.instructions.clear();
        if (!L) {
            error = "no target state";
            return false;
        }
        return L->vm()->api().disassemble_proto(L->proto_at(func_index), out, error);
    }

    bool disassemble_proto(vm::State* L, void* proto, Disassembly& out, std::string& error) {
        out.instructions.clear();
        if (!L || !proto) {
            error = kUnsupported;
            return false;
        }
        return L->vm()->api().disassemble_proto(proto, out, error);
    }

}  // namespace luadebug::target
