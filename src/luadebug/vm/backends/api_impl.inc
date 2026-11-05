// Normalized C API for the Lua 5.x family: one implementation, compiled once
// per version. Everything here goes through the module's resolved `Raw` table;
// no `lua_*` symbol is linked.

#include <atomic>
#include <cstring>


namespace luadebug::vm::LDB_NS {

    namespace {

        inline int abs_index(const Raw* r, lua_State* L, int idx) {
            const int real = real_index(idx);
            if (real > LUA_REGISTRYINDEX && real < 0) {
                return r->gettop(L) + real + 1;
            }
            return real;
        }

        // --- stack -------------------------------------------------------
        int x_gettop(const Raw* r, lua_State* L) {
            return r->gettop(L);
        }

        void x_settop(const Raw* r, lua_State* L, int idx) {
            r->settop(L, real_index(idx));
        }

        void x_pushvalue(const Raw* r, lua_State* L, int idx) {
            if (is_globals(idx)) {
                push_globals(r, L);
                return;
            }
            r->pushvalue(L, real_index(idx));
        }

        int x_absindex(const Raw* r, lua_State* L, int idx) {
            if (idx <= LUA_REGISTRYINDEX) {
                return real_index(idx);
            }
#if LUA_VERSION_NUM >= 502
            return r->absindex(L, idx);
#else
            return (idx > LUA_REGISTRYINDEX && idx < 0) ? r->gettop(L) + idx + 1 : idx;
#endif
        }

        void x_insert(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 503
            r->rotate(L, idx, 1);
#else
            r->insert(L, idx);
#endif
        }

        void x_remove(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 503
            // `lua_remove` is `lua_rotate(idx, -1)` *and* a pop: rotating alone
            // leaves a duplicate on the stack.
            r->rotate(L, idx, -1);
            r->settop(L, -2);
#else
            r->remove(L, idx);
#endif
        }

        void x_replace(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 503
            r->rotate(L, idx, -1);
            r->settop(L, -2); // rotate + pop == replace
#else
            r->replace(L, idx);
#endif
        }

        void x_xmove(const Raw* r, lua_State* from, lua_State* to, int n) {
            r->xmove(from, to, n);
        }

        int x_checkstack(const Raw* r, lua_State* L, int n) {
            return r->checkstack(L, n);
        }

        // --- push --------------------------------------------------------
        void x_pushnil(const Raw* r, lua_State* L) {
            r->pushnil(L);
        }
        void x_pushboolean(const Raw* r, lua_State* L, int v) {
            r->pushboolean(L, v);
        }
        void x_pushinteger(const Raw* r, lua_State* L, long long v) {
            r->pushinteger(L, static_cast<lua_Integer>(v));
        }
        void x_pushnumber(const Raw* r, lua_State* L, double v) {
            r->pushnumber(L, static_cast<lua_Number>(v));
        }
        void x_pushlstring(const Raw* r, lua_State* L, const char* s, std::size_t n) {
            r->pushlstring(L, s, n);
        }
        void x_pushstring(const Raw* r, lua_State* L, const char* s) {
            r->pushstring(L, s);
        }
        void x_pushlightuserdata(const Raw* r, lua_State* L, void* p) {
            r->pushlightuserdata(L, p);
        }
        void x_pushcclosure(const Raw* r, lua_State* L, CFunc fn, int n) {
            r->pushcclosure(L, fn, n);
        }
        lua_State* x_newthread(const Raw* r, lua_State* L) {
            return r->newthread(L);
        }
        void x_createtable(const Raw* r, lua_State* L, int narr, int nrec) {
            r->createtable(L, narr, nrec);
        }

