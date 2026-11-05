// Verifies the target-side disassembler against the target's own Proto.
//
// The listing must cover every instruction, keep the old worker's textual
// shape (address / bytes / "OP  operands" / comment) and resolve constants out
// of the prototype the code actually belongs to.

#include <cstdio>
#include <cstring>
#include <string>

#include "lua_compat.h"
#include "target/disasm.h"
#include "vm/test_bind.h"

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    bool contains(const std::string& text, const char* needle) {
        return text.find(needle) != std::string::npos;
    }

    const luadebug::target::DisassembledInstruction* find_op(
        const luadebug::target::Disassembly& listing, const char* op) {
        for (const auto& inst : listing.instructions) {
            if (inst.op_name == op) {
                return &inst;
            }
        }
        return nullptr;
    }

} // namespace

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // The tests embed the interpreter directly, so the module is this binary.
    auto binding = luadebug::vm::bind_test_module(
        L, test_profile(), reinterpret_cast<const void*>(&lua_gettop));
    if (!binding.state) {
        std::fprintf(stderr, "FAIL: cannot bind the test module: %s\n", binding.error.c_str());
        lua_close(L);
        return 1;
    }
    luadebug::vm::State* S = binding.state;

    if (!luadebug::target::disassembly_supported(S)) {
        std::printf("disasm_test: unsupported\n");
        lua_close(L);
        return 0;
    }

    const char* source =
        "local a, b = ...\n"
        "local t = {'x', 1, 2}\n"
        "local n = 0\n"
        "for i = 1, #t do n = n + i end\n"
        "greet(t[n], n)\n"
        "return n\n";

    if (luaL_loadstring(L, source) != LUA_OK) {
        std::fprintf(stderr, "FAIL: load: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    luadebug::target::Disassembly listing;
    std::string error;
    expect(luadebug::target::disassemble_function(S, -1, listing, error),
           "the chunk disassembles");
    expect(error.empty(), "no error is reported");

    // Every instruction is present, addressed by pc, in order.
    expect(listing.instructions.size() > 10, "the listing is complete");
    bool addresses_ok = true;
    bool bytes_ok     = true;
    for (std::size_t i = 0; i < listing.instructions.size(); ++i) {
        const auto& inst = listing.instructions[i];
        char expected_address[16];
        std::snprintf(expected_address, sizeof(expected_address), "0x%08X",
                      static_cast<unsigned>(i));
        if (inst.address != expected_address || inst.pc != static_cast<int>(i)) {
            addresses_ok = false;
        }
        if (inst.bytes.size() != 8) {
            bytes_ok = false;
        }
    }
    expect(addresses_ok, "addresses follow the program counter");
    expect(bytes_ok, "instruction bytes are 8 hex digits");
    expect(!listing.instructions.back().op_name.empty(), "the last instruction is named");
    expect(listing.instructions.back().op_name.compare(0, 6, "RETURN") == 0,
           "the chunk ends in RETURN");

    // Opcodes whose operands the old listing rendered explicitly.
    const auto* loadk = find_op(listing, "LOADK");
    expect(loadk != nullptr, "LOADK is present");
    if (loadk) {
        expect(contains(loadk->comment, "'x'"), "LOADK resolves a string constant");
        expect(!loadk->operands.empty(), "LOADK renders its operands");
    }
    expect(find_op(listing, "NEWTABLE") != nullptr, "NEWTABLE is present");
    expect(find_op(listing, "FORPREP") != nullptr, "FORPREP is present");
    expect(find_op(listing, "FORLOOP") != nullptr, "FORLOOP is present");

    const auto* call = find_op(listing, "CALL");
    expect(call != nullptr, "CALL is present");
    if (call) {
        expect(contains(call->comment, " in "), "CALL reports its argument count");
    }
    if (const auto* forprep = find_op(listing, "FORPREP")) {
        expect(contains(forprep->comment, "exit to "), "FORPREP reports its exit target");
    }

    // Lines come from the prototype and stay inside the source.
    bool lines_ok = true;
    bool has_lines = false;
    for (const auto& inst : listing.instructions) {
        if (!inst.has_line) {
            continue;
        }
        has_lines = true;
        if (inst.line < 1 || inst.line > 6) {
            lines_ok = false;
        }
    }
    expect(has_lines, "instructions carry line numbers");
    expect(lines_ok, "line numbers stay inside the chunk");

    // A function value that is not Lua code is refused, not crashed on.
    lua_pushinteger(L, 42);
    {
        luadebug::target::Disassembly other;
        std::string other_error;
        expect(!luadebug::target::disassemble_function(S, -1, other, other_error),
               "a non-function is refused");
        expect(!other_error.empty(), "the refusal carries a message");
        expect(other.instructions.empty(), "the refusal leaves no instructions");
    }

    lua_settop(L, 0);
    lua_close(L);

    if (failures == 0) {
        std::printf("disasm_test: OK\n");
        return 0;
    }
    std::printf("disasm_test: %d failure(s)\n", failures);
    return 1;
}
