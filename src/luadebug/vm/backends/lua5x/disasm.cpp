// Standard Lua (5.3 - 5.5) disassembler over the real Proto.
//
// Opcode names, operand layouts and the produced text follow each version's
// lopcodes.h/.c and backend/worker/disassemble/lua5x.lua, so clients see the
// same instruction listing and comments as before.

#include "vm/backends/lua5x/internal.h"
#include "target/proto_view.h"

#if LUA_VERSION_NUM >= 503

#    include <lobject.h>
#    include <lopcodes.h>
#    include <lstate.h>

#    include <cstdio>
#    include <string>
#    include <vector>

namespace luadebug::vm::LDB_NS {

    namespace {

#    if LUA_VERSION_NUM >= 505
        const char* const kOpNames[] = {
            "MOVE",
            "LOADI",
            "LOADF",
            "LOADK",
            "LOADKX",
            "LOADFALSE",
            "LFALSESKIP",
            "LOADTRUE",
            "LOADNIL",
            "GETUPVAL",
            "SETUPVAL",
            "GETTABUP",
            "GETTABLE",
            "GETI",
            "GETFIELD",
            "SETTABUP",
            "SETTABLE",
            "SETI",
            "SETFIELD",
            "NEWTABLE",
            "SELF",
            "ADDI",
            "ADDK",
            "SUBK",
            "MULK",
            "MODK",
            "POWK",
            "DIVK",
            "IDIVK",
            "BANDK",
            "BORK",
            "BXORK",
            "SHLI",
            "SHRI",
            "ADD",
            "SUB",
            "MUL",
            "MOD",
            "POW",
            "DIV",
            "IDIV",
            "BAND",
            "BOR",
            "BXOR",
            "SHL",
            "SHR",
            "MMBIN",
            "MMBINI",
            "MMBINK",
            "UNM",
            "BNOT",
            "NOT",
            "LEN",
            "CONCAT",
            "CLOSE",
            "TBC",
            "JMP",
            "EQ",
            "LT",
            "LE",
            "EQK",
            "EQI",
            "LTI",
            "LEI",
            "GTI",
            "GEI",
            "TEST",
            "TESTSET",
            "CALL",
            "TAILCALL",
            "RETURN",
            "RETURN0",
            "RETURN1",
            "FORLOOP",
            "FORPREP",
            "TFORPREP",
            "TFORCALL",
            "TFORLOOP",
            "SETLIST",
            "CLOSURE",
            "VARARG",
            "GETVARG",
            "ERRNNIL",
            "VARARGPREP",
        };
#    elif LUA_VERSION_NUM == 503
        // 5.3's opcode order differs from 5.4's (no LOADI/LOADF, but LOADBOOL;
        // arithmetic operators share one slot instead of the K variants).
        const char* const kOpNames[] = {
            "MOVE",
            "LOADK",
            "LOADKX",
            "LOADBOOL",
            "LOADNIL",
            "GETUPVAL",
            "GETTABUP",
            "GETTABLE",
            "SETTABUP",
            "SETUPVAL",
            "SETTABLE",
            "NEWTABLE",
            "SELF",
            "ADD",
            "SUB",
            "MUL",
            "MOD",
            "POW",
            "DIV",
            "IDIV",
            "BAND",
            "BOR",
            "BXOR",
            "SHL",
            "SHR",
            "UNM",
            "BNOT",
            "NOT",
            "LEN",
            "CONCAT",
            "JMP",
            "EQ",
            "LT",
            "LE",
            "TEST",
            "TESTSET",
            "CALL",
            "TAILCALL",
            "RETURN",
            "FORLOOP",
            "FORPREP",
            "TFORCALL",
            "TFORLOOP",
            "SETLIST",
            "CLOSURE",
            "VARARG",
            "EXTRAARG",
        };
#    else
        const char* const kOpNames[] = {
            "MOVE",
            "LOADI",
            "LOADF",
            "LOADK",
            "LOADKX",
            "LOADFALSE",
            "LFALSESKIP",
            "LOADTRUE",
            "LOADNIL",
            "GETUPVAL",
            "SETUPVAL",
            "GETTABUP",
            "GETTABLE",
            "GETI",
            "GETFIELD",
            "SETTABUP",
            "SETTABLE",
            "SETI",
            "SETFIELD",
            "NEWTABLE",
            "SELF",
            "ADDI",
            "ADDK",
            "SUBK",
            "MULK",
            "MODK",
            "POWK",
            "DIVK",
            "IDIVK",
            "BANDK",
            "BORK",
            "BXORK",
            "SHRI",
            "SHLI",
            "ADD",
            "SUB",
            "MUL",
            "MOD",
            "POW",
            "DIV",
            "IDIV",
            "BAND",
            "BOR",
            "BXOR",
            "SHL",
            "SHR",
            "MMBIN",
            "MMBINI",
            "MMBINK",
            "UNM",
            "BNOT",
            "NOT",
            "LEN",
            "CONCAT",
            "CLOSE",
            "TBC",
            "JMP",
            "EQ",
            "LT",
            "LE",
            "EQK",
            "EQI",
            "LTI",
            "LEI",
            "GTI",
            "GEI",
            "TEST",
            "TESTSET",
            "CALL",
            "TAILCALL",
            "RETURN",
            "RETURN0",
            "RETURN1",
            "FORLOOP",
            "FORPREP",
            "TFORPREP",
            "TFORCALL",
            "TFORLOOP",
            "SETLIST",
            "CLOSURE",
            "VARARG",
            "VARARGPREP",
        };
#    endif

