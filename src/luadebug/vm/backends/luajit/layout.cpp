#include "vm/backends/luajit/internal.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <lj_cdata.h>
#include <lj_ctype.h>
#include <lj_debug.h>
#include <lj_dispatch.h>
#include <lj_frame.h>
#include <lj_obj.h>
}

namespace luadebug::vm::LDB_NS {

    namespace {
        // LuaJIT resolves a negative index against the current frame; the
        // compat layer used the same helper for the cdata/upvalue paths.
        TValue* index2adr(lua_State* L, int idx) {
            if (idx > 0) {
                TValue* o = L->base + (idx - 1);
                return o < L->top ? o : niltv(L);
            }
            if (idx > LUA_REGISTRYINDEX) {
                return L->top + idx;
            }
            if (idx == LUA_GLOBALSINDEX) {
                TValue* o = &G(L)->tmptv;
                settabV(L, o, tabref(L->env));
                return o;
            }
            if (idx == LUA_REGISTRYINDEX) {
                return registry(L);
            }
            GCfunc* fn = curr_func(L);
            if (idx == LUA_ENVIRONINDEX) {
                TValue* o = &G(L)->tmptv;
                settabV(L, o, tabref(fn->c.env));
                return o;
            }
            idx = LUA_GLOBALSINDEX - idx;
            return idx <= fn->c.nupvalues ? &fn->c.upvalue[idx - 1] : niltv(L);
        }

        cTValue* debug_frame(lua_State* L, int level, int* size) {
            cTValue *frame, *nextframe, *bot = tvref(L->stack) + LJ_FR2;
            for (nextframe = frame = L->base - 1; frame > bot;) {
                if (frame_gc(frame) == obj2gco(L)) {
                    level++;  // skip dummy frames
                }
                if (level-- == 0) {
                    *size = static_cast<int>(nextframe - frame);
                    return frame;
                }
                nextframe = frame;
                if (frame_islua(frame)) {
                    frame = frame_prevl(frame);
                } else {
                    if (frame_isvarg(frame)) {
                        level++;
                    }
                    frame = frame_prevd(frame);
                }
            }
            *size = level;
            return nullptr;
        }

        Proto* func2proto(GCfunc* func) {
            if (!isluafunc(func)) {
                return nullptr;
            }
            return funcproto(func);
        }
    }  // namespace

    void push_globals(const Raw* r, lua_State* L) {
        r->pushvalue(L, LUA_GLOBALSINDEX);
    }

    void project(const lua_Debug& a, FrameInfo* out) {
        out->event           = a.event;
        out->currentline     = a.currentline;
        out->linedefined     = a.linedefined;
        out->lastlinedefined = a.lastlinedefined;
        out->name            = a.name;
        out->namewhat        = a.namewhat ? a.namewhat : "";
        out->what            = a.what ? a.what : "";
        out->source          = a.source;
        out->srclen          = a.source ? std::strlen(a.source) : 0;
        out->short_src       = a.short_src;
        out->nups            = a.nups;
        out->nparams         = 0;
        out->isvararg        = false;
        out->istailcall      = false;
        out->ftransfer       = 0;
        out->ntransfer       = 0;
    }

#if !defined(LUAJIT_VERSION)
    const char* getlocal(const Raw* r, lua_State* L, lua_Debug* ar, int n) {
        return r->getlocal(L, ar, n);
    }
#else
    const char* getlocal(const Raw* r, lua_State* L, lua_Debug* ar, int n) {
        return r->getlocal(L, ar, n);
    }
#endif

    void hook_info(const Raw* r, const void* ar, HookInfo* out) {
        (void)r;
        if (!ar) {
            out->event       = -1;
            out->currentline = -1;
            out->record      = nullptr;
            return;
        }
        const lua_Debug* a = static_cast<const lua_Debug*>(ar);
        out->event         = a->event;
        out->currentline   = a->currentline;
        out->record        = ar;
    }

