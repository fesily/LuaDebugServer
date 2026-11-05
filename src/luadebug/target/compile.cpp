#include "target/compile.h"

namespace luadebug::target {

    CompileResult compile_chunk(vm::State* L, std::string_view source, const char* chunkname) {
        CompileResult result;
        if (!L) {
            result.status  = CompileStatus::error;
            result.message = "no target state";
            return result;
        }
        const int top = L->gettop();
        if (L->loadbuffer(source.data(), source.size(), chunkname) != 0) {
            result.status  = CompileStatus::error;
            result.message = L->tostring(-1) ? L->tostring(-1) : "compile error";
            L->settop(top);
            return result;
        }
        return result;
    }

    CompileResult compile_check(vm::State* L, std::string_view source, const char* chunkname) {
        auto result = compile_chunk(L, source, chunkname);
        if (result.status == CompileStatus::ok) {
            L->pop(1);
        }
        return result;
    }

}  // namespace luadebug::target
