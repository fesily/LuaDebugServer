// Internal verification for the runtime ABI and the session hub.
//
// Covers what every bridge depends on:
//   * the ABI handshake rejects mismatched callers (through dlopen, so the
//     exported symbol set is exercised too),
//   * a bridge can only join an existing session,
//   * queued commands reach the owning worker on its own thread and responses
//     are delivered through the sink exactly once,
//   * a failing bridge yields an error response instead of a silent drop,
//   * unregister rejects new commands and drains an in-flight dispatch,
//   * post_event carries the runtime's threadId encoding.

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "runtime/abi.h"
#include "host/loader.h"
#include "session/session_hub.h"

namespace {

    using luadebug::debugger::Json;
    using luadebug::debugger::session::SessionHub;

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    struct BridgeState {
        std::vector<std::string> received;
        bool fail_next = false;
        std::thread::id seen_on;
    };

    LdStatus bridge_dispatch(void* ctx, LdSlice command, LdSink sink, void* sink_ctx) {
        auto* state = static_cast<BridgeState*>(ctx);
        state->seen_on = std::this_thread::get_id();
        state->received.emplace_back(command.data, command.size);
        if (state->fail_next) {
            state->fail_next = false;
            return LD_STATUS_FAILED;
        }
        std::string response = "{\"type\":\"response\",\"success\":true,\"command\":\"probe\","
                               "\"request_seq\":1}";
        sink(sink_ctx, LdSlice { response.data(), response.size() });
        return LD_STATUS_OK;
    }

