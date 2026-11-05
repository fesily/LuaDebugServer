#pragma once

// The single VM boundary of the bridge.
//
// Nothing outside `vm/` may include a Lua header, use `LUA_VERSION_NUM`, or
// touch a per-version internal type: the bridge is compiled once per platform
// and serves Lua 5.1-5.5 and LuaJIT through the backends in `vm/backends/`.
//
// Two layers live here:
//   * `Raw` - the target module's own C API, resolved per module (typed
//     pointers, never linked),
//   * `Api` - the normalized operations the shared core uses. Each backend
//     implements them with its own headers and internal layout.
//
// Indices, types, hook events and hook masks are normalized: `idx::registry`
// and friends are sentinels the backend translates, so a shared caller never
// has to know that 5.1 uses -10000 for the registry and 5.2+ uses -1001000.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "target/disasm.h"
#include "target/proto_view.h"

struct lua_State;

namespace luadebug::vm {

    using CFunc = int (*)(lua_State*);
    using Alloc = void* (*)(void* ud, void* ptr, std::size_t osize, std::size_t nsize);
    // `ar` is the target's own `lua_Debug` record; only backends dereference it.
    using HookFn   = void (*)(lua_State* L, const void* ar);
    using Resolver = void* (*)(void* module, const char* name);

    // Supported ABI/layout profiles. `lua-latest` maps to `lua55`: its sources
    // are byte-identical to 5.5 (verified with `diff -qr`), so it shares both
    // the backend and the group.
    enum class Profile : int {
        lua51  = 0,
        lua52  = 1,
        lua53  = 2,
        lua54  = 3,
        lua55  = 4,
        luajit = 5,
    };

    constexpr int kProfileCount = 6;

    const char* profile_name(Profile profile);
    const char* profile_symbol(Profile profile);
    bool profile_parse(const char* text, Profile& out);

    // ABI/layout group. States of one group may share a debugging session;
    // different groups may coexist in the process but never in one session.
    inline int profile_group(Profile profile) {
        return static_cast<int>(profile);
    }

    namespace type {
        enum : int {
            none          = -1,
            nil           = 0,
            boolean       = 1,
            lightuserdata = 2,
            number        = 3,
            string        = 4,
            table         = 5,
            function      = 6,
            userdata      = 7,
            thread        = 8,
        };
    }

    // Semantic pseudo-indices. Every backend translates them to the version's
    // own constants before calling the target module.
    namespace idx {
        constexpr int registry = -0x3fffff01;
        constexpr int globals  = -0x3fffff02;
        constexpr int environ  = -0x3fffff03;
        inline int upvalue(int n) {
            return -0x3fffff03 - n;
        }
    }

    namespace event {
        constexpr int call      = 0;
        constexpr int ret       = 1;
        constexpr int line      = 2;
        constexpr int count     = 3;
        constexpr int tail_call = 4;
        constexpr int exception = 5;
        constexpr int thread    = 6;
    }

    // Bit per hook event: the target's own `LUA_MASK*` are `1 << LUA_HOOK*`, and
    // the patched Lua versions number the exception/thread events 5 and 6.
    namespace mask {
        constexpr unsigned call      = 1u << event::call;
        constexpr unsigned ret       = 1u << event::ret;
        constexpr unsigned line      = 1u << event::line;
        constexpr unsigned count     = 1u << event::count;
        constexpr unsigned exception = 1u << event::exception;
        constexpr unsigned thread    = 1u << event::thread;
    }

    constexpr int kMultiRet = -1;
    constexpr int kOk       = 0;

    // Normalized projection of the target's `lua_Debug`.
    //
    // The native record stays inside `native`, sized by the backend with a
    // static_assert; sharing one version's `lua_Debug` as a common structure
    // would be a layout bug waiting to happen.
    struct FrameInfo {
        static constexpr std::size_t kNativeSize  = 192;
        static constexpr std::size_t kNativeAlign = 16;

        alignas(kNativeAlign) unsigned char native[kNativeSize] = {};

        // Normalized projection, filled by `getstack` / `getinfo`.
        int event             = 0;
        int currentline       = -1;
        int linedefined       = -1;
        int lastlinedefined   = -1;
        const char* name      = nullptr;
        const char* namewhat  = "";
        const char* what      = "";
        const char* source    = nullptr;
        std::size_t srclen    = 0;
        const char* short_src = "";
        // `short_src` is an inline array in the target's debug record; the
        // projection copies it so the pointer stays valid after the backend
        // returns (the native record is a temporary).
        char short_src_storage[128] = {};
        int nups                    = 0;
        int nparams                 = 0;
        bool isvararg               = false;
        bool istailcall             = false;
        int ftransfer               = 0;
        int ntransfer               = 0;
    };

