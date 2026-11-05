#include "bridge/session_boot.h"

#include <map>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/json.h"
#include "common/log.h"
#include "host/loader.h"
#include "host/platform_abi.h"
#include "runtime/abi.h"
#include "worker/worker_session.h"

namespace luadebug::bridge {

    namespace runtime = luadebug::debugger::runtime;

    namespace {

        struct State {
            runtime::BoundRuntime* bound = nullptr;  // owned by the registry
            std::unique_ptr<worker::WorkerSession> session;
            LdWorkerId worker = 0;
            vm::State* target = nullptr;
            bool active       = false;

            // Messages the session produces outside a command dispatch (hook
            // events, captured output) are posted to the hub, which routes them
            // through the master like any other worker output.
            static void post(void* ctx, LdSlice message) {
                auto* self      = static_cast<State*>(ctx);
                LdStatus status = LD_STATUS_OK;
                bool forwarded  = false;
                if (self->active && self->bound && self->bound->valid()) {
                    status    = self->bound->api.post_event(self->worker, message);
                    forwarded = true;
                }
            }
        };

        // One runtime binding per process (the hub is per-process), one session
        // per target state.
        struct Registry {
            std::mutex mutex;
            runtime::BoundRuntime bound;
            std::map<vm::State*, std::unique_ptr<State>> states;
            // ABI/layout group the session is fixed to; -1 until the first
            // worker joins.
            int group = -1;

            std::mutex& lock() {
                return mutex;
            }
        };

        // Deliberately leaked. A static destructor would destroy the sessions
        // after the target VM has been closed, and destroying a session calls
        // back into Lua (the allocator restore and the registry key clear), so
        // the process aborts inside `__cxa_finalize`. The real teardown path is
        // `shutdown(target)`, which runs while the state is still alive; at
        // process exit there is nothing left to release.
        Registry& registry() {
            static Registry* instance = new Registry();
            return *instance;
        }

        bool bind_runtime(Registry& reg, std::string& error) {
            if (reg.bound.valid()) {
                return true;
            }
            const std::string module_dir = runtime::platform_module_directory(
                reinterpret_cast<const void*>(&bind_runtime)
            );
            const std::string name = runtime::runtime_library_name();
            std::string candidates[2];
            candidates[0] = module_dir;
            if (!candidates[0].empty() && candidates[0].back() != '/' && candidates[0].back() != '\\') {
                candidates[0].push_back('/');
            }
            candidates[0] += name;
            candidates[1] = module_dir;
            if (!candidates[1].empty()) {
                candidates[1] += "/../";
                candidates[1] += name;
            }
            for (const auto& path : candidates) {
                if (path.empty()) {
                    continue;
                }
                if (runtime::bind_runtime(path, reg.bound, error)) {
                    return true;
                }
            }
            if (error.empty()) {
                error = "cannot locate " + name;
            }
            return false;
        }

        void install_pumps(State& s) {
            if (!s.session) {
                return;
            }
            // While stopped, the session's stop loop asks for commands until its
            // worker disappears.
            vm::State* target = s.target;
            s.session->set_pump([target] { return pump_state(target, 100); });
            // While the target runs, commands are drained from the hook tick so
            // breakpoints set from the client take effect without a stop.
            s.session->set_idle_pump([target] { (void)pump_state(target, 0); });
        }

        LdStatus dispatch_bridge(void* ctx, LdSlice command, LdSink sink, void* sink_ctx) {
            auto* s = static_cast<State*>(ctx);
            if (!s || !s->session) {
                return LD_STATUS_NO_SESSION;
            }
            // Keep the permanent sink: hook events produced during a dispatch
            // (a stop, captured output) must not be dropped when it returns.
            s->session->set_sink(sink, sink_ctx);
            const std::string_view payload(command.data ? command.data : "", command.size);
            const LdStatus status = s->session->dispatch(payload);
            s->session->set_sink(&State::post, s);
            return status;
        }

    }  // namespace

