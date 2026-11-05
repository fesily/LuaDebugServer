// Minimal bridge stand-in for the runtime-sharing proof.
//
// It has no Lua inside on purpose: what is being verified is the contract that
// two independently loaded bridges (the published `luadebug.so`
// files) join the *same* runtime session hub, get distinct worker ids, and can
// be registered/unregistered independently.
//
// Built twice; the two copies take their identity from LUA_DEBUG_SHIM_NAME.

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/abi.h"

// The name comes from the environment: passing it as a -D string literal through
// the build's shell quoting is fragile.

namespace {

    LdRuntimeApi g_api {};
    LdWorkerId g_worker = 0;
    void* g_runtime = nullptr;

    const char* shim_name() {
        const char* name = std::getenv("LUA_DEBUG_SHIM_NAME");
        return name && *name ? name : "shim";
    }

    struct State {
        const char* name = shim_name();
        int dispatched   = 0;
        std::string last_command;
    } g_state;

    LdStatus shim_dispatch(void* ctx, LdSlice command, LdSink sink, void* sink_ctx) {
        auto* state = static_cast<State*>(ctx);
        state->dispatched += 1;
        state->last_command.assign(command.data ? command.data : "", command.size);

        std::string response = "{\"type\":\"response\",\"success\":true,\"command\":\"probe\",";
        response += "\"worker\":\"";
        response += state->name;
        response += "\"}";
        sink(sink_ctx, LdSlice { response.data(), response.size() });
        return LD_STATUS_OK;
    }

} // namespace

// The build hides symbols by default; the host resolves these by name.
#if defined(_WIN32)
#    define SHIM_EXPORT __declspec(dllexport)
#else
#    define SHIM_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

    // Loads the runtime (path from LUA_DEBUG_RUNTIME), starts/reuses the
    // session and registers this bridge. Returns its worker id, 0 on failure.
    SHIM_EXPORT LdWorkerId ld_shim_start() {
        const char* runtime_path = std::getenv("LUA_DEBUG_RUNTIME");
        if (!runtime_path) {
            std::fprintf(stderr, "[%s] LUA_DEBUG_RUNTIME not set\n", shim_name());
            return 0;
        }
        g_runtime = ::dlopen(runtime_path, RTLD_NOW | RTLD_LOCAL);
        if (!g_runtime) {
            std::fprintf(stderr, "[%s] dlopen(%s): %s\n", shim_name(), runtime_path, ::dlerror());
            return 0;
        }
        auto get_api = reinterpret_cast<LdStatus (*)(uint32_t, LdRuntimeApi*)>(
            ::dlsym(g_runtime, "luadebug_runtime_get_api"));
        if (!get_api) {
            return 0;
        }
        g_api.size    = sizeof(LdRuntimeApi);
        g_api.version = LD_RUNTIME_ABI_VERSION;
        if (get_api(LD_RUNTIME_ABI_VERSION, &g_api) != LD_STATUS_OK) {
            return 0;
        }
        std::string config = "{\"name\":\"two-bridge-test\"}";
        if (g_api.start_session(LdSlice { config.data(), config.size() }, 0) != LD_STATUS_OK) {
            return 0;
        }
        LdBridgeApi bridge {};
        bridge.size     = sizeof(LdBridgeApi);
        bridge.version  = LD_RUNTIME_ABI_VERSION;
        bridge.ctx      = &g_state;
        bridge.dispatch = &shim_dispatch;
        if (g_api.register_worker(&bridge, &g_worker) != LD_STATUS_OK) {
            g_worker = 0;
        }
        return g_worker;
    }

    SHIM_EXPORT uint64_t ld_shim_worker() {
        return g_worker;
    }

    SHIM_EXPORT const char* ld_shim_name() {
        return shim_name();
    }

    SHIM_EXPORT int ld_shim_started() {
        return g_worker != 0;
    }

    // Exercises the worker's own pump path (used to prove per-worker isolation
    // once the master can enqueue; here it must simply not dispatch anything).
    SHIM_EXPORT int ld_shim_pump() {
        if (!g_worker) {
            return -1;
        }
        return static_cast<int>(g_api.pump_worker(g_worker, 0));
    }

    SHIM_EXPORT int ld_shim_dispatched() {
        return g_state.dispatched;
    }

    SHIM_EXPORT int ld_shim_register_again(uint64_t* out) {
        if (!g_api.register_worker) {
            return -1;
        }
        LdBridgeApi bridge {};
        bridge.size     = sizeof(LdBridgeApi);
        bridge.version  = LD_RUNTIME_ABI_VERSION;
        bridge.ctx      = &g_state;
        bridge.dispatch = &shim_dispatch;
        return static_cast<int>(g_api.register_worker(&bridge, out));
    }

    SHIM_EXPORT int ld_shim_unregister() {
        if (!g_worker) {
            return -1;
        }
        auto status = g_api.unregister_worker(g_worker);
        g_worker    = 0;
        return static_cast<int>(status);
    }

} // extern "C"