        const char* const kMetaMethods[] = {
            "__index",
            "__newindex",
            "__gc",
            "__mode",
            "__len",
            "__eq",
            "__add",
            "__sub",
            "__mul",
            "__mod",
            "__pow",
            "__div",
            "__idiv",
            "__band",
            "__bor",
            "__bxor",
            "__shl",
            "__shr",
            "__unm",
            "__bnot",
            "__lt",
            "__le",
            "__concat",
            "__call",
            "__close",
        };

        struct Fields {
            OpCode op = OP_MOVE;
            int A = 0, B = 0, C = 0, k = 0;
            int Bx = 0, sBx = 0, Ax = 0, sJ = 0, sB = 0, sC = 0;
        };

        Fields decode(Instruction i) {
            Fields f;
            f.op  = GET_OPCODE(i);
            f.A   = GETARG_A(i);
            f.B   = GETARG_B(i);
            f.C   = GETARG_C(i);
            f.Bx  = GETARG_Bx(i);
            f.sBx = GETARG_sBx(i);
            f.Ax  = GETARG_Ax(i);
#    if LUA_VERSION_NUM >= 504
            // 5.4 introduced the k bit and the signed short operands.
            f.k  = GETARG_k(i);
            f.sJ = GETARG_sJ(i);
            f.sB = GETARG_sB(i);
            f.sC = GETARG_sC(i);
#    endif
            return f;
        }

        const char* op_name(OpCode op) {
            const auto index = static_cast<std::size_t>(op);
            const auto count = sizeof(kOpNames) / sizeof(kOpNames[0]);
            return index < count ? kOpNames[index] : nullptr;
        }

        std::string num(int value) {
            return std::to_string(value);
        }

        std::string spaces(int a, int b, int c) {
            return num(a) + " " + num(b) + " " + num(c);
        }

