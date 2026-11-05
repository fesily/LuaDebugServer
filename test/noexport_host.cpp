// A Lua host that exports nothing, used to prove the table-only binding path.
//
// The interpreter is linked statically into this executable and no Lua symbol is
// reachable from it: the target is built with hidden visibility and its export list
// is restricted, so the dynamic symbol table is empty. As a result:
//
//   * `dlsym` cannot find `lua_gettop` anywhere, and
//   * a stripped copy has no symbol table either.
//
// The host therefore knows its API only as addresses, takes them from its own
// function pointers, and hands that table to the bridge. Nothing in this file is
// exported to help the debugger: the binding it exercises is the same one an
// external symbol table uses for a private interpreter.
//
// Usage:
//   test_noexport_host check-hidden
//   test_noexport_host symbols [output.json]
//   test_noexport_host bind     BRIDGE PROFILE
//   test_noexport_host resolver BRIDGE PROFILE

// The vendored Lua headers carry no `extern "C"` guard: the API is compiled as C
// and reached through C linkage even from a C++ host.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bridge/bridge_abi.h"

namespace {

    int failures = 0;

    void check(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    using symbols = std::map<std::string, void*>;

    // The API this interpreter actually has. Conditional entries match the
    // version, so the table describes *this* build and never claims a symbol the
    // linker did not provide.
    symbols own_symbols() {
        symbols table;
        auto add = [&table](const char* name, void* address) {
            if (address) {
                table.emplace(name, address);
            }
        };
#define ADD(symbol) add(#symbol, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&symbol)))
        // stack
        ADD(lua_gettop);
        ADD(lua_settop);
        ADD(lua_pushvalue);
        ADD(lua_absindex);
        ADD(lua_rotate);
        // `lua_insert`/`lua_remove`/`lua_replace` are macros over `lua_rotate`
        // since 5.3, so they have no address to give; the backend derives them.
        ADD(lua_xmove);
        ADD(lua_checkstack);
        // push
        ADD(lua_pushnil);
        ADD(lua_pushboolean);
        ADD(lua_pushinteger);
        ADD(lua_pushnumber);
        ADD(lua_pushlstring);
        ADD(lua_pushstring);
        ADD(lua_pushlightuserdata);
        ADD(lua_pushcclosure);
        ADD(lua_newthread);
        ADD(lua_createtable);
        // access
        ADD(lua_type);
        ADD(lua_typename);
        ADD(lua_toboolean);
        ADD(lua_tointegerx);
        ADD(lua_tonumberx);
        ADD(lua_isnumber);
        ADD(lua_isstring);
        ADD(lua_tolstring);
        ADD(lua_touserdata);
        ADD(lua_topointer);
        ADD(lua_tocfunction);
        ADD(lua_rawlen);
        ADD(lua_isinteger);
        ADD(lua_rawequal);
        ADD(lua_stringtonumber);
        // tables and globals
        ADD(lua_getglobal);
        ADD(lua_setglobal);
        ADD(lua_getfield);
        ADD(lua_setfield);
        ADD(lua_gettable);
        ADD(lua_settable);
        ADD(lua_rawget);
        ADD(lua_rawset);
        ADD(lua_rawgeti);
        ADD(lua_rawseti);
        ADD(lua_next);
        // metatables / uservalues / upvalues
        ADD(lua_getmetatable);
        ADD(lua_setmetatable);
        ADD(lua_getiuservalue);
        ADD(lua_setiuservalue);
        ADD(lua_getupvalue);
        ADD(lua_setupvalue);
        // calls
        ADD(lua_callk);
        ADD(lua_pcallk);
        ADD(lua_error);
        ADD(luaL_loadbufferx);
        // hooks and allocator
        ADD(lua_sethook);
        ADD(lua_gethook);
        ADD(lua_gethookmask);
        ADD(lua_gethookcount);
        ADD(lua_getallocf);
        ADD(lua_setallocf);
        // debug
        ADD(lua_getstack);
        ADD(lua_getinfo);
        ADD(lua_getlocal);
        ADD(lua_setlocal);
        // version evidence: data, not code
        add("lua_ident", reinterpret_cast<void*>(const_cast<char*>(lua_ident)));
        // 5.4+ only
        ADD(lua_resetthread);
        ADD(lua_newuserdatauv);
#undef ADD
        return table;
    }

    std::string hex_of(std::uintptr_t value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
        return std::string(buffer);
    }

    // Emits the host's own table as a `luaSymbols` document. Every name must be one
    // the contract accepts, otherwise the artefact cannot be fed back to the bridge;
    // `test_vm_source --check-symbols` verifies that in CTest.
    int print_symbols(const char* path) {
        const symbols table = own_symbols();
        std::string json = "{\n";
        bool first       = true;
        for (const auto& entry : table) {
            if (!first) {
                json += ",\n";
            }
            first = false;
            json += "  \"" + entry.first + "\": { \"address\": \""
                  + hex_of(reinterpret_cast<std::uintptr_t>(entry.second)) + "\" }";
        }
        json += "\n}\n";
        if (path) {
            FILE* file = std::fopen(path, "wb");
            if (!file) {
                std::fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            std::fwrite(json.data(), 1, json.size(), file);
            std::fclose(file);
        } else {
            std::fwrite(json.data(), 1, json.size(), stdout);
        }
        return 0;
    }

    // The negative proof: the interpreter is in this process and its API is
    // unreachable by name.
    int check_hidden() {
        void* global = ::dlsym(RTLD_DEFAULT, "lua_gettop");
        check(global == nullptr, "a process-wide lookup finds no lua_gettop");
        void* self = ::dlopen(nullptr, RTLD_NOW);
        check(self != nullptr, "the host can dlopen itself");
        if (self) {
            check(::dlsym(self, "lua_gettop") == nullptr,
                  "a lookup inside this image finds no lua_gettop");
            check(::dlsym(self, "luaL_newstate") == nullptr,
                  "a lookup inside this image finds no luaL_newstate");
        }
        // The API does exist, just not by name: prove it through a pointer the
        // host computed itself.
        check(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&lua_gettop)) != nullptr,
              "the API is callable through its address");
        std::printf("noexport host: %zu symbols are known by address only\n", own_symbols().size());
        return 0;
    }