        // --- access ------------------------------------------------------
        int x_type(const Raw* r, lua_State* L, int idx) {
            return r->type(L, real_index(idx));
        }
        const char* x_typename_(const Raw* r, lua_State* L, int t) {
            (void)L;
            return r->typename_(L, t);
        }
        int x_toboolean(const Raw* r, lua_State* L, int idx) {
            return r->toboolean(L, real_index(idx));
        }
        long long x_tointeger(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 502
            int isnum = 0;
            const lua_Integer v = r->tointegerx(L, idx, &isnum);
            return isnum ? static_cast<long long>(v) : 0;
#else
            return static_cast<long long>(r->tointeger(L, idx));
#endif
        }
        double x_tonumber(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 502
            int isnum = 0;
            const lua_Number v = r->tonumberx(L, idx, &isnum);
            return isnum ? static_cast<double>(v) : 0.0;
#else
            return static_cast<double>(r->tonumber(L, idx));
#endif
        }
        const char* x_tolstring(const Raw* r, lua_State* L, int idx, std::size_t* len) {
            return r->tolstring(L, real_index(idx), len);
        }
        void* x_touserdata(const Raw* r, lua_State* L, int idx) {
            return r->touserdata(L, real_index(idx));
        }
        const void* x_topointer(const Raw* r, lua_State* L, int idx) {
            return r->topointer(L, real_index(idx));
        }
        CFunc x_tocfunction(const Raw* r, lua_State* L, int idx) {
            return reinterpret_cast<CFunc>(r->tocfunction(L, real_index(idx)));
        }
        std::size_t x_rawlen(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 502
            return static_cast<std::size_t>(r->rawlen(L, idx));
#else
            return static_cast<std::size_t>(r->objlen(L, idx));
#endif
        }
        int x_rawequal(const Raw* r, lua_State* L, int a, int b) {
            return r->rawequal(L, real_index(a), real_index(b));
        }
        int x_iscfunction(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
            if (r->type(L, idx) != LUA_TFUNCTION) {
                return 0;
            }
            return r->tocfunction(L, idx) != nullptr;
        }
        int x_isinteger(const Raw* r, lua_State* L, int idx) {
            idx = real_index(idx);
#if LUA_VERSION_NUM >= 503
            return r->isinteger(L, idx);
#else
            if (r->type(L, idx) != LUA_TNUMBER) {
                return 0;
            }
#if LUA_VERSION_NUM >= 502
            // 5.2 spells `lua_tonumber` as a macro over `lua_tonumberx`, so the
            // plain entry is not exported at all.
            const lua_Number v = r->tonumberx(L, idx, nullptr);
#else
            const lua_Number v = r->tonumber(L, idx);
#endif
            return static_cast<lua_Number>(static_cast<lua_Integer>(v)) == v;
#endif
        }
        int x_stringtonumber(const Raw* r, lua_State* L, const char* s) {
            // Added in 5.3; older profiles convert numbers themselves.
            if (!r->stringtonumber) {
                return 0;
            }
            return r->stringtonumber(L, s) != 0;
        }

        // --- tables and globals ------------------------------------------
        int x_getglobal(const Raw* r, lua_State* L, const char* name) {
#if LUA_VERSION_NUM == 501
            r->getfield(L, LUA_GLOBALSINDEX, name);
#else
            r->getglobal(L, name);
#endif
            return r->type(L, -1);
        }
        void x_setglobal(const Raw* r, lua_State* L, const char* name) {
#if LUA_VERSION_NUM == 501
            r->setfield(L, LUA_GLOBALSINDEX, name);
#else
            r->setglobal(L, name);
#endif
        }
        int x_getfield(const Raw* r, lua_State* L, int idx, const char* name) {
            idx = real_index(idx);
            r->getfield(L, idx, name);
#if LUA_VERSION_NUM >= 502
            return LUA_TNONE; // 5.2+ report the pushed type; caller uses type()
#else
            return r->type(L, -1);
#endif
        }
        void x_setfield(const Raw* r, lua_State* L, int idx, const char* name) {
            r->setfield(L, real_index(idx), name);
        }
        void x_gettable(const Raw* r, lua_State* L, int idx) {
            r->gettable(L, real_index(idx));
        }
        void x_settable(const Raw* r, lua_State* L, int idx) {
            r->settable(L, real_index(idx));
        }
        void x_rawget(const Raw* r, lua_State* L, int idx) {
            r->rawget(L, real_index(idx));
        }
        void x_rawset(const Raw* r, lua_State* L, int idx) {
            r->rawset(L, real_index(idx));
        }
        int x_rawgeti(const Raw* r, lua_State* L, int idx, long long n) {
            idx = real_index(idx);
            r->rawgeti(L, idx, static_cast<lua_Integer>(n));
#if LUA_VERSION_NUM >= 502
            return LUA_TNONE;
#else
            return r->type(L, -1);
#endif
        }
        void x_rawseti(const Raw* r, lua_State* L, int idx, long long n) {
            r->rawseti(L, real_index(idx), static_cast<lua_Integer>(n));
        }
        int x_next(const Raw* r, lua_State* L, int idx) {
            return r->next(L, real_index(idx));
        }