    // One hook event, projected out of the target's own debug record.
    struct HookInfo {
        int event       = 0;
        int currentline = -1;
        // The target's native record (a `lua_Debug`); backends dereference it,
        // shared code never does.
        const void* record = nullptr;
    };

    // The target module's C API, resolved per module identity.
    //
    // Entry points absent from a profile stay null (for example `lua_getfenv`
    // after 5.1); normalized wrappers then report the documented fallback.
    struct Raw {
        // stack
        int (*gettop)(lua_State*);
        void (*settop)(lua_State*, int);
        void (*pushvalue)(lua_State*, int);
        int (*absindex)(lua_State*, int);
        void (*rotate)(lua_State*, int, int);
        void (*insert)(lua_State*, int);
        void (*remove)(lua_State*, int);
        void (*replace)(lua_State*, int);
        void (*xmove)(lua_State*, lua_State*, int);
        int (*checkstack)(lua_State*, int);
        // push
        void (*pushnil)(lua_State*);
        void (*pushboolean)(lua_State*, int);
        void (*pushinteger)(lua_State*, long long);
        void (*pushnumber)(lua_State*, double);
        void (*pushlstring)(lua_State*, const char*, std::size_t);
        void (*pushstring)(lua_State*, const char*);
        void (*pushlightuserdata)(lua_State*, void*);
        void (*pushcclosure)(lua_State*, CFunc, int);
        lua_State* (*newthread)(lua_State*);
        void (*createtable)(lua_State*, int, int);
        // access
        int (*type)(lua_State*, int);
        const char* (*typename_)(lua_State*, int);
        int (*toboolean)(lua_State*, int);
        int (*tointegerx)(lua_State*, int, int*);
        int (*tonumberx)(lua_State*, int, int*);
        // 5.1/5.2 have no `x` variant; the backend falls back to these.
        long long (*tointeger)(lua_State*, int);
        double (*tonumber)(lua_State*, int);
        int (*isnumber)(lua_State*, int);
        int (*isstring)(lua_State*, int);
        const char* (*tolstring)(lua_State*, int, std::size_t*);
        void* (*touserdata)(lua_State*, int);
        const void* (*topointer)(lua_State*, int);
        CFunc (*tocfunction)(lua_State*, int);
        std::size_t (*rawlen)(lua_State*, int);
        int (*rawequal)(lua_State*, int, int);
        int (*isinteger)(lua_State*, int);
        // 5.1 reports `size_t` (lua_objlen); 5.2+ report `lua_Unsigned`.
        std::size_t (*objlen)(lua_State*, int);
        // tables and globals
        void (*getglobal)(lua_State*, const char*);
        void (*setglobal)(lua_State*, const char*);
        void (*getfield)(lua_State*, int, const char*);
        void (*setfield)(lua_State*, int, const char*);
        void (*gettable)(lua_State*, int);
        void (*settable)(lua_State*, int);
        void (*rawget)(lua_State*, int);
        void (*rawset)(lua_State*, int);
        void (*rawgeti)(lua_State*, int, long long);
        void (*rawseti)(lua_State*, int, long long);
        int (*next)(lua_State*, int);
        // metatables / environments
        int (*getmetatable)(lua_State*, int);
        int (*setmetatable)(lua_State*, int);
        int (*getfenv)(lua_State*, int);
        int (*setfenv)(lua_State*, int);
        // 5.1/5.2 expose one uservalue; 5.3+ index it.
        int (*getuservalue)(lua_State*, int);
        // 5.3+ replace the single uservalue with an indexed one.
        int (*getiuservalue)(lua_State*, int, int);
        // 5.1 has no uservalue API at all; the backend maps it to the fenv.
        int (*setuservalue)(lua_State*, int);
        int (*setiuservalue)(lua_State*, int, int);
        // upvalues
        const char* (*getupvalue)(lua_State*, int, int);
        const char* (*setupvalue)(lua_State*, int, int);
        // calls
        void (*call)(lua_State*, int, int);
        // 5.2+ spell `lua_call` as a macro over `lua_callk`.
        void (*callk)(lua_State*, int, int, int, void*);
        int (*pcall)(lua_State*, int, int, int);
        int (*pcallk)(lua_State*, int, int, int, intptr_t, void*);
        int (*error)(lua_State*);
        int (*loadbufferx)(lua_State*, const char*, std::size_t, const char*, const char*);
        int (*loadbuffer)(lua_State*, const char*, std::size_t, const char*);
        // hooks
        void (*sethook)(lua_State*, HookFn, unsigned, int);
        HookFn (*gethook)(lua_State*, int*);
        unsigned (*gethookmask)(lua_State*);
        int (*gethookcount)(lua_State*);
        // allocator
        Alloc (*getallocf)(lua_State*, void** ud);
        Alloc (*setallocf)(lua_State*, Alloc, void*);
        // debug
        int (*getstack)(lua_State*, int, void* ar);
        int (*getinfo)(lua_State*, const char* what, void* ar);
        const char* (*getlocal)(lua_State*, const void* ar, int n);
        const char* (*setlocal)(lua_State*, const void* ar, int n);
        const char* (*getfuncname)(lua_State*, const void* ar, const char** namewhat);
        int (*stringtonumber)(lua_State*, const char*);
        // LuaJIT only; presence discriminates it from stock Lua 5.1.
        void* (*setmode)(lua_State*, int, int);
    };