        std::string quote(const char* data, std::size_t size) {
            std::string out = "'";
            for (std::size_t i = 0; i < size; ++i) {
                const char c = data[i];
                if (c == '\'' || c == '\\') {
                    out.push_back('\\');
                }
                out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }

        std::string constant_text(const TValue* value) {
            if (!value) {
                return {};
            }
            if (ttisstring(value)) {
                const TString* str = tsvalue(value);
                return quote(getstr(str), tsslen(str));
            }
            if (ttisnil(value)) {
                return "nil";
            }
            if (ttisboolean(value)) {
#    if LUA_VERSION_NUM >= 504
                return ttistrue(value) ? "true" : "false";
#    else
                return bvalue(value) ? "true" : "false";
#    endif
            }
            if (ttisinteger(value)) {
                return std::to_string(static_cast<long long>(ivalue(value)));
            }
            if (ttisnumber(value)) {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%.14g", static_cast<double>(fltvalue(value)));
                return buffer;
            }
            return {};
        }

        const TValue* constant_at(const Proto* proto, int index) {
            if (!proto || index < 0 || index >= proto->sizek) {
                return nullptr;
            }
            return &proto->k[index];
        }

        std::string upvalue_name(const Proto* proto, int index) {
            if (!proto || index < 0 || index >= proto->sizeupvalues) {
                return {};
            }
            const TString* name = proto->upvalues[index].name;
            if (!name) {
                return {};
            }
            return std::string(getstr(name), tsslen(name));
        }

#    if LUA_VERSION_NUM == 503
        std::string format_operands(const Fields& d, const char* name, int pc) {
            const std::string op(name);
            (void)pc;
            if (op == "MOVE" || op == "GETUPVAL" || op == "SETUPVAL" || op == "UNM" || op == "BNOT" || op == "NOT" || op == "LEN") {
                return num(d.A) + " " + num(d.B);
            }
            if (op == "LOADK") {
                return num(d.A) + " " + num(d.Bx);
            }
            if (op == "LOADKX" || op == "EXTRAARG") {
                return op == "EXTRAARG" ? num(d.Ax) : num(d.A);
            }
            if (op == "LOADBOOL") {
                return spaces(d.A, d.B, d.C);
            }
            if (op == "LOADNIL" || op == "RETURN" || op == "VARARG") {
                return num(d.A) + " " + num(d.B);
            }
            if (op == "JMP" || op == "FORLOOP" || op == "FORPREP" || op == "TFORLOOP") {
                return num(d.A) + " " + num(d.sBx);
            }
            if (op == "TFORCALL") {
                return num(d.A) + " " + num(d.C);
            }
            if (op == "CLOSURE") {
                return num(d.A) + " " + num(d.Bx);
            }
            if (op == "TEST") {
                return num(d.A) + " " + num(d.C);
            }
            return spaces(d.A, d.B, d.C);
        }

        std::string comment(const Fields& d, const char* name, const Proto* proto, int pc, int extra_ax) {
            const std::string op(name);
            if (op == "LOADK") {
                return constant_text(constant_at(proto, d.Bx));
            }
            if (op == "LOADKX") {
                return extra_ax >= 0 ? constant_text(constant_at(proto, extra_ax)) : std::string();
            }
            if (op == "EXTRAARG") {
                return "Ax=" + num(d.Ax);
            }
            if (op == "CLOSURE") {
                if (proto && d.Bx >= 0 && d.Bx < proto->sizep && proto->p[d.Bx]) {
                    const Proto* child = proto->p[d.Bx];
                    std::string params;
                    for (int i = 0; i < child->numparams; ++i) {
                        if (i) {
                            params += ", ";
                        }
                        const TString* varname =
                            (child->locvars && i < child->sizelocvars) ? child->locvars[i].varname
                                                                       : nullptr;
                        if (varname) {
                            params += std::string(getstr(varname), tsslen(varname));
                        } else {
                            params += "param_" + num(i + 1);
                        }
                    }
                    if (child->is_vararg != 0) {
                        params += params.empty() ? "..." : ", ...";
                    }
                    return "function function_" + num(d.Bx) + "(" + params + ")";
                }
                return {};
            }
            if (op == "CALL" || op == "TAILCALL") {
                const int nargs    = d.B > 0 ? d.B - 1 : 0;
                const int nresults = d.C > 0 ? d.C - 1 : 0;
                return num(nargs) + " in " + num(nresults) + " out";
            }
            if (op == "RETURN") {
                return num(d.B > 0 ? d.B - 1 : 0) + " out";
            }
            if (op == "JMP" || op == "FORLOOP" || op == "TFORLOOP") {
                return "to " + num(pc + d.sBx);
            }
            if (op == "FORPREP") {
                return "exit to " + num(pc + d.sBx);
            }
            return {};
        }

        std::string symbol(const Proto* proto, int pc, const Fields& d) {
            const std::string op(kOpNames[static_cast<std::size_t>(d.op)]);
            if (op != "CALL" && op != "TAILCALL") {
                return {};
            }
            for (int step = 1; step <= 4 && step <= pc; ++step) {
                const Instruction prev_ins = proto->code[pc - step];
                const Fields prev          = decode(prev_ins);
                if (prev.A != d.A) {
                    continue;
                }
                const std::string prev_op(kOpNames[static_cast<std::size_t>(prev.op)]);
                if (prev_op == "GETTABUP") {
                    std::string text = upvalue_name(proto, prev.B);
                    if (text.empty()) {
                        text = "_UPV[" + num(prev.B) + "]";
                    }
                    const TValue* key = constant_at(proto, prev.C);
                    text += "." + (key ? constant_text(key) : std::string("?"));
                    return text;
                }
                if (prev_op == "GETTABLE" || prev_op == "SELF") {
                    const TValue* key = constant_at(proto, prev.C);
                    return key ? constant_text(key) : std::string();
                }
                if (prev_op == "LOADK") {
                    const TValue* value = constant_at(proto, prev.Bx);
                    return value ? constant_text(value) : std::string();
                }
                if (prev_op == "CLOSURE") {
                    return "closure_" + num(prev.Bx);
                }
                break;
            }
            return {};
        }
#    else
        std::string format_operands(const Fields& d, const char* name, int pc) {
            const std::string op(name);
            if (op == "MOVE" || op == "GETUPVAL" || op == "SETUPVAL" || op == "UNM" || op == "BNOT" || op == "NOT" || op == "LEN" || op == "CLOSE" || op == "TBC") {
                return num(d.A) + " " + num(d.B);
            }
            if (op == "LOADI" || op == "LOADF") {
                return num(d.A) + " " + num(d.sBx);
            }
            if (op == "LOADK" || op == "CLOSURE") {
                return num(d.A) + " " + num(d.Bx);
            }
            if (op == "LOADKX" || op == "LOADFALSE" || op == "LFALSESKIP" || op == "LOADTRUE" || op == "VARARGPREP") {
                return num(d.A);
            }
            if (op == "LOADNIL") {
                return num(d.A) + " " + num(d.B);
            }
            if (op == "GETTABUP" || op == "SETTABUP" || op == "GETTABLE" || op == "SETTABLE" || op == "GETI" || op == "SETI" || op == "GETFIELD" || op == "SETFIELD" || op == "SELF" || op == "NEWTABLE") {
                return spaces(d.A, d.B, d.C);
            }
            if (op == "ADD" || op == "SUB" || op == "MUL" || op == "MOD" || op == "POW" || op == "DIV" || op == "IDIV" || op == "BAND" || op == "BOR" || op == "BXOR" || op == "SHL" || op == "SHR" || op == "ADDK" || op == "SUBK" || op == "MULK" || op == "MODK" || op == "POWK" || op == "DIVK" || op == "IDIVK") {
                return spaces(d.A, d.B, d.C);
            }
            if (op == "ADDI" || op == "SHRI" || op == "SHLI") {
                return spaces(d.A, d.B, d.sC);
            }
            if (op == "BANDK" || op == "BORK" || op == "BXORK") {
                return num(d.A) + " " + num(d.C);
            }
            if (op == "EQ" || op == "LT" || op == "LE") {
                return spaces(d.A, d.B, d.C) + " " + num(d.k);
            }
            if (op == "EQK") {
                return spaces(d.A, d.B, d.k);
            }
            if (op == "EQI" || op == "LTI" || op == "LEI" || op == "GTI" || op == "GEI") {
                return num(d.A) + " " + num(d.sB);
            }
            if (op == "TEST") {
                return num(d.A) + " " + num(d.k);
            }
            if (op == "TESTSET") {
                return spaces(d.A, d.B, d.k);
            }
            if (op == "MMBIN" || op == "CALL" || op == "TAILCALL" || op == "MMBINK") {
                return spaces(d.A, d.B, d.C);
            }
            if (op == "MMBINI") {
                return num(d.A) + " " + num(d.sB) + " " + num(d.C) + " " + num(d.k);
            }
            if (op == "RETURN") {
                return num(d.A) + " " + num(d.B);
            }
            if (op == "RETURN0") {
                return {};
            }
            if (op == "RETURN1") {
                return num(d.A);
            }
            if (op == "JMP") {
                return num(d.sJ);
            }
            if (op == "FORLOOP" || op == "FORPREP" || op == "TFORPREP" || op == "TFORLOOP") {
                return num(d.A) + " " + num(d.Bx);
            }
            if (op == "TFORCALL") {
                return num(d.A) + " " + num(d.C);
            }
            if (op == "SETLIST") {
                return spaces(d.A, d.B, d.C);
            }
            if (op == "VARARG") {
                return num(d.A) + " " + num(d.C);
            }
            if (op == "CONCAT") {
                return num(d.A) + " " + num(d.B);
            }
            if (op == "EXTRAARG") {
                return num(d.Ax);
            }
            (void)pc;
            return spaces(d.A, d.B, d.C);
        }

        // Comments mirroring disassemble/lua5x.lua's comment().
        std::string comment(const Fields& d, const char* name, const Proto* proto, int pc, int extra_ax) {
            const std::string op(name);
            if (op == "GETTABUP" || op == "SETTABUP") {
                std::string text  = upvalue_name(proto, d.B);
                const TValue* key = constant_at(proto, d.C);
                if (key) {
                    text += " " + constant_text(key);
                }
                return text;
            }
            if (op == "LOADK") {
                return constant_text(constant_at(proto, d.Bx));
            }
            if (op == "LOADKX") {
                return extra_ax >= 0 ? constant_text(constant_at(proto, extra_ax)) : std::string();
            }
            if (op == "EXTRAARG") {
                return "Ax=" + num(d.Ax);
            }
            if (op == "CLOSURE") {
                if (proto && d.Bx >= 0 && d.Bx < proto->sizep && proto->p[d.Bx]) {
                    const Proto* child = proto->p[d.Bx];
                    std::string params;
                    for (int i = 0; i < child->numparams; ++i) {
                        if (i) {
                            params += ", ";
                        }
#        if LUA_VERSION_NUM >= 504
                        const TString* lv = i < child->sizeupvalues && child->locvars
                                                ? nullptr
                                                : nullptr;
                        (void)lv;
                        params += "param_" + num(i + 1);
#        else
                        params += "param_" + num(i + 1);
#        endif
                    }
#        if LUA_VERSION_NUM >= 505
                    const bool vararg = isvararg(child) != 0;
#        else
                    const bool vararg = child->is_vararg != 0;
#        endif
                    if (vararg) {
                        params += params.empty() ? "..." : ", ...";
                    }
                    return "function function_" + num(d.Bx) + "(" + params + ")";
                }
                return {};
            }
            if (op == "CALL" || op == "TAILCALL") {
                const int nargs    = d.B > 0 ? d.B - 1 : 0;
                const int nresults = d.C > 0 ? d.C - 1 : 0;
                return num(nargs) + " in " + num(nresults) + " out";
            }
            if (op == "RETURN") {
                return num(d.B > 0 ? d.B - 1 : 0) + " out";
            }
            if (op == "MMBIN") {
                const auto index = static_cast<std::size_t>(d.C);
                const auto count = sizeof(kMetaMethods) / sizeof(kMetaMethods[0]);
                return index < count ? kMetaMethods[index] : "";
            }
            if (op == "MMBINI" || op == "MMBINK") {
                const auto index = static_cast<std::size_t>(d.C);
                const auto count = sizeof(kMetaMethods) / sizeof(kMetaMethods[0]);
                std::string text = index < count ? kMetaMethods[index] : "";
                if (d.k != 0) {
                    text += " flip";
                }
                return text;
            }
            if (op == "JMP") {
                return "to " + num(pc + d.sJ);
            }
            if (op == "FORLOOP") {
                return "to " + num(pc - d.Bx);
            }
            if (op == "FORPREP") {
                return "exit to " + num(pc + d.Bx + 1);
            }
            if (op == "TFORPREP") {
                return "to " + num(pc + d.Bx);
            }
            if (op == "TFORLOOP") {
                return "to " + num(pc - d.Bx);
            }
            return {};
        }

        // Best-effort name for the callee of CALL/TAILCALL, from the instruction
        // that loaded the register.
        std::string symbol(const Proto* proto, int pc, const Fields& d) {
            const std::string op(kOpNames[static_cast<std::size_t>(d.op)]);
            if (op != "CALL" && op != "TAILCALL") {
                return {};
            }
            for (int step = 1; step <= 4 && step <= pc; ++step) {
                const Instruction prev_ins = proto->code[pc - step];
                const Fields prev          = decode(prev_ins);
                if (prev.A != d.A) {
                    continue;
                }
                const std::string prev_op(kOpNames[static_cast<std::size_t>(prev.op)]);
                if (prev_op == "GETTABUP") {
                    std::string text = upvalue_name(proto, prev.B);
                    if (text.empty()) {
                        text = "_UPV[" + num(prev.B) + "]";
                    }
                    const TValue* key = constant_at(proto, prev.C);
                    text += "." + (key ? constant_text(key) : std::string("?"));
                    return text;
                }
                if (prev_op == "GETFIELD" || prev_op == "SELF") {
                    const TValue* key = constant_at(proto, prev.C);
                    return key ? constant_text(key) : std::string();
                }
                if (prev_op == "LOADK") {
                    const TValue* value = constant_at(proto, prev.Bx);
                    return value ? constant_text(value) : std::string();
                }
                if (prev_op == "CLOSURE") {
                    return "closure_" + num(prev.Bx);
                }
                break;
            }
            return {};
        }

#    endif  // LUA_VERSION_NUM == 503

    }  // namespace