        // --- metatables / environments / upvalues ------------------------
        int x_getmetatable(const Raw* r, lua_State* L, int idx) {
            return r->getmetatable(L, real_index(idx));
        }
        int x_setmetatable(const Raw* r, lua_State* L, int idx) {
            return r->setmetatable(L, real_index(idx));
        }
        int x_getfenv(const Raw* r, lua_State* L, int idx) {
#if LUA_VERSION_NUM == 501
            return r->getfenv(L, real_index(idx));
#else
            (void)r;
            (void)L;
            (void)idx;
            return 0;
#endif
        }
        int x_setfenv(const Raw* r, lua_State* L, int idx) {
#if LUA_VERSION_NUM == 501
            return r->setfenv(L, real_index(idx));
#else
            (void)r;
            (void)L;
            (void)idx;
            return 0;
#endif
        }
        int x_getiuservalue(const Raw* r, lua_State* L, int idx, int n) {
            idx = real_index(idx);
#if LUA_VERSION_NUM == 501
            if (n != 1) {
                r->pushnil(L);
                return LUA_TNONE;
            }
            r->getfenv(L, idx);
            return r->type(L, -1);
#elif LUA_VERSION_NUM <= 503
            // 5.2 and 5.3 have exactly one uservalue and no indexed variant.
            if (n != 1) {
                r->pushnil(L);
                return LUA_TNONE;
            }
            r->getuservalue(L, idx);
            return r->type(L, -1);
#else
            return r->getiuservalue(L, idx, n);
#endif
        }
        int x_setiuservalue(const Raw* r, lua_State* L, int idx, int n) {
            idx = real_index(idx);
#if LUA_VERSION_NUM == 501
            if (n != 1) {
                r->settop(L, -2);
                return 0;
            }
            return r->setfenv(L, idx);
#elif LUA_VERSION_NUM <= 503
            if (n != 1) {
                r->settop(L, -2);
                return 0;
            }
            r->setuservalue(L, idx);
            return 1;
#else
            return r->setiuservalue(L, idx, n);
#endif
        }
        const char* x_getupvalue(const Raw* r, lua_State* L, int func, int n) {
            return r->getupvalue(L, real_index(func), n);
        }
        const char* x_setupvalue(const Raw* r, lua_State* L, int func, int n) {
            return r->setupvalue(L, real_index(func), n);
        }

