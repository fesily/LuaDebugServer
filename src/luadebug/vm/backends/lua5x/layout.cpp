#include "vm/backends/lua5x/internal.h"

#include <cstdio>
#include <cstring>

#include <lobject.h>
#include <lstate.h>

#if LUA_VERSION_NUM >= 504
#    define LDB_STKID(s) (s).p
#else
#    define LDB_STKID(s) (s)
#    define s2v(o) (o)
#endif

namespace luadebug::vm::LDB_NS {

    void push_globals(const Raw* r, lua_State* L) {
#if LUA_VERSION_NUM == 501
        r->pushvalue(L, LUA_GLOBALSINDEX);
#else
        r->rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
#endif
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
#if LUA_VERSION_NUM >= 504
        out->srclen = a.srclen;
#else
        out->srclen = a.source ? std::strlen(a.source) : 0;
#endif
        std::snprintf(out->short_src_storage, sizeof(out->short_src_storage), "%s", a.short_src ? a.short_src : "");
        out->short_src = out->short_src_storage;
        out->nups      = a.nups;
#if LUA_VERSION_NUM >= 502
        out->nparams    = a.nparams;
        out->isvararg   = a.isvararg != 0;
        out->istailcall = a.istailcall != 0;
#else
        out->nparams    = 0;
        out->isvararg   = false;
        out->istailcall = false;
#endif
#if LUA_VERSION_NUM >= 504
        out->ftransfer = a.ftransfer;
        out->ntransfer = a.ntransfer;
#else
        out->ftransfer = 0;
        out->ntransfer = 0;
#endif
    }

#if LUA_VERSION_NUM == 501
    // 5.1 hands out varargs through a negative index; positive indices are the
    // target's own.
    const char* getlocal(const Raw* r, lua_State* L, lua_Debug* ar, int n) {
        if (n >= 0) {
            return r->getlocal(L, ar, n);
        }
        CallInfo* ci = L->base_ci + ar->i_ci;
        if (!ci) {
            return nullptr;
        }
        Proto* proto = static_cast<Proto*>(ci2proto(r, ci));
        if (!proto) {
            return nullptr;
        }
        const int index   = -n;
        const int nparams = proto->numparams;
        if (index >= ci->base - ci->func - nparams) {
            return nullptr;
        }
        StkId o = ci->func + nparams + index;
        setobj2s(L, L->top, o);
        L->top++;
        return "(*vararg)";
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
#if LUA_VERSION_NUM >= 502
        for (CallInfo* ci = L->ci; ci != &L->base_ci; ci = ci->previous) {
            level++;
        }
#else
        for (CallInfo* ci = L->ci; ci > L->base_ci; ci--) {
            level++;
            if (f_isLua(ci))
                level += ci->tailcalls;
        }
#endif
        return level;
    }

    lua_State* main_thread(const Raw* r, lua_State* L) {
        (void)r;
#if LUA_VERSION_NUM >= 505
        return &G(L)->mainth.l;
#else
        return L->l_G->mainthread;
#endif
    }

    namespace {
        Proto* func2proto(StkId func) {
#if LUA_VERSION_NUM >= 502
            if (!ttisLclosure(s2v(func))) {
                return nullptr;
            }
            return clLvalue(s2v(func))->p;
#else
            if (clvalue(func)->c.isC) {
                return nullptr;
            }
            return clvalue(func)->l.p;
#endif
        }
    }  // namespace

    void* callinfo(const Raw* r, lua_State* L) {
        (void)r;
        return L->ci;
    }

    void* ci2proto(const Raw* r, void* ci) {
        (void)r;
        if (!ci) {
            return nullptr;
        }
        return func2proto(LDB_STKID(static_cast<CallInfo*>(ci)->func));
    }

    void* debug2ci(const Raw* r, lua_State* L, const FrameInfo* ar) {
        (void)r;
        const lua_Debug* a = reinterpret_cast<const lua_Debug*>(ar->native);
#if LUA_VERSION_NUM >= 502
        return a->i_ci;
#else
        return L->base_ci + a->i_ci;
#endif
    }

    void* ci_previous(const Raw* r, void* ci) {
        (void)r;
        CallInfo* info = static_cast<CallInfo*>(ci);
        if (!info) {
            return nullptr;
        }
#if LUA_VERSION_NUM >= 502
        return info->previous;
#else
        // 5.1 stores call frames in one array with no chained list; callers use
        // `getstack(1)` there.
        return nullptr;
#endif
    }

    int hook_ci(const Raw* r, lua_State* L, const void* ar, void** out) {
        (void)r;
        const lua_Debug* a = static_cast<const lua_Debug*>(ar);
        if (!a || !a->i_ci) {
            return 0;
        }
#if LUA_VERSION_NUM >= 502
        *out = a->i_ci;
#else
        *out = L->base_ci + a->i_ci;
#endif
        return 1;
    }

    int is_lua_func(const Raw* r, lua_State* L, const void* ar) {
        (void)r;
        (void)L;
        (void)ar;
        // Every Lua frame in 5.x is defined; only LuaJIT needs the check.
        return 1;
    }

    const char* cdatatype(const Raw* r, lua_State* L, int idx) {
        (void)r;
        (void)L;
        (void)idx;
        return nullptr;
    }

    int exception_code(const Raw* r, lua_State* L, const void* ar) {
        (void)r;
        const lua_Debug* a = static_cast<const lua_Debug*>(ar);
        if (!a) {
            return 0;
        }
#if LUA_VERSION_NUM >= 505
        // 5.5 keeps the pending transfer count on the thread.
        (void)a;
        return static_cast<int>(L->transferinfo.ntransfer);
#elif LUA_VERSION_NUM >= 504
        return a->i_ci ? static_cast<int>(a->i_ci->u2.transferinfo.ntransfer) : 0;
#else
        (void)L;
        return a->currentline;
#endif
    }

    int allowhook_get(const Raw* r, lua_State* L) {
        (void)r;
        return L->allowhook;
    }

    void allowhook_set(const Raw* r, lua_State* L, int v) {
        (void)r;
        L->allowhook = static_cast<lu_byte>(v);
    }

    void* proto_at(const Raw* r, lua_State* L, int abs_index) {
        const int top    = r->gettop(L);
        const StkId slot = LDB_STKID(L->top) + (abs_index - top - 1);
        return func2proto(slot);
    }

    int frame_pc_of(const Raw* r, lua_State* L, int level) {
        if (!L) {
            return -1;
        }
        lua_Debug ar;
        if (!r->getstack(L, level, &ar)) {
            return -1;
        }
#if LUA_VERSION_NUM >= 502
        CallInfo* ci = ar.i_ci;
#else
        CallInfo* ci = L->base_ci + ar.i_ci;
#endif
        if (!ci) {
            return -1;
        }
        Proto* proto = func2proto(LDB_STKID(ci->func));
        if (!proto || !proto->code) {
            return -1;
        }
#if LUA_VERSION_NUM >= 502
        const Instruction* saved = ci->u.l.savedpc;
#else
        const Instruction* saved = ci->savedpc;
#endif
        // `savedpc` points just past the instruction being executed, so `- 1`
        // names the instruction about to run. `disassemble` and instruction
        // breakpoints use the same convention.
        const ptrdiff_t pc = saved - proto->code - 1;
        if (pc < 0) {
            return 0;
        }
        if (pc >= proto->sizecode) {
            return proto->sizecode > 0 ? proto->sizecode - 1 : 0;
        }
        return static_cast<int>(pc);
    }

}  // namespace luadebug::vm::LDB_NS