    bool startup(vm::State* target, const BootOptions& options, std::string& error) {
        if (!target) {
            error = "no target state";
            return false;
        }
        Registry& reg = registry();

        // Binding, registration and the pump installation happen under the
        // registry lock; the handshake below must not (its pump re-enters).
        State* raw      = nullptr;
        bool has_client = false;
        {
            std::lock_guard<std::mutex> guard(reg.lock());

            if (!target->valid()) {
                error = "no target state";
                return false;
            }
            auto found = reg.states.find(target);
            if (found != reg.states.end() && found->second->active) {
                return true;
            }
            const int group = target->vm()->group();
            if (reg.group >= 0 && reg.group != group) {
                // The session is fixed to one ABI/layout group: joining a
                // different one is refused *before* any hook, allocator or
                // worker state is touched.
                error = std::string("the session is bound to VM group ") + vm::profile_name(static_cast<vm::Profile>(reg.group)) + "; this state belongs to group " + vm::profile_name(target->vm()->profile());
                return false;
            }
            if (!bind_runtime(reg, error)) {
                return false;
            }

            // A standalone host creates the session; with an adapter the session
            // already exists and this is a no-op.
            reg.bound.api.start_session(
                LdSlice { options.config_json.data(), options.config_json.size() },
                options.attach_only ? 1u : 0u
            );

            auto state     = std::make_unique<State>();
            state->bound   = &reg.bound;
            state->session = std::make_unique<worker::WorkerSession>(target);
            state->target  = target;
            raw            = state.get();

            LdBridgeApi api {};
            api.size     = sizeof(LdBridgeApi);
            api.version  = LD_RUNTIME_ABI_VERSION;
            api.ctx      = raw;
            api.dispatch = &dispatch_bridge;

            const LdStatus status = reg.bound.api.register_worker(&api, &state->worker);
            if (status != LD_STATUS_OK) {
                error = "cannot register the worker (status " + std::to_string(status) + ")";
                return false;
            }

            state->session->set_sink(&State::post, raw);
            state->session->attach_hooks();
            install_pumps(*state);
            state->active = true;
            // The group is fixed only after the join fully succeeded, so a failed
            // first attempt cannot occupy the session.
            reg.group = group;
            reg.states.emplace(target, std::move(state));

            // A session without a client address (in-process embedders, tests)
            // has nobody to configure it, so the handshake is skipped.
            luadebug::debugger::Json parsed;
            std::string parse_error;
            if (luadebug::debugger::parse_json(options.config_json, parsed, parse_error) == luadebug::debugger::ParseStatus::ok && parsed.is_object()) {
                auto it    = parsed.find("address");
                has_client = it != parsed.end() && it->is_string() && !it->get<std::string>().empty();
            }
        }

        // Handshake with the master before the debuggee runs any code: the old
        // worker suspended the target in `DBG(...)` until the session was
        // configured, and breakpoints set from the client depend on it.
        while (has_client && raw->active && !raw->session->ready()) {
            if (!pump_state(target, 100)) {
                break;
            }
        }
        return true;
    }

    void shutdown(vm::State* target) {
        Registry& reg = registry();
        std::unique_ptr<State> state;
        {
            std::lock_guard<std::mutex> guard(reg.lock());
            auto found = reg.states.find(target);
            if (found == reg.states.end()) {
                return;
            }
            state = std::move(found->second);
            reg.states.erase(found);
        }
        state->active = false;
        if (state->worker != 0 && state->bound && state->bound->valid()) {
            state->bound->api.unregister_worker(state->worker);
        }
        state->session.reset();
    }

    void shutdown_all() {
        Registry& reg = registry();
        std::vector<vm::State*> targets;
        {
            std::lock_guard<std::mutex> guard(reg.lock());
            for (const auto& entry : reg.states) {
                targets.push_back(entry.first);
            }
        }
        for (vm::State* target : targets) {
            shutdown(target);
        }
    }

    bool running(vm::State* target) {
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.lock());
        auto found = reg.states.find(target);
        return found != reg.states.end() && found->second->active;
    }

    unsigned long long worker_id(vm::State* target) {
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.lock());
        auto found = reg.states.find(target);
        return found == reg.states.end() ? 0ULL : static_cast<unsigned long long>(found->second->worker);
    }

    bool pump_state(vm::State* target, unsigned wait_ms) {
        Registry& reg                = registry();
        LdWorkerId worker            = 0;
        runtime::BoundRuntime* bound = nullptr;
        bool active                  = false;
        {
            std::lock_guard<std::mutex> guard(reg.lock());
            auto found = reg.states.find(target);
            if (found == reg.states.end() || !found->second->active) {
                return false;
            }
            worker = found->second->worker;
            bound  = found->second->bound;
            active = true;
        }
        if (!active || !bound || !bound->valid()) {
            return false;
        }
        // wait_ms == 0 is the running-target tick: whether anything was drained
        // is irrelevant, the target keeps executing either way.
        for (;;) {
            const LdStatus status = bound->api.pump_worker(worker, wait_ms);
            if (status == LD_STATUS_OK) {
                return true;
            }
            if (status != LD_STATUS_IDLE) {
                return false;
            }
            if (wait_ms == 0) {
                return true;
            }
        }
    }

    bool pump(unsigned wait_ms) {
        Registry& reg = registry();
        std::vector<vm::State*> targets;
        {
            std::lock_guard<std::mutex> guard(reg.lock());
            for (const auto& entry : reg.states) {
                targets.push_back(entry.first);
            }
        }
        if (targets.empty()) {
            return false;
        }
        bool any = false;
        for (vm::State* target : targets) {
            any = pump_state(target, wait_ms) || any;
        }
        return any;
    }

}  // namespace luadebug::bridge
