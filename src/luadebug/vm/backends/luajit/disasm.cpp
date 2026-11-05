// LuaJIT has no disassembler: the old worker's disassemble/instruction
// breakpoint support was 5.3+ only, and LuaJIT frames carry no instruction
// positions either. These entry points exist so the bridge links and the
// session can answer "unsupported" honestly.

#include "vm/backends/luajit/internal.h"

namespace luadebug::vm::LDB_NS {

    std::string instruction_reference_for(void* /*proto*/, int /*pc*/) {
        return {};
    }

    bool disassembly_supported() {
        return false;
    }

    bool disassemble_proto(void* /*proto*/, Disassembly& out, std::string& error) {
        out.instructions.clear();
        error = "Cannot dump function";
        return false;
    }

}  // namespace luadebug::vm::LDB_NS
