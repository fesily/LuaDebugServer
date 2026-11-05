// Two Lua states of the same ABI group in one process, one bridge file, one
// session hub.
//
// This is the multi-VM contract a service-per-state runtime (skynet-style) needs:
// each state registers its own worker, commands are routed to the right one, and
// shutting one down leaves the other registered. The bridge is loaded from disk
// (never linked) and each state enters it through the same profile entry point,
// so the test exercises the boundary a real host uses.

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <lua.hpp>

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    struct Bridge {
        void* handle = nullptr;
        using open_fn = int (*)(lua_State*);
        open_fn open  = nullptr;

        bool load(const char* path, const char* entry) {
            handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                std::fprintf(stderr, "dlopen %s: %s\n", path, ::dlerror());
                return false;
            }
            open = reinterpret_cast<open_fn>(::dlsym(handle, entry));
            return open != nullptr;
        }
    };

    // Calls a function from the module table with the given arguments.
    bool call_start(lua_State* L, const char* config, bool attach_only) {
        lua_getfield(L, -1, "start");
        if (!lua_isfunction(L, -1)) {
            return false;
        }
        lua_pushlstring(L, config, std::strlen(config));
        lua_pushboolean(L, attach_only ? 1 : 0);
        if (lua_pcall(L, 2, 1, 0) != 0) {
            std::fprintf(stderr, "start failed: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        const bool ok = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return ok;
    }

    bool call_bool(lua_State* L, const char* name) {
        lua_getfield(L, -1, name);
        if (!lua_isfunction(L, -1)) {
            return false;
        }
        if (lua_pcall(L, 0, 1, 0) != 0) {
            lua_pop(L, 1);
            return false;
        }
        const bool ok = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return ok;
    }

    unsigned long long call_worker_id(lua_State* L) {
        lua_getfield(L, -1, "worker_id");
        if (!lua_isfunction(L, -1)) {
            return 0;
        }
        if (lua_pcall(L, 0, 1, 0) != 0) {
            lua_pop(L, 1);
            return 0;
        }
        const unsigned long long id = static_cast<unsigned long long>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return id;
    }

    // Runs a script file in `L`: a breakpoint in it goes through the bridge's own
    // hook, so the harness can prove which worker stopped.
    bool run_file(lua_State* L, const char* path) {
        if (luaL_loadfile(L, path) != 0) {
            std::fprintf(stderr, "load %s: %s\n", path, lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::fprintf(stderr, "run %s: %s\n", path, lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    void call_stop(lua_State* L) {
        lua_getfield(L, -1, "stop");
        if (lua_isfunction(L, -1) && lua_pcall(L, 0, 0, 0) != 0) {
            lua_pop(L, 1);
        }
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s BRIDGE [--serve ADDRESS SCRIPT_A SCRIPT_B]\n", argv[0]);
        return 2;
    }

    // Serve mode: both states join one session whose DAP client connects to
    // `ADDRESS`, then each state runs its own script. This is the mixed-VM case a
    // real client sees: two workers of one group, routed by worker id.
    const bool serve = argc > 3 && std::strcmp(argv[2], "--serve") == 0;
    if (serve && argc < 6) {
        std::fprintf(stderr, "usage: %s BRIDGE --serve ADDRESS SCRIPT_A SCRIPT_B\n", argv[0]);
        return 2;
    }

    Bridge bridge;
    if (!bridge.load(argv[1], "luaopen_luadebug_lua54")) {
        std::fprintf(stderr, "cannot load the bridge or its lua54 entry\n");
        return 1;
    }

    lua_State* first  = luaL_newstate();
    lua_State* second = luaL_newstate();
    luaL_openlibs(first);
    luaL_openlibs(second);

    if (serve) {
        // The session listens; the debugger connects to it.
        std::string config = std::string("{\"luaVersion\":\"lua54\",\"client\":false,\"address\":\"")
                           + argv[3] + "\"}";
        lua_pushcfunction(first, bridge.open);
        if (lua_pcall(first, 0, 1, 0) != 0 || !lua_istable(first, -1)) {
            std::fprintf(stderr, "cannot open the module table: %s\n", lua_tostring(first, -1));
            return 1;
        }
        // The first join blocks until the client configured the session, so the
        // scripts below only run once breakpoints exist.
        if (!call_start(first, config.c_str(), false)) {
            return 1;
        }
        lua_pushcfunction(second, bridge.open);
        if (lua_pcall(second, 0, 1, 0) != 0 || !lua_istable(second, -1)
            || !call_start(second, config.c_str(), false)) {
            return 1;
        }
        const bool ok_a = run_file(first, argv[4]);
        const bool ok_b = run_file(second, argv[5]);
        call_stop(first);
        call_stop(second);
        lua_close(first);
        lua_close(second);
        return (ok_a && ok_b) ? 0 : 1;
    }

    // No client address: an in-process embedder has nobody to configure the
    // session, so the handshake is skipped and both workers just register.
    const char* config = "{\"luaVersion\":\"lua54\"}";

    lua_pushcfunction(first, bridge.open);
    expect(lua_pcall(first, 0, 1, 0) == 0, "the module table opens in the first state");
    expect(lua_istable(first, -1), "the entry returns a module table");
    expect(call_start(first, config, false), "the first state joins the session");

    lua_pushcfunction(second, bridge.open);
    expect(lua_pcall(second, 0, 1, 0) == 0, "the module table opens in the second state");
    expect(call_start(second, config, false), "the second state joins the same session");

    const unsigned long long id1 = call_worker_id(first);
    const unsigned long long id2 = call_worker_id(second);
    expect(id1 != 0, "the first state has a worker id");
    expect(id2 != 0, "the second state has a worker id");
    expect(id1 != id2, "the two states are separate workers");
    expect(call_bool(first, "running"), "the first worker is running");
    expect(call_bool(second, "running"), "the second worker is running");

    // Coroutines and a full collection in each VM, through real Lua code: a
    // debugged state must behave exactly like an undebugged one.
    const char* churn =
        "local co = coroutine.create(function(a) local b = a + 1 coroutine.yield(b)"
        " local c = b * 2 return c end)"
        " local ok1, v1 = coroutine.resume(co, 20)"
        " assert(ok1 and v1 == 21, 'first resume')"
        " local ok2, v2 = coroutine.resume(co)"
        " assert(ok2 and v2 == 42, 'second resume')"
        " collectgarbage('collect') collectgarbage('collect')"
        " RESULT = v2";
    expect(luaL_dostring(first, churn) == 0, "the first VM runs coroutines and GC");
    expect(luaL_dostring(second, churn) == 0, "the second VM runs coroutines and GC");
    lua_getglobal(first, "RESULT");
    expect(lua_tointeger(first, -1) == 42, "the first VM produced its own result");
    lua_pop(first, 1);
    lua_getglobal(second, "RESULT");
    expect(lua_tointeger(second, -1) == 42, "the second VM produced its own result");
    lua_pop(second, 1);

    // Stopping one leaves the other bound to the session.
    call_stop(first);
    expect(!call_bool(first, "running"), "the stopped worker is gone");
    expect(call_bool(second, "running"), "the other worker survives");

    // The surviving VM keeps working after the other worker was torn down.
    expect(luaL_dostring(second, churn) == 0, "the surviving VM still runs Lua");
    lua_getglobal(second, "RESULT");
    expect(lua_tointeger(second, -1) == 42, "the surviving VM still returns its result");
    lua_pop(second, 1);

    call_stop(second);
    lua_close(first);
    lua_close(second);

    if (failures == 0) {
        std::printf("two_lua_vms_test: OK\n");
        return 0;
    }
    std::printf("two_lua_vms_test: %d failure(s)\n", failures);
    return 1;
}