    int stacklevel(const Raw* r, lua_State* L) {
        (void)r;
        int level = 0;
        cTValue *frame, *nextframe, *bot = tvref(L->stack) + LJ_FR2;
        for (nextframe = frame = L->base - 1; frame > bot;) {
            if (frame_gc(frame) == obj2gco(L)) {
                level--;
            }
            level++;
            nextframe = frame;
            if (frame_islua(frame)) {
                frame = frame_prevl(frame);
            } else {
                if (frame_isvarg(frame)) {
                    level--;
                }
                frame = frame_prevd(frame);
            }
        }
        return level;
    }

    lua_State* main_thread(const Raw* r, lua_State* L) {
        (void)r;
        return &gcref(G(L)->mainthref)->th;
    }

    void* callinfo(const Raw* r, lua_State* L) {
        (void)r;
        int size = 0;
        return const_cast<void*>(static_cast<const void*>(debug_frame(L, 0, &size)));
    }

    void* ci2proto(const Raw* r, void* ci) {
        (void)r;
        if (!ci) {
            return nullptr;
        }
        return func2proto(frame_func(static_cast<cTValue*>(ci)));
    }

    void* debug2ci(const Raw* r, lua_State* L, const FrameInfo* ar) {
        (void)r;
        const lua_Debug* a    = reinterpret_cast<const lua_Debug*>(ar->native);
        const uint32_t offset = static_cast<uint32_t>(a->i_ci) & 0xffff;
        return tvref(L->stack) + offset;
    }

    void* ci_previous(const Raw* r, void* ci) {
        // LuaJIT's CallInfo is a TValue and has no `previous`; the return event
        // path uses `getstack(1)` instead (see hook::Core).
        (void)r;
        (void)ci;
        return nullptr;
    }

    int hook_ci(const Raw* r, lua_State* L, const void* ar, void** out) {
        (void)r;
        const lua_Debug* a = static_cast<const lua_Debug*>(ar);
        if (!a || !a->i_ci) {
            return 0;
        }
        const uint32_t offset = static_cast<uint32_t>(a->i_ci) & 0xffff;
        *out                  = tvref(L->stack) + offset;
        return 1;
    }

    int is_lua_func(const Raw* r, lua_State* L, const void* ar) {
        (void)r;
        const lua_Debug* a = static_cast<const lua_Debug*>(ar);
        if (!a) {
            return 0;
        }
        const uint32_t offset = static_cast<uint32_t>(a->i_ci) & 0xffff;
        cTValue* ci           = tvref(L->stack) + offset;
        return isluafunc(frame_func(ci)) ? 1 : 0;
    }

    const char* cdatatype(const Raw* r, lua_State* L, int idx) {
        (void)r;
        cTValue* o = index2adr(L, real_index(idx));
        if (!tviscdata(o)) {
            return nullptr;
        }
        GCcdata* cd = cdataV(o);
        return cd->ctypeid == CTID_CTYPEID ? "ctype" : "cdata";
    }

    int exception_code(const Raw* r, lua_State* L, const void* ar) {
        // LuaJIT has no exception hook.
        (void)r;
        (void)L;
        (void)ar;
        return 0;
    }

    int allowhook_get(const Raw* r, lua_State* L) {
        (void)r;
        return hook_active(G(L)) ? 0 : 1;
    }

    void allowhook_set(const Raw* r, lua_State* L, int v) {
        (void)r;
        global_State* g = G(L);
        if (v) {
            hook_leave(g);
        } else {
            hook_enter(g);
        }
    }

    void* proto_at(const Raw* r, lua_State* L, int abs_index) {
        const int top       = r->gettop(L);
        const TValue* value = L->top + (abs_index - (top + 1));
        if (!tvisfunc(value)) {
            return nullptr;
        }
        GCfunc* fn = funcV(value);
        if (!isluafunc(fn)) {
            return nullptr;
        }
        return funcproto(fn);
    }

    int frame_pc_of(const Raw* r, lua_State* L, int level) {
        (void)r;
        (void)L;
        (void)level;
        // LuaJIT exposes no supported way to read a frame's bytecode position.
        return -1;
    }

}  // namespace luadebug::vm::LDB_NS