    std::string instruction_reference_for(void* proto, int pc) {
        int linedefined     = 0;
        int lastlinedefined = 0;
        std::string_view source;
        if (!proto_identity(proto, linedefined, lastlinedefined, source)) {
            return {};
        }
        return "bp_" + std::to_string(linedefined) + "_" + std::to_string(lastlinedefined) + "_" + std::to_string(pc) + "_" + std::string(source);
    }

    bool disassembly_supported() {
        return true;
    }

    bool disassemble_proto(void* proto_handle, Disassembly& out, std::string& error) {
        out.instructions.clear();
        Proto* proto = static_cast<Proto*>(proto_handle);
        if (!proto || !proto->code || proto->sizecode <= 0) {
            error = "Cannot dump function";
            return false;
        }

        std::vector<int> lines;
        proto_lines(proto, lines);

        // LOADKX takes its constant index from the following EXTRAARG.
        std::vector<int> extra_ax(static_cast<std::size_t>(proto->sizecode), -1);
        for (int pc = 0; pc + 1 < proto->sizecode; ++pc) {
            const Fields f = decode(proto->code[pc]);
#    if LUA_VERSION_NUM >= 504
            if (f.op == OP_LOADKX) {
                const Fields next = decode(proto->code[pc + 1]);
                if (next.op == OP_EXTRAARG) {
                    extra_ax[static_cast<std::size_t>(pc)] = next.Ax;
                }
            }
#    endif
        }

        for (int pc = 0; pc < proto->sizecode; ++pc) {
            const Instruction raw = proto->code[pc];
            const Fields d        = decode(raw);
            const char* name      = op_name(d.op);

            DisassembledInstruction inst;
            inst.pc       = pc;
            inst.op_name  = name ? name : ("UNKNOWN_" + num(static_cast<int>(d.op)));
            inst.operands = name ? format_operands(d, name, pc) : spaces(d.A, d.B, d.C);
            inst.comment  = name ? comment(d, name, proto, pc, extra_ax[static_cast<std::size_t>(pc)])
                                 : std::string();
            if (pc < static_cast<int>(lines.size()) && lines[static_cast<std::size_t>(pc)] > 0) {
                inst.has_line = true;
                inst.line     = lines[static_cast<std::size_t>(pc)];
            }
            char address[16];
            std::snprintf(address, sizeof(address), "0x%08X", pc);
            inst.address = address;
            char bytes[16];
            std::snprintf(bytes, sizeof(bytes), "%08X", static_cast<unsigned>(raw & 0xFFFFFFFFu));
            inst.bytes = bytes;
            const std::string op(inst.op_name);
            inst.invalid = op == "EXTRAARG" || op == "VARARGPREP";
            inst.symbol  = name ? symbol(proto, pc, d) : std::string();
            out.instructions.push_back(std::move(inst));
        }
        return true;
    }

}  // namespace luadebug::target

#else  // LUA_VERSION_NUM < 503

// The old worker's disassembler only covered 5.3 - 5.5 (5.1/5.2 bytecode is
// not addressable there either), so these entry points report "unsupported"
// instead of leaving the bridge with undefined symbols.

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

}  // namespace luadebug::target

#endif  // LUA_VERSION_NUM >= 503