    using bind_fn = const LdBridgeOps* (*)(const LdBridgeDescriptor*, void**, const char**);

    struct bridge {
        const LdBridgeOps* ops = nullptr;
        void* ctx              = nullptr;
        void* handle           = nullptr;
    };

    bind_fn load_bridge(const char* path, void*& handle) {
        handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            std::fprintf(stderr, "cannot load %s: %s\n", path, ::dlerror());
            return nullptr;
        }
        return reinterpret_cast<bind_fn>(::dlsym(handle, "luadebug_bridge_bind"));
    }

    std::uint64_t call_gettop(lua_State* L) {
        return static_cast<std::uint64_t>(lua_gettop(L));
    }

    // Runs `1 + 1` through this interpreter and returns the result. It proves the
    // binding did not corrupt the VM: the same state the bridge resolved must
    // still execute Lua.
    double execute1(lua_State* L) {
        const char* chunk = "return 1 + 1";
        if (luaL_loadbufferx(L, chunk, std::strlen(chunk), "=(noexport)", nullptr) != 0) {
            lua_settop(L, 0);
            return -1.0;
        }
        if (lua_pcallk(L, 0, 1, 0, 0, nullptr) != 0) {
            lua_settop(L, 0);
            return -1.0;
        }
        double value = static_cast<double>(lua_tonumberx(L, -1, nullptr));
        lua_settop(L, 0);
        return value;
    }

    int bind_table(const char* bridge_path, int profile) {
        void* handle   = nullptr;
        bind_fn lookup = load_bridge(bridge_path, handle);
        if (!lookup) {
            return 1;
        }
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);

        const symbols table = own_symbols();
        std::vector<LdBridgeSymbol> entries;
        entries.reserve(table.size());
        for (const auto& entry : table) {
            entries.push_back(LdBridgeSymbol { entry.first.c_str(), entry.second });
        }

        // No module, no resolver: the table is the entire identity of the VM.
        LdBridgeDescriptor descriptor {};
        descriptor.size         = sizeof(descriptor);
        descriptor.abi_version  = LDB_BRIDGE_ABI_VERSION;
        descriptor.profile      = profile;
        descriptor.state        = L;
        descriptor.symbols      = entries.data();
        descriptor.symbol_count = static_cast<uint32_t>(entries.size());

        void* ctx       = nullptr;
        const char* why = nullptr;
        const LdBridgeOps* ops = lookup(&descriptor, &ctx, &why);
        if (!ops) {
            std::fprintf(stderr, "bind failed: %s\n", why ? why : "(no reason)");
            return 1;
        }
        check(ops->profile != nullptr, "the ops table reports the bound profile");
        if (ops->profile) {
            check(ops->profile(ctx) == profile, "the bound profile is the requested one");
        }
        // A table-only binding must be able to run a whole session: registration,
        // hook installation and the worker all go through the resolved addresses.
        const char* config = "{}";
        const auto size    = static_cast<uint32_t>(std::strlen(config));
        const int started  = ops->start(ctx, config, size, 0);
        if (started != 1) {
            std::fprintf(stderr, "start failed: %s\n", ops->last_error(ctx));
        }
        check(started == 1, "the table-only binding starts a session");
        check(ops->running(ctx) == 1, "the worker runs");
        check(ops->worker_id(ctx) != 0, "the worker has an id");
        check(execute1(L) == 2.0, "the bound VM still executes Lua");
        ops->stop(ctx);
        check(ops->running(ctx) == 0, "the worker stopped");
        check(call_gettop(L) == 0, "the stack is balanced after the session");
        ops->release(ctx);
        lua_close(L);
        ::dlclose(handle);
        return 0;
    }

    int bind_resolver(const char* bridge_path, int profile) {
        void* handle   = nullptr;
        bind_fn lookup = load_bridge(bridge_path, handle);
        if (!lookup) {
            return 1;
        }
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);

        const symbols table = own_symbols();
        // The resolver's context is a value of our choosing, not `module`: the
        // call must hand back exactly this pointer so a host can carry its own
        // state (and cannot be confused with a module handle).
        struct resolver_context {
            const symbols* table = nullptr;
            int calls            = 0;
            int wrong_user       = 0;
            void* expected       = nullptr;
            void* seen_user      = nullptr;
        } context { &table, 0, 0, nullptr, nullptr };
        context.expected = &context;

        LdBridgeDescriptor descriptor {};
        descriptor.size        = sizeof(descriptor);
        descriptor.abi_version = LDB_BRIDGE_ABI_VERSION;
        descriptor.profile     = profile;
        descriptor.state       = L;
        descriptor.resolver    = [](void* user, const char* name) -> void* {
            auto* self = static_cast<resolver_context*>(user);
            if (!self) {
                return nullptr;
            }
            self->seen_user = user;
            ++self->calls;
            if (user != self->expected) {
                ++self->wrong_user;
            }
            auto it = self->table->find(name);
            return it == self->table->end() ? nullptr : it->second;
        };
        descriptor.resolver_user = &context;

        void* ctx              = nullptr;
        const char* why        = nullptr;
        const LdBridgeOps* ops = lookup(&descriptor, &ctx, &why);
        if (!ops) {
            std::fprintf(stderr, "resolver bind failed: %s\n", why ? why : "(no reason)");
            return 1;
        }
        check(context.calls > 0, "the resolver was actually consulted");
        check(context.wrong_user == 0,
              "every resolver call received the caller's own user pointer");
        check(context.seen_user == &context, "the resolver received its own user pointer");
        const char* config = "{}";
        const int started  = ops->start(ctx, config, static_cast<uint32_t>(std::strlen(config)), 0);
        if (started != 1) {
            std::fprintf(stderr, "resolver start failed: %s\n", ops->last_error(ctx));
        }
        check(started == 1, "a resolver-only binding starts a session");
        check(execute1(L) == 2.0, "the resolver-bound VM still executes Lua");
        ops->stop(ctx);
        ops->release(ctx);
        lua_close(L);
        ::dlclose(handle);
        return 0;
    }

    // A table with one entry replaced, so the identity differs from
    // `own_symbols()` while every required entry stays present and callable.
    std::vector<LdBridgeSymbol> table_with_swap(std::vector<std::string>& names,
                                                const char* from, const char* to) {
        symbols table = own_symbols();
        auto source   = table.find(to);
        if (source != table.end()) {
            table[from] = source->second;
        }
        std::vector<LdBridgeSymbol> entries;
        entries.reserve(table.size());
        names.clear();
        for (const auto& entry : table) {
            names.push_back(entry.first);
        }
        std::size_t index = 0;
        for (const auto& entry : table) {
            entries.push_back(LdBridgeSymbol { names[index].c_str(), entry.second });
            ++index;
        }
        return entries;
    }

    struct standard_table {
        std::vector<std::string> names;
        std::vector<LdBridgeSymbol> entries;

        standard_table() {
            const symbols table = own_symbols();
            names.reserve(table.size());
            for (const auto& entry : table) {
                names.push_back(entry.first);
            }
            entries.reserve(names.size());
            std::size_t index = 0;
            for (const auto& entry : table) {
                entries.push_back(LdBridgeSymbol { names[index].c_str(), entry.second });
                ++index;
            }
        }
    };

    LdBridgeDescriptor table_descriptor(void* state, int profile,
                                        std::vector<LdBridgeSymbol>& entries) {
        LdBridgeDescriptor descriptor {};
        descriptor.size         = sizeof(descriptor);
        descriptor.abi_version  = LDB_BRIDGE_ABI_VERSION;
        descriptor.profile      = profile;
        descriptor.state        = state;
        descriptor.symbols      = entries.data();
        descriptor.symbol_count = static_cast<uint32_t>(entries.size());
        return descriptor;
    }

    // Two states, three questions: the same table binds both, a table with a
    // different address set is a different VM, and a bound state refuses to be
    // rebound. Identity must come from the resolved addresses alone - there is no
    // module here to tell the VMs apart.
    int bind_two_states(const char* bridge_path, int profile) {
        void* handle   = nullptr;
        bind_fn lookup = load_bridge(bridge_path, handle);
        if (!lookup) {
            return 1;
        }
        lua_State* first  = luaL_newstate();
        lua_State* second = luaL_newstate();
        luaL_openlibs(first);
        luaL_openlibs(second);

        standard_table shared;
        LdBridgeDescriptor descriptor = table_descriptor(first, profile, shared.entries);
        void* ctx_a       = nullptr;
        const char* why   = nullptr;
        const LdBridgeOps* ops_a = lookup(&descriptor, &ctx_a, &why);
        if (!ops_a) {
            std::fprintf(stderr, "first bind failed: %s\n", why ? why : "(no reason)");
            return 1;
        }
        descriptor.state = second;
        void* ctx_b      = nullptr;
        const LdBridgeOps* ops_b = lookup(&descriptor, &ctx_b, &why);
        check(ops_b != nullptr, "the same table binds a second state");

        // Both states run their own Lua, and the two contexts are distinct even
        // though they share one binding.
        check(execute1(first) == 2.0, "the first state executes");
        check(execute1(second) == 2.0, "the second state executes");
        check(ops_b == nullptr || ctx_a != ctx_b, "each state gets its own context");

        // A different address set for the same profile is a different VM, so the
        // already-bound state must be refused instead of silently re-bound.
        std::vector<std::string> other_names;
        std::vector<LdBridgeSymbol> other =
            table_with_swap(other_names, "lua_rawseti", "lua_rawset");
        LdBridgeDescriptor conflicting = table_descriptor(first, profile, other);
        void* ctx_c        = nullptr;
        const char* why_c  = nullptr;
        const LdBridgeOps* ops_c = lookup(&conflicting, &ctx_c, &why_c);
        check(ops_c == nullptr, "rebinding a bound state to another table is refused");
        if (why_c) {
            check(std::strstr(why_c, "already bound") != nullptr,
                  "the refusal says the state is already bound");
        }
        // The refusal left the live binding untouched: the state still runs.
        check(execute1(first) == 2.0, "the refused rebind left the state intact");
        if (ops_a->running(ctx_a) == 1) {
            ops_a->stop(ctx_a);
        }
        if (ops_b && ops_b->running(ctx_b) == 1) {
            ops_b->stop(ctx_b);
        }
        ops_a->release(ctx_a);
        if (ops_b) {
            ops_b->release(ctx_b);
        }
        lua_close(first);
        lua_close(second);
        ::dlclose(handle);
        return 0;
    }

    // A table that satisfies the six "required" names but drops an entry the
    // backend calls without a null check must be refused at bind time, not crash
    // the target on the first stop.
    int bind_partial(const char* bridge_path, int profile) {
        void* handle   = nullptr;
        bind_fn lookup = load_bridge(bridge_path, handle);
        if (!lookup) {
            return 1;
        }
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);

        const char* const dropped[] = { "lua_pushstring", "lua_tolstring", "lua_checkstack" };
        for (const char* name : dropped) {
            symbols partial = own_symbols();
            partial.erase(name);
            std::vector<std::string> names;
            std::vector<LdBridgeSymbol> entries;
            for (const auto& entry : partial) {
                names.push_back(entry.first);
            }
            std::size_t index = 0;
            for (const auto& entry : partial) {
                entries.push_back(LdBridgeSymbol { names[index].c_str(), entry.second });
                ++index;
            }
            LdBridgeDescriptor descriptor = table_descriptor(L, profile, entries);
            void* ctx       = nullptr;
            const char* why = nullptr;
            const LdBridgeOps* ops = lookup(&descriptor, &ctx, &why);
            check(ops == nullptr, "a table missing an entry the backend calls cannot bind");
            check(why && std::strstr(why, name), "the refusal names the missing entry");
            check(execute1(L) == 2.0, "the VM still runs after the refusal");
        }

        lua_close(L);
        ::dlclose(handle);
        return 0;
    }

    // Failures must name what is wrong and leave the VM alone: an incomplete
    // table, and a profile the table cannot be that.
    int bind_failures(const char* bridge_path, int profile) {
        void* handle   = nullptr;
        bind_fn lookup = load_bridge(bridge_path, handle);
        if (!lookup) {
            return 1;
        }
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);

        // 1. a required entry is missing.
        {
            symbols partial = own_symbols();
            partial.erase("lua_getfield");
            std::vector<std::string> names;
            std::vector<LdBridgeSymbol> entries;
            for (const auto& entry : partial) {
                names.push_back(entry.first);
            }
            std::size_t index = 0;
            for (const auto& entry : partial) {
                entries.push_back(LdBridgeSymbol { names[index].c_str(), entry.second });
                ++index;
            }
            LdBridgeDescriptor descriptor = table_descriptor(L, profile, entries);
            void* ctx       = nullptr;
            const char* why = nullptr;
            const LdBridgeOps* ops = lookup(&descriptor, &ctx, &why);
            check(ops == nullptr, "a table without lua_getfield cannot bind");
            check(why && std::strstr(why, "lua_getfield"),
                  "the error names the missing entry");
            // A binding that never happened must not have touched the target.
            check(execute1(L) == 2.0, "the VM still runs after a refused binding");
        }

        // 2. a profile the table cannot be: a 5.4 API is not a 5.1 one.
        {
            standard_table shared;
            LdBridgeDescriptor descriptor = table_descriptor(L, 0, shared.entries);
            void* ctx       = nullptr;
            const char* why = nullptr;
            const LdBridgeOps* ops = lookup(&descriptor, &ctx, &why);
            check(ops == nullptr, "the table cannot bind as lua51");
            check(why && std::strstr(why, "lua51"),
                  "the error names the profile it failed to be");
            check(execute1(L) == 2.0, "the VM still runs after a refused profile");
        }

        lua_close(L);
        ::dlclose(handle);
        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s check-hidden|symbols [out.json]|bind|resolver ...\n",
                     argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    int result             = 2;
    if (mode == "check-hidden") {
        result = check_hidden();
    } else if (mode == "symbols") {
        result = print_symbols(argc > 2 ? argv[2] : nullptr);
    } else if ((mode == "bind" || mode == "resolver" || mode == "two-states")
               && argc >= 4) {
        const int profile = std::atoi(argv[3]);
        if (mode == "bind") {
            result = bind_table(argv[2], profile);
        } else if (mode == "resolver") {
            result = bind_resolver(argv[2], profile);
        } else {
            result = bind_two_states(argv[2], profile);
        }
    } else if (mode == "failures" && argc >= 4) {
        result = bind_failures(argv[2], std::atoi(argv[3]));
    } else if (mode == "partial" && argc >= 4) {
        result = bind_partial(argv[2], std::atoi(argv[3]));
    } else {
        std::fprintf(stderr, "bad arguments\n");
        return 2;
    }
    if (result != 0) {
        return result;
    }
    if (failures == 0) {
        std::printf("noexport host %s: all checks passed\n", mode.c_str());
        return 0;
    }
    std::fprintf(stderr, "noexport host %s: %d failure(s)\n", mode.c_str(), failures);
    return 1;
}