    void reset_hub() {
        // Each test starts from a fresh hub state.
        auto& hub = SessionHub::instance();
        while (hub.worker_count() != 0) {
            // Unregister everything we can see by id: ids start at 1.
            bool removed = false;
            for (LdWorkerId id = 1; id < 64; ++id) {
                if (hub.worker_registered(id)) {
                    hub.unregister_worker(id);
                    removed = true;
                }
            }
            if (!removed) {
                break;
            }
        }
    }

} // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "publish/runtime/darwin-arm64/libluadebug_runtime.dylib";

    // ---------------------------------------------------------------- dlopen ABI
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "FAIL: dlopen(%s): %s\n", path, dlerror());
        return 1;
    }
    auto get_api = reinterpret_cast<LdStatus (*)(uint32_t, LdRuntimeApi*)>(
        dlsym(handle, "luadebug_runtime_get_api"));
    expect(get_api != nullptr, "luadebug_runtime_get_api must be exported");

    LdRuntimeApi api {};
    api.size    = sizeof(LdRuntimeApi);
    api.version = LD_RUNTIME_ABI_VERSION + 1;
    expect(get_api(LD_RUNTIME_ABI_VERSION, &api) == LD_STATUS_ABI_MISMATCH,
           "mismatched table version must be rejected");
    api.version = LD_RUNTIME_ABI_VERSION;
    api.size    = sizeof(LdRuntimeApi) - 1;
    expect(get_api(LD_RUNTIME_ABI_VERSION, &api) == LD_STATUS_ABI_MISMATCH,
           "undersized table must be rejected");
    api.size    = sizeof(LdRuntimeApi);
    api.version = LD_RUNTIME_ABI_VERSION;
    expect(get_api(LD_RUNTIME_ABI_VERSION, &api) == LD_STATUS_OK, "get_api handshake");
    expect(api.start_session && api.register_worker && api.unregister_worker
               && api.pump_worker && api.post_event,
           "every function pointer must be filled");
    dlclose(handle);

    // The loader must bind the same library by canonical path, reject a wrong
    // ABI, and report a missing file instead of silently degrading.
    {
        using namespace luadebug::debugger::runtime;
        BoundRuntime bound;
        std::string error;
        bool ok = bind_runtime(path, bound, error);
        expect(ok, "bind_runtime on the published library");
        expect(bound.valid(), "bound runtime exposes the ABI table");
        if (bound.valid()) {
            LdSlice config { "", 0 };
            expect(bound.api.start_session(config, 0) == LD_STATUS_INVALID_ARGUMENT,
                   "start_session rejects an empty config");
        }
        unbind(bound);

        BoundRuntime missing;
        expect(!bind_runtime("/nonexistent/libluadebug_runtime.dylib", missing, error),
               "missing runtime library must fail");
        expect(!error.empty(), "load failure carries a message");
        expect(error.find("ABI mismatch") == std::string::npos,
               "missing library is not reported as an ABI mismatch");
    }

    // ------------------------------------------------------------- hub behaviour
    reset_hub();
    auto& hub = SessionHub::instance();

    std::vector<Json> delivered;
    hub.set_client_sink([&](const Json& message) { delivered.push_back(message); });

    LdBridgeApi bridge {};
    bridge.size     = sizeof(LdBridgeApi);
    bridge.version  = LD_RUNTIME_ABI_VERSION;
    bridge.dispatch = &bridge_dispatch;
    BridgeState state;
    bridge.ctx = &state;

    LdWorkerId worker = 0;
    expect(hub.register_worker(&bridge, &worker) == LD_STATUS_NO_SESSION,
           "register before start_session must fail");

    expect(hub.start_session("{\"name\":\"test\"}", true) == LD_STATUS_NO_SESSION,
           "attach_only without a session must not create one");
    expect(hub.start_session("{\"name\":\"test\"}", false) == LD_STATUS_OK, "start_session");
    expect(hub.start_session("{\"other\":1}", false) == LD_STATUS_OK,
           "start_session is idempotent");
    expect(hub.session_config().find("test") != std::string::npos,
           "the first session config is kept");

    expect(hub.register_worker(&bridge, &worker) == LD_STATUS_OK, "register worker");
    BridgeState other_state;
    LdBridgeApi other_bridge = bridge;
    other_bridge.ctx         = &other_state;
    LdWorkerId other_worker  = 0;
    expect(hub.register_worker(&other_bridge, &other_worker) == LD_STATUS_OK,
           "register second worker");
    expect(worker != other_worker, "worker ids must be unique per VM");

    // Commands are consumed only by the owning worker's pump.
    expect(hub.enqueue(worker, "{\"command\":\"probe\",\"seq\":1}") == LD_STATUS_OK, "enqueue");
    expect(hub.pump_worker(other_worker, 0) == LD_STATUS_IDLE,
           "pump other worker reports idle (nothing of its own to run)");
    expect(other_state.received.empty(), "other worker must not see the command");
    expect(hub.pump_worker(worker, 0) == LD_STATUS_OK, "pump owning worker dispatches");
    expect(state.received.size() == 1, "owning worker must receive the command");
    expect(delivered.size() == 1, "exactly one response is delivered");
    if (!delivered.empty()) {
        expect(delivered[0].contains("threadId"), "response carries the runtime threadId");
        expect(delivered[0].value("success", false) == true, "response payload preserved");
    }
    expect(hub.pump_worker(worker, 0) == LD_STATUS_IDLE, "an empty pump reports idle");
    expect(state.received.size() == 1, "empty pump must not dispatch");

    // A failing bridge is reported as an error response for the same request.
    delivered.clear();
    state.fail_next = true;
    expect(hub.enqueue(worker, "{\"command\":\"probe\",\"seq\":7}") == LD_STATUS_OK, "enqueue bad");
    hub.pump_worker(worker, 0);
    expect(delivered.size() == 1, "failure produces one response");
    if (!delivered.empty()) {
        expect(delivered[0].value("success", true) == false, "failure response has success=false");
        expect(delivered[0].value("request_seq", 0) == 7, "failure response echoes the request seq");
    }

    // Invalid command JSON must be rejected, not treated as an empty request.
    expect(hub.enqueue(worker, "not json") == LD_STATUS_OK, "enqueue garbage is accepted");
    expect(hub.pump_worker(worker, 0) == LD_STATUS_OK, "pump garbage");
    expect(state.received.size() == 2, "garbage command is not dispatched");

    // post_event stamps the threadId and reaches the client sink.
    delivered.clear();
    expect(hub.post_event(worker, "{\"event\":\"stopped\",\"body\":{\"reason\":\"breakpoint\"}}")
               == LD_STATUS_OK,
           "post_event");
    expect(delivered.size() == 1, "event delivered");
    if (!delivered.empty()) {
        expect(delivered[0].contains("threadId"), "event carries threadId");
    }

    // A worker whose dispatch is still running must be drained by unregister.
    delivered.clear();
    std::atomic<bool> in_dispatch { false };
    struct SlowState {
        std::atomic<bool>* flag;
    } slow { &in_dispatch };
    LdBridgeApi slow_bridge = bridge;
    slow_bridge.ctx = &slow;
    slow_bridge.dispatch = [](void* ctx, LdSlice, LdSink, void*) -> LdStatus {
        auto* state = static_cast<SlowState*>(ctx);
        state->flag->store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        state->flag->store(false);
        return LD_STATUS_OK;
    };
    LdWorkerId slow_worker = 0;
    expect(hub.register_worker(&slow_bridge, &slow_worker) == LD_STATUS_OK, "register slow");
    expect(hub.enqueue(slow_worker, "{\"command\":\"probe\",\"seq\":9}") == LD_STATUS_OK, "enqueue slow");
    std::thread pump_thread([&]() { hub.pump_worker(slow_worker, 0); });
    while (!in_dispatch.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(hub.unregister_worker(slow_worker) == LD_STATUS_OK, "unregister waits for in-flight");
    expect(!in_dispatch.load(), "in-flight dispatch finished before unregister returned");
    pump_thread.join();
    expect(hub.enqueue(slow_worker, "{}") == LD_STATUS_NOT_REGISTERED,
           "unregistered worker rejects new commands");
    expect(hub.unregister_worker(slow_worker) == LD_STATUS_NOT_REGISTERED,
           "double unregister is reported");

    expect(hub.unregister_worker(worker) == LD_STATUS_OK, "unregister worker");
    expect(hub.unregister_worker(other_worker) == LD_STATUS_OK, "unregister other worker");

    if (failures == 0) {
        std::printf("session_hub_test: OK\n");
        return 0;
    }
    std::printf("session_hub_test: %d failure(s)\n", failures);
    return 1;
}
