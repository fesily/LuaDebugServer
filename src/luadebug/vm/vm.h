#pragma once

// A debuggee VM binding plus the states that belong to it.
//
// Every operation a shared caller performs on a target state goes through
// `State`, so the binding travels with the state: there is no ambient "current
// Lua API", no thread-local to keep coherent, and no way to call one module's
// symbols with another module's state.
//
// Binding identity is `(module handle, profile, raw API table)`. Two modules of
// the same version still get their own `Vm`, because their resolved function
// addresses may differ.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vm/api.h"

namespace luadebug::vm {

    class State;

    class Vm {
    public:
        Vm(Profile profile, std::string identity, const Api* api, const Raw& raw);
        ~Vm();

        Vm(const Vm&)            = delete;
        Vm& operator=(const Vm&) = delete;

        Profile profile() const {
            return profile_;
        }
        int group() const {
            return profile_group(profile_);
        }
        const Api& api() const {
            return *api_;
        }
        const Raw* raw() const {
            return &raw_;
        }
        // Stable identity of this binding: the profile plus every resolved
        // function address. Two descriptions that resolve to the same addresses
        // are the same VM; anything else is a different one, even when both claim
        // the same profile.
        const std::string& identity() const {
            return identity_;
        }

        // --- states ----------------------------------------------------------
        // Finds the state, adopting it into this VM when it is not known yet.
        // Adoption is only meaningful inside this VM (a hook fired for it, or
        // it was created through `newthread`).
        State* state(lua_State* raw);
        State* find(lua_State* raw) const;

        void forget(lua_State* raw);
        // Drops every state of this VM (shutdown).
        void forget_all();

        std::size_t state_count();

    private:
        Profile profile_;
        std::string identity_;
        const Api* api_;
        Raw raw_;
        std::unordered_map<lua_State*, std::unique_ptr<State>> states_;
        mutable std::mutex mutex_;
    };

    class State {
    public:
        State() = default;
        State(lua_State* raw, Vm* vm)
            : raw_(raw)
            , vm_(vm) {}

        lua_State* raw() const {
            return raw_;
        }
        Vm* vm() const {
            return vm_;
        }
        bool valid() const {
            return raw_ != nullptr && vm_ != nullptr;
        }

        // Global registry lookup (hook trampolines, C closures that only
        // receive a raw state). Never creates an entry; null when the state is
        // unknown. A thread-local cache in front is a cache only: a miss falls
        // back to the registry, so correctness never depends on it.
        static State* find(lua_State* raw);

