// Standard Lua (5.1 - 5.5) Proto walker for line information.
//
// Two layouts exist:
//   * 5.4/5.5: `lineinfo` holds per-instruction deltas (ls_byte) with
//     ABSLINEINFO markers resolving to `abslineinfo` entries; OP_VARARGPREP is
//     the first instruction of a vararg function and must not become a line.
//   * 5.1 - 5.3: `lineinfo` holds absolute lines directly.
//
// The compressed decoder mirrors luaG_getfuncline() from the matching ldebug.c.

#include "vm/backends/lua5x/internal.h"

#include <algorithm>

extern "C" {
#include <lobject.h>
#include <lopcodes.h>
#include <lstate.h>

#if LUA_VERSION_NUM >= 504
#    include <ldebug.h>
#endif
}

namespace luadebug::vm::LDB_NS {

    namespace {

#if LUA_VERSION_NUM >= 504
        // Trailing opcodes that exist only in a given version; derived from the
        // version's own lopcodes.h rather than hard-coding "the last one".
        constexpr int kOpVarargPrep =
#    if LUA_VERSION_NUM >= 505
            83;
#    else
            81;
#    endif

        int base_line(const Proto* f, int pc, int* basepc) {
            if (f->sizeabslineinfo == 0 || pc < f->abslineinfo[0].pc) {
                *basepc = -1;
                return f->linedefined;
            }
            int i = static_cast<unsigned>(pc) / MAXIWTHABS - 1;
            while (i + 1 < f->sizeabslineinfo && pc >= f->abslineinfo[i + 1].pc) {
                ++i;
            }
            *basepc = f->abslineinfo[i].pc;
            return f->abslineinfo[i].line;
        }

        // luaG_getfuncline(): -1 when the chunk carries no line information.
        int funcline(const Proto* f, int pc) {
            if (!f->lineinfo) {
                return -1;
            }
            int basepc   = 0;
            int baseline = base_line(f, pc, &basepc);
            while (basepc++ < pc) {
                baseline += f->lineinfo[basepc];
            }
            return baseline;
        }
#endif

        bool collect(const Proto* f, LineInfo& lines, int depth) {
            if (depth > 64) {
                return true;  // pathological nesting: stop instead of recursing
            }
            FunctionLines function;
            function.start_line = f->linedefined;
            function.end_line   = f->lastlinedefined;

#if LUA_VERSION_NUM >= 505
            const bool is_vararg = isvararg(f) != 0;
#else
            const bool is_vararg = f->is_vararg != 0;
#endif

#if LUA_VERSION_NUM >= 504
            if (f->lineinfo == nullptr) {
                // Stripped chunk: no line info, and we must not invent any.
            } else {
                int start = 0;
                if (is_vararg && f->sizecode > 0) {
                    const Instruction first = f->code[0];
                    if (GET_OPCODE(first) == kOpVarargPrep) {
                        // VARARGPREP belongs to the function header, not to a
                        // stoppable line; 5.4/5.5 must not break there.
                        start = 1;
                    }
                }
                for (int pc = start; pc < f->sizelineinfo; ++pc) {
                    const int line = funcline(f, pc);
                    if (line > 0) {
                        function.active_lines.push_back(line);
                    }
                }
            }
#else
            if (f->lineinfo == nullptr) {
                // Stripped chunk.
            } else {
                for (int pc = 0; pc < f->sizelineinfo; ++pc) {
                    const int line = f->lineinfo[pc];
                    if (line > 0) {
                        function.active_lines.push_back(line);
                    }
                }
            }
#endif
            std::sort(function.active_lines.begin(), function.active_lines.end());
            function.active_lines.erase(std::unique(function.active_lines.begin(), function.active_lines.end()), function.active_lines.end());
            lines.add_function(std::move(function));

            for (int i = 0; i < f->sizep; ++i) {
                if (f->p[i]) {
                    if (!collect(f->p[i], lines, depth + 1)) {
                        return false;
                    }
                }
            }
            return true;
        }

    }  // namespace

    bool collect_proto_lines(const Raw* r, lua_State* L, int func_index, target::LineInfo& lines, std::string& error) {
        const int index = (func_index > LUA_REGISTRYINDEX && func_index < 0)
                              ? r->gettop(L) + func_index + 1
                              : real_index(func_index);
        if (r->type(L, index) != LUA_TFUNCTION || r->tocfunction(L, index) != nullptr) {
            error = "not a lua function";
            return false;
        }
        Proto* proto = static_cast<Proto*>(proto_at(r, L, index));
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

    bool proto_identity(void* proto_handle, int& linedefined, int& lastlinedefined, std::string_view& source) {
        const Proto* proto = static_cast<const Proto*>(proto_handle);
        if (!proto) {
            return false;
        }
        linedefined     = proto->linedefined;
        lastlinedefined = proto->lastlinedefined;
        source          = std::string_view();
        if (proto->source) {
            const char* text = getstr(proto->source);
#if LUA_VERSION_NUM >= 503
            const std::size_t size = tsslen(proto->source);
#else
            // 5.1 / 5.2 keep the size in TString.tsv.
            const std::size_t size = proto->source->tsv.len;
#endif
            // '@' marks a file name; '=' and raw chunk names are kept as-is,
            // exactly like the old worker's source ids.
            const std::size_t skip = size > 0 && text[0] == '@' ? 1 : 0;
            source                 = std::string_view(text + skip, size - skip);
        }
        return true;
    }

    bool proto_lines(void* proto_handle, std::vector<int>& lines) {
        lines.clear();
        const Proto* proto = static_cast<const Proto*>(proto_handle);
        if (!proto || proto->sizecode <= 0) {
            return false;
        }
        lines.resize(static_cast<std::size_t>(proto->sizecode), 0);
        if (proto->lineinfo == nullptr) {
            return true;  // stripped: every entry stays 0
        }
        for (int pc = 0; pc < proto->sizecode; ++pc) {
#if LUA_VERSION_NUM >= 504
            lines[static_cast<std::size_t>(pc)] = funcline(proto, pc);
#else
            lines[static_cast<std::size_t>(pc)] = proto->lineinfo[pc];
#endif
        }
        return true;
    }

}  // namespace luadebug::vm::LDB_NS
