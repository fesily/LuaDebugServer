#pragma once

// Per-profile backend internals for the standard Lua 5.x family.
//
// This header is compiled once per version (5.1 - 5.5): the build puts that
// version's source directory on the include path, so `LUA_VERSION_NUM` and the
// internal headers (lstate.h, lobject.h, ...) describe the target layout. Every
// symbol is namespaced with `LDB_NS` (lua51 ... lua55), because all five copies
// live in the same bridge library.
//
// Calling the target's C API directly is impossible by construction: the
// backend only has the resolved `Raw` table, so no undefined `lua_*` symbol can
// leak into the product. Reading the target's internal layout is possible
// because the profile pins the version *and* the layout.

#include "vm/api.h"

#include "target/disasm.h"
#include "target/proto_view.h"

// The version's own headers. `lua.hpp` is not used: this file must not pull a
// C++ view that differs per version.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace luadebug::vm::LDB_NS {

    using target::DisassembledInstruction;
    using target::Disassembly;
    using target::FunctionLines;
    using target::LineInfo;

    // Sentinel indices are translated here, once, for every entry point that
    // takes a stack index.
    inline int real_index(int idx) {
        if (idx == idx::registry) {
            return LUA_REGISTRYINDEX;
        }
#if LUA_VERSION_NUM == 501
        if (idx == idx::globals) {
            return LUA_GLOBALSINDEX;
        }
        if (idx == idx::environ) {
            return LUA_ENVIRONINDEX;
        }
#else
        if (idx == idx::globals || idx == idx::environ) {
            // 5.2+ keep the globals in the registry; the caller that needs the
            // table pushed uses `push_globals`, not an index.
            return LUA_REGISTRYINDEX;
        }
#endif
        if (idx <= idx::environ && idx > idx::environ - 512) {
            // upvalue(n): idx::upvalue(n) = idx::environ - n. 5.1 numbers upvalues
            // down from LUA_GLOBALSINDEX, 5.2+ down from LUA_REGISTRYINDEX.
            const int n = idx::environ - idx;
#if LUA_VERSION_NUM == 501
            return LUA_GLOBALSINDEX - n;
#else
            return LUA_REGISTRYINDEX - n;
#endif
        }
        return idx;
    }

    inline bool is_globals(int idx) {
        return idx == idx::globals;
    }

    // Pushes the global table (the version's `_G` equivalent).
    void push_globals(const Raw* r, lua_State* L);

    // --- frames ----------------------------------------------------------
    void project(const lua_Debug& native, FrameInfo* out);
    const char* getlocal(const Raw* r, lua_State* L, lua_Debug* ar, int n);
    void hook_info(const Raw* r, const void* ar, HookInfo* out);

    // --- internal layout -------------------------------------------------
    int stacklevel(const Raw* r, lua_State* L);
    lua_State* main_thread(const Raw* r, lua_State* L);
    void* callinfo(const Raw* r, lua_State* L);
    void* ci2proto(const Raw* r, void* ci);
    void* debug2ci(const Raw* r, lua_State* L, const FrameInfo* ar);
    void* ci_previous(const Raw* r, void* ci);
    int hook_ci(const Raw* r, lua_State* L, const void* ar, void** out);
    int is_lua_func(const Raw* r, lua_State* L, const void* ar);
    const char* cdatatype(const Raw* r, lua_State* L, int idx);
    int exception_code(const Raw* r, lua_State* L, const void* ar);
    int allowhook_get(const Raw* r, lua_State* L);
    void allowhook_set(const Raw* r, lua_State* L, int v);

    // --- prototypes ------------------------------------------------------
    void* proto_at(const Raw* r, lua_State* L, int abs_index);
    bool collect_proto_lines(const Raw* r, lua_State* L, int func_index, target::LineInfo& lines, std::string& error);
    bool proto_identity(void* proto, int& linedefined, int& lastlinedefined, std::string_view& source);
    bool proto_lines(void* proto, std::vector<int>& lines);
    int frame_pc_of(const Raw* r, lua_State* L, int level);
    bool disassemble_proto(void* proto, target::Disassembly& out, std::string& error);
    std::string instruction_reference_for(void* proto, int pc);
    bool disassembly_supported();

    // --- table slots ------------------------------------------------------
    unsigned int table_array_size(const void* t);
    unsigned int table_hash_size(const void* t);
    bool table_array_base_zero();
    bool table_get_hash_kv(lua_State* L, const void* tv, unsigned int i);
    bool table_get_hash_k(lua_State* L, const void* tv, unsigned int i);
    bool table_get_hash_v(lua_State* L, const void* tv, unsigned int i);
    bool table_set_hash_v(lua_State* L, const void* tv, unsigned int i);
    bool table_get_array(lua_State* L, const void* tv, unsigned int i);
    bool table_set_array(lua_State* L, const void* tv, unsigned int i);

}  // namespace luadebug::vm::LDB_NS