        // --- stack -----------------------------------------------------------
        int gettop() const {
            return A().gettop(R(), raw_);
        }
        void settop(int idx) const {
            A().settop(R(), raw_, idx);
        }
        void pushvalue(int idx) const {
            A().pushvalue(R(), raw_, idx);
        }
        int absindex(int idx) const {
            return A().absindex(R(), raw_, idx);
        }
        void insert(int idx) const {
            A().insert(R(), raw_, idx);
        }
        void remove(int idx) const {
            A().remove(R(), raw_, idx);
        }
        void replace(int idx) const {
            A().replace(R(), raw_, idx);
        }
        void xmove(State* to, int n) const {
            A().xmove(R(), raw_, to->raw(), n);
        }
        bool checkstack(int n) const {
            return A().checkstack(R(), raw_, n) != 0;
        }
        void pop(int n = 1) const {
            A().settop(R(), raw_, -n - 1);
        }
        // --- push ------------------------------------------------------------
        void pushnil() const {
            A().pushnil(R(), raw_);
        }
        void pushboolean(bool v) const {
            A().pushboolean(R(), raw_, v ? 1 : 0);
        }
        void pushinteger(long long v) const {
            A().pushinteger(R(), raw_, v);
        }
        void pushnumber(double v) const {
            A().pushnumber(R(), raw_, v);
        }
        void pushlstring(const char* s, std::size_t n) const {
            A().pushlstring(R(), raw_, s, n);
        }
        void pushstring(const char* s) const {
            A().pushstring(R(), raw_, s);
        }
        void pushlightuserdata(void* p) const {
            A().pushlightuserdata(R(), raw_, p);
        }
        void pushcclosure(CFunc fn, int n) const {
            A().pushcclosure(R(), raw_, fn, n);
        }
        State* newthread() const {
            return vm_->state(A().newthread(R(), raw_));
        }
        void createtable(int narr, int nrec) const {
            A().createtable(R(), raw_, narr, nrec);
        }
        // --- access ----------------------------------------------------------
        int type(int idx) const {
            return A().type(R(), raw_, idx);
        }
        const char* typename_(int t) const {
            return A().typename_(R(), raw_, t);
        }
        bool toboolean(int idx) const {
            return A().toboolean(R(), raw_, idx) != 0;
        }
        long long tointeger(int idx) const {
            return A().tointeger(R(), raw_, idx);
        }
        double tonumber(int idx) const {
            return A().tonumber(R(), raw_, idx);
        }
        const char* tolstring(int idx, std::size_t* len = nullptr) const {
            return A().tolstring(R(), raw_, idx, len);
        }
        const char* tostring(int idx) const {
            return A().tolstring(R(), raw_, idx, nullptr);
        }
        void* touserdata(int idx) const {
            return A().touserdata(R(), raw_, idx);
        }
        const void* topointer(int idx) const {
            return A().topointer(R(), raw_, idx);
        }
        CFunc tocfunction(int idx) const {
            return A().tocfunction(R(), raw_, idx);
        }
        std::size_t rawlen(int idx) const {
            return A().rawlen(R(), raw_, idx);
        }
        bool rawequal(int a, int b) const {
            return A().rawequal(R(), raw_, a, b) != 0;
        }
        bool iscfunction(int idx) const {
            return A().iscfunction(R(), raw_, idx) != 0;
        }
        bool isinteger(int idx) const {
            return A().isinteger(R(), raw_, idx) != 0;
        }
        bool isfunction(int idx) const {
            return type(idx) == type::function;
        }
        bool istable(int idx) const {
            return type(idx) == type::table;
        }
        bool isstring(int idx) const {
            return type(idx) == type::string;
        }
        bool isnoneornil(int idx) const {
            const int t = type(idx);
            return t == type::none || t == type::nil;
        }
        bool isnil(int idx) const {
            return type(idx) == type::nil;
        }
        bool stringtonumber(const char* s) const {
            return A().stringtonumber(R(), raw_, s) != 0;
        }
        // --- tables and globals ---------------------------------------------
        int getglobal(const char* name) const {
            return A().getglobal(R(), raw_, name);
        }
        void setglobal(const char* name) const {
            A().setglobal(R(), raw_, name);
        }
        int getfield(int idx, const char* name) const {
            return A().getfield(R(), raw_, idx, name);
        }
        void setfield(int idx, const char* name) const {
            A().setfield(R(), raw_, idx, name);
        }
        void gettable(int idx) const {
            A().gettable(R(), raw_, idx);
        }
        void settable(int idx) const {
            A().settable(R(), raw_, idx);
        }
        void rawget(int idx) const {
            A().rawget(R(), raw_, idx);
        }
        void rawset(int idx) const {
            A().rawset(R(), raw_, idx);
        }
        int rawgeti(int idx, long long n) const {
            return A().rawgeti(R(), raw_, idx, n);
        }
        void rawseti(int idx, long long n) const {
            A().rawseti(R(), raw_, idx, n);
        }
        void rawgetp(int idx, const void* p) const {
            pushlightuserdata(const_cast<void*>(p));
            rawget(idx);
        }
        void rawsetp(int idx, const void* p) const {
            pushlightuserdata(const_cast<void*>(p));
            insert(-2);
            rawset(idx);
        }
        bool next(int idx) const {
            return A().next(R(), raw_, idx) != 0;
        }
        // --- metatables / environments / upvalues ---------------------------
        bool getmetatable(int idx) const {
            return A().getmetatable(R(), raw_, idx) != 0;
        }
        bool setmetatable(int idx) const {
            return A().setmetatable(R(), raw_, idx) != 0;
        }
        bool getfenv(int idx) const {
            return A().getfenv(R(), raw_, idx) != 0;
        }
        bool setfenv(int idx) const {
            return A().setfenv(R(), raw_, idx) != 0;
        }
        int getiuservalue(int idx, int n) const {
            return A().getiuservalue(R(), raw_, idx, n);
        }
        bool setiuservalue(int idx, int n) const {
            return A().setiuservalue(R(), raw_, idx, n) != 0;
        }
        const char* getupvalue(int funcindex, int n) const {
            return A().getupvalue(R(), raw_, funcindex, n);
        }
        const char* setupvalue(int funcindex, int n) const {
            return A().setupvalue(R(), raw_, funcindex, n);
        }
        static int upvalue(int n) {
            return idx::upvalue(n);
        }
        // --- calls -----------------------------------------------------------
        void call(int nargs, int nresults) const {
            A().call(R(), raw_, nargs, nresults);
        }
        int pcall(int nargs, int nresults, int errfunc = 0) const {
            return A().pcall(R(), raw_, nargs, nresults, errfunc);
        }
        int error() const {
            return A().error(R(), raw_);
        }
        int loadbuffer(const char* buff, std::size_t size, const char* name) const {
            return A().loadbuffer(R(), raw_, buff, size, name);
        }
        // --- registry references --------------------------------------------
        int ref() const {
            return A().ref(R(), raw_);
        }
        void unref(int r) const {
            A().unref(R(), raw_, r);
        }
        // --- hooks -----------------------------------------------------------
        void sethook(HookFn fn, unsigned mask, int count) const {
            A().sethook(R(), raw_, fn, mask, count);
        }
        HookFn gethook(int* mask) const {
            return A().gethook(R(), raw_, mask);
        }
        unsigned gethookmask() const {
            return A().gethookmask(R(), raw_);
        }
        int gethookcount() const {
            return A().gethookcount(R(), raw_);
        }
        bool allowhook() const {
            return A().allowhook_get(R(), raw_) != 0;
        }
        void set_allowhook(bool v) const {
            A().allowhook_set(R(), raw_, v ? 1 : 0);
        }
        // --- allocator -------------------------------------------------------
        Alloc getallocf(void** ud) const {
            return A().getallocf(R(), raw_, ud);
        }
        Alloc setallocf(Alloc fn, void* ud) const {
            return A().setallocf(R(), raw_, fn, ud);
        }
        // --- debug frames ----------------------------------------------------
        bool getstack(int level, FrameInfo* ar) const {
            return A().getstack(R(), raw_, level, ar) != 0;
        }
        bool getinfo(const char* what, FrameInfo* ar) const {
            return A().getinfo(R(), raw_, what, ar) != 0;
        }
        const char* getlocal(FrameInfo* ar, int n) const {
            return A().getlocal(R(), raw_, ar, n);
        }
        const char* setlocal(FrameInfo* ar, int n) const {
            return A().setlocal(R(), raw_, ar, n);
        }
        // --- internal layout -------------------------------------------------
        int stacklevel() const {
            return A().stacklevel(R(), raw_);
        }
        State* mainthread() const {
            return vm_->state(A().mainthread(R(), raw_));
        }
        void* callinfo() const {
            return A().callinfo(R(), raw_);
        }
        void* ci2proto(void* ci) const {
            return A().ci2proto(R(), ci);
        }
        void* debug2ci(const FrameInfo* ar) const {
            return A().debug2ci(R(), raw_, ar);
        }
        void* ci_previous(void* ci) const {
            return A().ci_previous(R(), ci);
        }
        int hook_ci(const HookInfo& ar, void** ci) const {
            return A().hook_ci(R(), raw_, ar.record, ci);
        }
        bool is_lua_func(const HookInfo& ar) const {
            return A().is_lua_func(R(), raw_, ar.record) != 0;
        }
        const char* cdatatype(int idx) const {
            return A().cdatatype(R(), raw_, idx);
        }
        // Table slot access. The table is addressed by `topointer`, and the
        // slot semantics differ per version (array base, hash node ordering,
        // LuaJIT's GCtab), so the backend owns them.
        unsigned table_array_size(const void* t) const {
            return A().table_array_size(t);
        }
        unsigned table_hash_size(const void* t) const {
            return A().table_hash_size(t);
        }
        bool table_array_base_zero() const {
            return A().table_array_base_zero();
        }
        bool table_get_hash_kv(const void* t, unsigned i) const {
            return A().table_get_hash_kv(raw_, t, i);
        }
        bool table_get_hash_k(const void* t, unsigned i) const {
            return A().table_get_hash_k(raw_, t, i);
        }
        bool table_get_hash_v(const void* t, unsigned i) const {
            return A().table_get_hash_v(raw_, t, i);
        }
        bool table_set_hash_v(const void* t, unsigned i) const {
            return A().table_set_hash_v(raw_, t, i);
        }
        bool table_get_array(const void* t, unsigned i) const {
            return A().table_get_array(raw_, t, i);
        }
        bool table_set_array(const void* t, unsigned i) const {
            return A().table_set_array(raw_, t, i);
        }

        // Prototype handle of the Lua function at `func_index` (null when the
        // value is not a Lua function).
        void* proto_at(int func_index) const {
            return A().proto_at(R(), raw_, absindex(func_index));
        }
        int frame_pc(int level) const {
            return A().frame_pc(R(), raw_, level);
        }

    private:
        const Api& A() const {
            return vm_->api();
        }
        const Raw* R() const {
            return vm_->raw();
        }

        lua_State* raw_ = nullptr;
        Vm* vm_         = nullptr;
    };

    // The VM that owns `raw`, or null. Same lookup as `State::find`, for entry
    // points that need the binding before they may touch the stack.
    Vm* vm_of(lua_State* raw);

}  // namespace luadebug::vm