        // --- calls -------------------------------------------------------
        void x_call(const Raw* r, lua_State* L, int nargs, int nresults) {
#if LUA_VERSION_NUM >= 502
            // `lua_call` is a macro over `lua_callk` from 5.2 on; the plain entry
            // is not exported at all.
            r->callk(L, nargs, nresults, 0, nullptr);
#else
            r->call(L, nargs, nresults);
#endif
        }
        int x_pcall(const Raw* r, lua_State* L, int nargs, int nresults, int errfunc) {
#if LUA_VERSION_NUM >= 502
            return r->pcallk(L, nargs, nresults, real_index(errfunc), 0, nullptr);
#else
            return r->pcall(L, nargs, nresults, real_index(errfunc));
#endif
        }
        int x_error(const Raw* r, lua_State* L) {
            return r->error(L);
        }
        int x_loadbuffer(const Raw* r, lua_State* L, const char* buff, std::size_t size,
                         const char* name) {
#if LUA_VERSION_NUM >= 502
            return r->loadbufferx(L, buff, size, name, nullptr);
#else
            return r->loadbuffer(L, buff, size, name);
#endif
        }

        // --- registry references -----------------------------------------
        // Same contract as `luaL_ref(L, LUA_REGISTRYINDEX)`: the value on top of
        // the stack is stored at `registry[ref]` and consumed. Consumers read it
        // back with `rawgeti(registry, ref)`, so a private table would break them.
        // Starts past the registry's reserved slots (main thread, globals): a
        // reference must never overwrite them.
        std::atomic<int> g_next_ref { 1024 };

        int x_ref(const Raw* r, lua_State* L) {
            const int ref = g_next_ref.fetch_add(1, std::memory_order_relaxed);
            r->rawseti(L, LUA_REGISTRYINDEX, ref);  // registry[ref] = value, pops it
            return ref;
        }
        void x_unref(const Raw* r, lua_State* L, int ref) {
            if (ref <= 0) {
                return;
            }
            r->pushnil(L);
            r->rawseti(L, LUA_REGISTRYINDEX, ref);
        }

        // --- hooks --------------------------------------------------------
        unsigned translate_mask(unsigned mask) {
            return mask; // identical bit layout in every 5.x version
        }

        void x_sethook(const Raw* r, lua_State* L, HookFn fn, unsigned mask, int count) {
            r->sethook(L, fn, translate_mask(mask), count);
        }
        HookFn x_gethook(const Raw* r, lua_State* L, int* mask) {
            return reinterpret_cast<HookFn>(r->gethook(L, mask));
        }
        unsigned x_gethookmask(const Raw* r, lua_State* L) {
            return r->gethookmask(L);
        }
        int x_gethookcount(const Raw* r, lua_State* L) {
            return r->gethookcount(L);
        }

        // --- allocator ----------------------------------------------------
        Alloc x_getallocf(const Raw* r, lua_State* L, void** ud) {
            return reinterpret_cast<Alloc>(r->getallocf(L, ud));
        }
        Alloc x_setallocf(const Raw* r, lua_State* L, Alloc fn, void* ud) {
            return reinterpret_cast<Alloc>(r->setallocf(L, reinterpret_cast<lua_Alloc>(fn), ud));
        }

        // --- debug frames -------------------------------------------------
        int x_getstack(const Raw* r, lua_State* L, int level, FrameInfo* out) {
            // `lua_getstack` fills only the frame identity; every other field of
            // `lua_Debug` (source, what, name, nups, ...) is left uninitialized.
            // Projecting a record that was never zeroed reads garbage - on 5.2/5.3
            // the projection even runs `strlen()` on it.
            lua_Debug native;
            std::memset(&native, 0, sizeof(native));
            const int ok = r->getstack(L, level, &native);
            if (!ok) {
                return 0;
            }
            static_assert(sizeof(lua_Debug) <= FrameInfo::kNativeSize,
                          "FrameInfo must be able to hold the target debug record");
            std::memcpy(out->native, &native, sizeof(lua_Debug));
            project(native, out);
            return 1;
        }
        int x_getinfo(const Raw* r, lua_State* L, const char* what, FrameInfo* ar) {
            lua_Debug native;
            std::memcpy(&native, ar->native, sizeof(lua_Debug));
            const int ok = r->getinfo(L, what, &native);
            if (!ok) {
                return 0;
            }
            std::memcpy(ar->native, &native, sizeof(lua_Debug));
            project(native, ar);
            return 1;
        }