    // Normalized operations implemented by each backend.
    struct Api {
        // --- stack -----------------------------------------------------------
        int (*gettop)(const Raw* r, lua_State*);
        void (*settop)(const Raw* r, lua_State*, int);
        void (*pushvalue)(const Raw* r, lua_State*, int);
        int (*absindex)(const Raw* r, lua_State*, int);
        void (*insert)(const Raw* r, lua_State*, int);
        void (*remove)(const Raw* r, lua_State*, int);
        void (*replace)(const Raw* r, lua_State*, int);
        void (*xmove)(const Raw* r, lua_State*, lua_State*, int);
        int (*checkstack)(const Raw* r, lua_State*, int);
        // --- push ------------------------------------------------------------
        void (*pushnil)(const Raw* r, lua_State*);
        void (*pushboolean)(const Raw* r, lua_State*, int);
        void (*pushinteger)(const Raw* r, lua_State*, long long);
        void (*pushnumber)(const Raw* r, lua_State*, double);
        void (*pushlstring)(const Raw* r, lua_State*, const char*, std::size_t);
        void (*pushstring)(const Raw* r, lua_State*, const char*);
        void (*pushlightuserdata)(const Raw* r, lua_State*, void*);
        void (*pushcclosure)(const Raw* r, lua_State*, CFunc, int);
        lua_State* (*newthread)(const Raw* r, lua_State*);
        void (*createtable)(const Raw* r, lua_State*, int, int);
        // --- access ----------------------------------------------------------
        int (*type)(const Raw* r, lua_State*, int);
        const char* (*typename_)(const Raw* r, lua_State*, int);
        int (*toboolean)(const Raw* r, lua_State*, int);
        long long (*tointeger)(const Raw* r, lua_State*, int);
        double (*tonumber)(const Raw* r, lua_State*, int);
        const char* (*tolstring)(const Raw* r, lua_State*, int, std::size_t*);
        void* (*touserdata)(const Raw* r, lua_State*, int);
        const void* (*topointer)(const Raw* r, lua_State*, int);
        CFunc (*tocfunction)(const Raw* r, lua_State*, int);
        std::size_t (*rawlen)(const Raw* r, lua_State*, int);
        int (*rawequal)(const Raw* r, lua_State*, int, int);
        int (*iscfunction)(const Raw* r, lua_State*, int);
        int (*isinteger)(const Raw* r, lua_State*, int);
        int (*stringtonumber)(const Raw* r, lua_State*, const char*);
        // --- tables and globals ---------------------------------------------
        int (*getglobal)(const Raw* r, lua_State*, const char*);
        void (*setglobal)(const Raw* r, lua_State*, const char*);
        int (*getfield)(const Raw* r, lua_State*, int, const char*);
        void (*setfield)(const Raw* r, lua_State*, int, const char*);
        void (*gettable)(const Raw* r, lua_State*, int);
        void (*settable)(const Raw* r, lua_State*, int);
        void (*rawget)(const Raw* r, lua_State*, int);
        void (*rawset)(const Raw* r, lua_State*, int);
        int (*rawgeti)(const Raw* r, lua_State*, int, long long);
        void (*rawseti)(const Raw* r, lua_State*, int, long long);
        int (*next)(const Raw* r, lua_State*, int);
        // --- metatables / environments / upvalues ---------------------------
        int (*getmetatable)(const Raw* r, lua_State*, int);
        int (*setmetatable)(const Raw* r, lua_State*, int);
        int (*getfenv)(const Raw* r, lua_State*, int);
        int (*setfenv)(const Raw* r, lua_State*, int);
        int (*getiuservalue)(const Raw* r, lua_State*, int, int);
        int (*setiuservalue)(const Raw* r, lua_State*, int, int);
        const char* (*getupvalue)(const Raw* r, lua_State*, int, int);
        const char* (*setupvalue)(const Raw* r, lua_State*, int, int);
        // --- calls -----------------------------------------------------------
        void (*call)(const Raw* r, lua_State*, int, int);
        int (*pcall)(const Raw* r, lua_State*, int, int, int);
        int (*error)(const Raw* r, lua_State*);
        // Compiles a chunk with the target's own compiler; returns 0 on success
        // and leaves the function on the stack.
        int (*loadbuffer)(const Raw* r, lua_State*, const char* buff, std::size_t size, const char* name);
        // --- registry references --------------------------------------------
        int (*ref)(const Raw* r, lua_State*);
        void (*unref)(const Raw* r, lua_State*, int ref);
        // --- hooks -----------------------------------------------------------
        void (*sethook)(const Raw* r, lua_State*, HookFn, unsigned mask, int count);
        HookFn (*gethook)(const Raw* r, lua_State*, int* mask);
        unsigned (*gethookmask)(const Raw* r, lua_State*);
        int (*gethookcount)(const Raw* r, lua_State*);
        // --- allocator -------------------------------------------------------
        Alloc (*getallocf)(const Raw* r, lua_State*, void** ud);
        Alloc (*setallocf)(const Raw* r, lua_State*, Alloc, void* ud);
        // --- debug frames ----------------------------------------------------
        int (*getstack)(const Raw* r, lua_State*, int level, FrameInfo* ar);
        int (*getinfo)(const Raw* r, lua_State*, const char* what, FrameInfo* ar);
        const char* (*getlocal)(const Raw* r, lua_State*, FrameInfo* ar, int n);
        const char* (*setlocal)(const Raw* r, lua_State*, FrameInfo* ar, int n);

