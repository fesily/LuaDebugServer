// LuaJIT Proto walker for line information.
//
// Mirrors lj_debug.c's `opt_L` construction: the line map is a packed array of
// 1/2/4-byte deltas added to `firstline`, and the last instruction (BC_RET) is
// not part of any active line. Child functions are found among the prototype's
// GC constants.

#include "vm/backends/luajit/internal.h"

extern "C" {
#include <lj_obj.h>
}

#include <algorithm>
#include <cstdint>

namespace luadebug::vm::LDB_NS {

    namespace {

        GCproto* proto_at_lua(const Raw* r, lua_State* L, int index) {
            index = (index > LUA_REGISTRYINDEX && index < 0) ? r->gettop(L) + index + 1 : index;
            if (r->type(L, index) != LUA_TFUNCTION || r->tocfunction(L, index) != nullptr) {
                return nullptr;
            }
            const TValue* value = L->top + (index - (r->gettop(L) + 1));
            if (!tvisfunc(value)) {
                return nullptr;
            }
            GCfunc* fn = funcV(value);
            if (!isluafunc(fn)) {
                return nullptr;
            }
            return funcproto(fn);
        }

        bool collect(const GCproto* pt, LineInfo& lines, int depth) {
            if (depth > 64) {
                return true;
            }
            FunctionLines function;
            function.start_line = pt->firstline;
            function.end_line   = static_cast<int>(pt->firstline + pt->numline);

            const void* lineinfo = proto_lineinfo(pt);
            if (lineinfo != nullptr) {
                const int width = pt->numline < 256 ? 1 : (pt->numline < 65536 ? 2 : 4);
                // Same bound as lua_getinfo("L"): the trailing BC_RET is skipped.
                const MSize count = pt->sizebc > 0 ? pt->sizebc - 1 : 0;
                for (MSize i = 0; i < count; ++i) {
                    int delta = 0;
                    switch (width) {
                    case 1:
                        delta = static_cast<const uint8_t*>(lineinfo)[i];
                        break;
                    case 2:
                        delta = static_cast<const uint16_t*>(lineinfo)[i];
                        break;
                    default:
                        delta = static_cast<int>(static_cast<const uint32_t*>(lineinfo)[i]);
                        break;
                    }
                    const int line = static_cast<int>(pt->firstline) + delta;
                    if (line > 0) {
                        function.active_lines.push_back(line);
                    }
                }
            }
            std::sort(function.active_lines.begin(), function.active_lines.end());
            function.active_lines.erase(std::unique(function.active_lines.begin(), function.active_lines.end()), function.active_lines.end());
            lines.add_function(std::move(function));

            // Child functions live among the collectable constants.
            if (pt->sizekgc > 0) {
                const GCRef* kr = mref(pt->k, GCRef) - static_cast<ptrdiff_t>(pt->sizekgc);
                for (MSize i = 0; i < pt->sizekgc; ++i) {
                    const GCobj* obj = gcref(kr[i]);
                    if (obj && obj->gch.gct == ~LJ_TPROTO) {
                        if (!collect(gco2pt(obj), lines, depth + 1)) {
                            return false;
                        }
                    }
                }
            }
            return true;
        }

    }  // namespace

    bool collect_proto_lines(const Raw* r, lua_State* L, int func_index, target::LineInfo& lines, std::string& error) {
        GCproto* proto = proto_at_lua(r, L, func_index);
        if (!proto) {
            error = "not a lua function";
            return false;
        }
        if (!collect(proto, lines, 0)) {
            error = "line info walk failed";
            return false;
        }
        if (lines.functions().empty()) {
            error = "empty chunk";
            return false;
        }
        return true;
    }

    // LuaJIT exposes no supported way to read a frame's bytecode position from
    // here, so instruction references stay unavailable for this target - exactly
    // as before. The disassembler and instruction breakpoints keep their
    // 5.3-5.5 matrix.
    bool proto_identity(void* proto_handle, int& linedefined, int& lastlinedefined, std::string_view& source) {
        const GCproto* proto = static_cast<const GCproto*>(proto_handle);
        if (!proto) {
            return false;
        }
        // LuaJIT stores only the first line of the function; the old worker's
        // instruction references carried that pair as (linedefined, 0).
        linedefined     = static_cast<int>(proto->firstline);
        lastlinedefined = 0;
        source          = std::string_view();
        // Nested prototypes inherit their chunk's name, so this is the same id
        // the old worker reported as the function's source.
        const GCstr* name = proto_chunkname(proto);
        if (name) {
            const char* text       = strdata(name);
            const std::size_t size = name->len;
            const std::size_t skip = size > 0 && text[0] == '@' ? 1 : 0;
            source                 = std::string_view(text + skip, size - skip);
        }
        return true;
    }

    bool proto_lines(void* proto_handle, std::vector<int>& lines) {
        lines.clear();
        const GCproto* proto = static_cast<const GCproto*>(proto_handle);
        if (!proto || proto->sizebc <= 0) {
            return false;
        }
        lines.resize(static_cast<std::size_t>(proto->sizebc), 0);
        const void* lineinfo = proto_lineinfo(proto);
        if (lineinfo == nullptr) {
            return true;
        }
        const int width = proto->numline < 256 ? 1 : (proto->numline < 65536 ? 2 : 4);
        for (MSize i = 0; i < proto->sizebc; ++i) {
            int delta = 0;
            switch (width) {
            case 1:
                delta = static_cast<const uint8_t*>(lineinfo)[i];
                break;
            case 2:
                delta = static_cast<const uint16_t*>(lineinfo)[i];
                break;
            default:
                delta = static_cast<int>(static_cast<const uint32_t*>(lineinfo)[i]);
                break;
            }
            lines[static_cast<std::size_t>(i)] = static_cast<int>(proto->firstline) + delta;
        }
        return true;
    }

}  // namespace luadebug::vm::LDB_NS