        const char* x_getlocal(const Raw* r, lua_State* L, FrameInfo* ar, int n) {
            lua_Debug native;
            std::memcpy(&native, ar->native, sizeof(lua_Debug));
            return getlocal(r, L, &native, n);
        }
        const char* x_setlocal(const Raw* r, lua_State* L, FrameInfo* ar, int n) {
            lua_Debug native;
            std::memcpy(&native, ar->native, sizeof(lua_Debug));
            const char* name = r->setlocal(L, &native, n);
            std::memcpy(ar->native, &native, sizeof(lua_Debug));
            return name;
        }

        // --- probe --------------------------------------------------------
        // Validates that the resolved table really is this profile's Lua. The
        // macro/function split is the trap here: 5.2+ spell `lua_call`,
        // `lua_pcall`, `lua_tointeger`, `luaL_loadbuffer` and (5.3+) the uservalue
        // and stack-shuffling entry points as macros, so the plain symbols do not
        // exist. Anything the backend actually uses must be present *before* a
        // hook, allocator or worker is installed.
        bool x_probe(const Raw* r, std::string* why) {
            auto need = [why](bool present, const char* name) {
                if (!present) {
                    *why = std::string("missing ") + name;
                    return false;
                }
                return true;
            };
            // Every entry point the wrappers below call without a null check must
            // be present here: a table that satisfies a smaller required set would
            // bind and then call through a null pointer inside the debuggee.
            if (!need(r->gettop != nullptr, "lua_gettop")
                || !need(r->settop != nullptr, "lua_settop")
                || !need(r->pushvalue != nullptr, "lua_pushvalue")
                || !need(r->type != nullptr, "lua_type")
                || !need(r->checkstack != nullptr, "lua_checkstack")
                || !need(r->pushnil != nullptr, "lua_pushnil")
                || !need(r->pushboolean != nullptr, "lua_pushboolean")
                || !need(r->pushinteger != nullptr, "lua_pushinteger")
                || !need(r->pushnumber != nullptr, "lua_pushnumber")
                || !need(r->pushstring != nullptr, "lua_pushstring")
                || !need(r->pushlightuserdata != nullptr, "lua_pushlightuserdata")
                || !need(r->toboolean != nullptr, "lua_toboolean")
                || !need(r->tolstring != nullptr, "lua_tolstring")
                || !need(r->touserdata != nullptr, "lua_touserdata")
                || !need(r->topointer != nullptr, "lua_topointer")
                || !need(r->tocfunction != nullptr, "lua_tocfunction")
                || !need(r->typename_ != nullptr, "lua_typename")
                || !need(r->rawequal != nullptr, "lua_rawequal")
                || !need(r->gettable != nullptr, "lua_gettable")
                || !need(r->settable != nullptr, "lua_settable")
                || !need(r->xmove != nullptr, "lua_xmove")
                || !need(r->pushlstring != nullptr, "lua_pushlstring")
                || !need(r->pushcclosure != nullptr, "lua_pushcclosure")
                || !need(r->createtable != nullptr, "lua_createtable")
                || !need(r->newthread != nullptr, "lua_newthread")
                || !need(r->getfield != nullptr, "lua_getfield")
                || !need(r->setfield != nullptr, "lua_setfield")
                || !need(r->rawget != nullptr, "lua_rawget")
                || !need(r->rawset != nullptr, "lua_rawset")
                || !need(r->rawgeti != nullptr, "lua_rawgeti")
                || !need(r->rawseti != nullptr, "lua_rawseti")
                || !need(r->next != nullptr, "lua_next")
                || !need(r->getmetatable != nullptr, "lua_getmetatable")
                || !need(r->setmetatable != nullptr, "lua_setmetatable")
                || !need(r->getupvalue != nullptr, "lua_getupvalue")
                || !need(r->setupvalue != nullptr, "lua_setupvalue")
                || !need(r->getstack != nullptr, "lua_getstack")
                || !need(r->getinfo != nullptr, "lua_getinfo")
                || !need(r->getlocal != nullptr, "lua_getlocal")
                || !need(r->setlocal != nullptr, "lua_setlocal")
                || !need(r->sethook != nullptr, "lua_sethook")
                || !need(r->gethook != nullptr, "lua_gethook")
                || !need(r->gethookmask != nullptr, "lua_gethookmask")
                || !need(r->gethookcount != nullptr, "lua_gethookcount")
                || !need(r->getallocf != nullptr, "lua_getallocf")
                || !need(r->setallocf != nullptr, "lua_setallocf")
                || !need(r->error != nullptr, "lua_error")) {
                return false;
            }
#if defined(LUAJIT_VERSION)
            if (!need(r->setmode != nullptr, "luaJIT_setmode")) {
                return false;  // not LuaJIT
            }
            if (!need(r->getfenv != nullptr, "lua_getfenv")
                || !need(r->setfenv != nullptr, "lua_setfenv")
                || !need(r->call != nullptr, "lua_call")
                || !need(r->pcall != nullptr, "lua_pcall")
                || !need(r->insert != nullptr, "lua_insert")
                || !need(r->remove != nullptr, "lua_remove")
                || !need(r->objlen != nullptr, "lua_objlen")
                || !need(r->tonumber != nullptr, "lua_tonumber")
                || !need(r->tointeger != nullptr, "lua_tointeger")) {
                return false;
            }
#elif LUA_VERSION_NUM == 501
            if (r->setmode) {
                *why = "module looks like LuaJIT";
                return false;
            }
            if (!need(r->getfenv != nullptr, "lua_getfenv")
                || !need(r->setfenv != nullptr, "lua_setfenv")
                || !need(r->call != nullptr, "lua_call")
                || !need(r->pcall != nullptr, "lua_pcall")
                || !need(r->insert != nullptr, "lua_insert")
                || !need(r->remove != nullptr, "lua_remove")
                || !need(r->replace != nullptr, "lua_replace")
                || !need(r->objlen != nullptr, "lua_objlen")
                || !need(r->tonumber != nullptr, "lua_tonumber")
                || !need(r->tointeger != nullptr, "lua_tointeger")
                || !need(r->loadbuffer != nullptr, "luaL_loadbuffer")) {
                return false;
            }
#else
            if (r->getfenv || r->setfenv) {
                *why = "module exposes 5.1 environments";
                return false;
            }
            // 5.1 and LuaJIT spell the global accessors as macros and the backend
            // derives them from `lua_getfield`; 5.2+ call them directly.
            if (!need(r->getglobal != nullptr, "lua_getglobal")
                || !need(r->setglobal != nullptr, "lua_setglobal")
                || !need(r->callk != nullptr, "lua_callk")
                || !need(r->pcallk != nullptr, "lua_pcallk")
                || !need(r->tonumberx != nullptr, "lua_tonumberx")
                || !need(r->tointegerx != nullptr, "lua_tointegerx")
                || !need(r->rawlen != nullptr, "lua_rawlen")
                || !need(r->loadbufferx != nullptr, "luaL_loadbufferx")
                || !need(r->absindex != nullptr, "lua_absindex")) {
                return false;
            }
#    if LUA_VERSION_NUM >= 504
            if (!need(r->rotate != nullptr, "lua_rotate")
                || !need(r->isinteger != nullptr, "lua_isinteger")
                || !need(r->stringtonumber != nullptr, "lua_stringtonumber")
                || !need(r->getiuservalue != nullptr, "lua_getiuservalue")
                || !need(r->setiuservalue != nullptr, "lua_setiuservalue")) {
                return false;
            }
#    elif LUA_VERSION_NUM == 503
            if (!need(r->rotate != nullptr, "lua_rotate")
                || !need(r->isinteger != nullptr, "lua_isinteger")
                || !need(r->stringtonumber != nullptr, "lua_stringtonumber")
                || !need(r->getuservalue != nullptr, "lua_getuservalue")
                || !need(r->setuservalue != nullptr, "lua_setuservalue")) {
                return false;
            }
#    else
            // 5.2: stack shuffling is still a function per operation and the
            // uservalue API is the single-value one; `lua_stringtonumber` and
            // `lua_rotate` do not exist yet.
            if (!need(r->insert != nullptr, "lua_insert")
                || !need(r->remove != nullptr, "lua_remove")
                || !need(r->replace != nullptr, "lua_replace")
                || !need(r->getuservalue != nullptr, "lua_getuservalue")
                || !need(r->setuservalue != nullptr, "lua_setuservalue")) {
                return false;
            }
#    endif
#endif
            return true;
        }