        // --- internal layout -------------------------------------------------
        int (*stacklevel)(const Raw* r, lua_State*);
        lua_State* (*mainthread)(const Raw* r, lua_State*);
        void* (*callinfo)(const Raw* r, lua_State*);  // current CallInfo*
        void* (*ci2proto)(const Raw* r, void* ci);    // Proto* or null for C functions
        void* (*debug2ci)(const Raw* r, lua_State*, const FrameInfo* ar);
        void* (*ci_previous)(const Raw* r, void* ci);
        // `ar` is the target's native hook record.
        int (*hook_ci)(const Raw* r, lua_State*, const void* ar, void** ci);  // 0 when absent
        void (*hook_info)(const Raw* r, const void* ar, HookInfo* out);
        // Error code carried by an exception-hook event (versions that have one).
        int (*exception_code)(const Raw* r, lua_State*, const void* ar);
        int (*is_lua_func)(const Raw* r, lua_State*, const void* ar);
        const char* (*cdatatype)(const Raw* r, lua_State*, int idx);  // LuaJIT cdata, null otherwise
        int (*allowhook_get)(const Raw* r, lua_State*);
        void (*allowhook_set)(const Raw* r, lua_State*, int v);

        // Proto views. `proto` is the backend's own Proto pointer.
        bool (*proto_identity)(void* proto, int& linedefined, int& lastlinedefined, std::string_view& source);
        bool (*proto_lines)(void* proto, std::vector<int>& lines);
        bool (*collect_proto_lines)(const Raw* r, lua_State* L, int func_index, target::LineInfo& lines, std::string& error);
        void* (*proto_at)(const Raw* r, lua_State*, int func_index);
        int (*frame_pc)(const Raw* r, lua_State*, int level);
        bool (*disassemble_proto)(void* proto, target::Disassembly& out, std::string& error);

        // Table slot access (LuaJIT hash-part stability, 5.x array/hash slots).
        unsigned int (*table_array_size)(const void* t);
        unsigned int (*table_hash_size)(const void* t);
        bool (*table_array_base_zero)();
        bool (*table_get_hash_kv)(lua_State*, const void* tv, unsigned int i);
        bool (*table_get_hash_k)(lua_State*, const void* tv, unsigned int i);
        bool (*table_get_hash_v)(lua_State*, const void* tv, unsigned int i);
        bool (*table_set_hash_v)(lua_State*, const void* tv, unsigned int i);
        bool (*table_get_array)(lua_State*, const void* tv, unsigned int i);
        bool (*table_set_array)(lua_State*, const void* tv, unsigned int i);

        // Disassembly availability (5.3-5.5 today).
        bool (*disassembly_supported)();
        // Copies a function at `func_index` into a stable handle? Not needed:
        // prototypes live as long as the function value does.

        // --- misc ------------------------------------------------------------
        // Validates that the resolved table really is this profile: required
        // symbols present, discriminating entry points agreeing. Fills `why` on
        // failure. Called before any hook, allocator or worker is installed.
        bool (*probe)(const Raw* r, std::string* why);
        // Non-zero when the version has exception hooks / thread hooks.
        bool (*has_exception_hook)();
        bool (*has_thread_hook)();
        // LUA_VERSION_NUM of the profile (501 for LuaJIT). Only semantics that
        // genuinely differ by version (e.g. stepping masks) consult it.
        int (*lua_version_num)();
    };

    // Backend registration: each backend self-registers at load time, so the
    // shared core has no link-time dependency on backends it does not carry
    // (a one-profile test binary links exactly one of them).
    void register_backend(Profile profile, const Api* api);
    const Api* backend_for(Profile profile);

}  // namespace luadebug::vm