        int x_lua_version_num() {
            return LUA_VERSION_NUM;
        }

        bool x_has_exception_hook() {
#if defined(LUA_MASKEXCEPTION)
            return true;
#else
            return false;
#endif
        }
        bool x_has_thread_hook() {
#if defined(LUA_MASKTHREAD)
            return true;
#else
            return false;
#endif
        }

        const Api kApi = {
            // stack
            x_gettop, x_settop, x_pushvalue, x_absindex, x_insert, x_remove, x_replace,
            x_xmove, x_checkstack,
            // push
            x_pushnil, x_pushboolean, x_pushinteger, x_pushnumber, x_pushlstring,
            x_pushstring, x_pushlightuserdata, x_pushcclosure, x_newthread, x_createtable,
            // access
            x_type, x_typename_, x_toboolean, x_tointeger, x_tonumber, x_tolstring,
            x_touserdata, x_topointer, x_tocfunction, x_rawlen, x_rawequal, x_iscfunction,
            x_isinteger, x_stringtonumber,
            // tables
            x_getglobal, x_setglobal, x_getfield, x_setfield, x_gettable, x_settable,
            x_rawget, x_rawset, x_rawgeti, x_rawseti, x_next,
            // metatables
            x_getmetatable, x_setmetatable, x_getfenv, x_setfenv, x_getiuservalue,
            x_setiuservalue, x_getupvalue, x_setupvalue,
            // calls
            x_call, x_pcall, x_error, x_loadbuffer,
            // refs
            x_ref, x_unref,
            // hooks
            x_sethook, x_gethook, x_gethookmask, x_gethookcount,
            // allocator
            x_getallocf, x_setallocf,
            // frames
            x_getstack, x_getinfo, x_getlocal, x_setlocal,
            // layout
            stacklevel, main_thread, callinfo, ci2proto, debug2ci, ci_previous, hook_ci,
            hook_info, exception_code, is_lua_func, cdatatype, allowhook_get, allowhook_set,
            // proto
            proto_identity, proto_lines, collect_proto_lines, proto_at, frame_pc_of,
            disassemble_proto,
            // table slots
            table_array_size, table_hash_size, table_array_base_zero, table_get_hash_kv,
            table_get_hash_k, table_get_hash_v, table_set_hash_v, table_get_array,
            table_set_array,
            // disassembly
            disassembly_supported,
            // probe
            x_probe,
            x_has_exception_hook, x_has_thread_hook, x_lua_version_num,
        };

    } // namespace

} // namespace luadebug::vm::LDB_NS

    // Called by the family's translation unit to publish this profile's table.
    namespace luadebug::vm {
        void register_backend(Profile profile, const Api* api);
    }

    // Internal linkage: every profile family compiles this file, so an external
    // definition would collide in the single bridge library.
    static void ld_backend_register() {
        luadebug::vm::register_backend(luadebug::vm::Profile::LDB_PROFILE,
                                       &luadebug::vm::LDB_NS::kApi);
    }
