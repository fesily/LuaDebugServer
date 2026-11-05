#include "session/session_hub.h"

#include <cstdlib>
#include <thread>

#include "common/log.h"

namespace luadebug::debugger::session {

    namespace {

        // Reference-id allocation is part of the runtime so every bridge encodes
        // ids the same way: (threadId << 24) | local, (threadId << 32) | source.
        constexpr int kThreadIdShift = 24;

        std::string json_string_field(const Json& object, const char* key) {
            if (!object.is_object()) {
                return {};
            }
            auto it = object.find(key);
            if (it == object.end() || !it->is_string()) {
                return {};
            }
            return it->get<std::string>();
        }

        long thread_id_of(LdWorkerId worker) {
            return static_cast<long>(worker);
        }

    }  // namespace

    SessionHub::~SessionHub() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
        }
        outbox_cv_.notify_all();
        if (session_thread_.joinable()) {
            if (session_thread_.get_id() == std::this_thread::get_id()) {
                session_thread_.detach();
            } else {
                session_thread_.join();
            }
        }
        {
            std::lock_guard<std::mutex> guard(channel_mutex_);
            if (channel_) {
                flush_outbox_locked();
                channel_->flush();
                channel_->close();
            }
        }
    }

    SessionHub& SessionHub::instance() {
        static SessionHub hub;
        return hub;
    }

    LdStatus SessionHub::start_session(std::string_view config_json, bool attach_only) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (active_) {
            // Idempotent: keep the first config, matching the Lua master's
            // "config is replayed from the cached initialize request" rule.
            return LD_STATUS_OK;
        }
        if (attach_only) {
            return LD_STATUS_NO_SESSION;
        }
        Json parsed;
        std::string error;
        if (parse_json(config_json, parsed, error) != ParseStatus::ok) {
            log_error("start_session: invalid config json: %s", error.c_str());
            return LD_STATUS_INVALID_ARGUMENT;
        }
        active_ = true;

        // The master owns the DAP conversation; the transport is optional (a
        // session can also be driven in-process by tests or by another host).
        // The connect/listen choice must be read before resolve_session_config():
        // that function defaults `client` to true whenever an address is set,
        // while the script's own flag is what selects the direction.
        const bool client_mode = parsed.is_object() && parsed.contains("client") && parsed["client"].is_boolean() && parsed["client"].get<bool>();
        Json session_config    = parsed;
        if (session_config.contains("initialize") && session_config["initialize"].is_object()) {
            // `start_session` may be handed either the bare launch config or the
            // already-wrapped one the bridges cache.
            resolve_session_config(session_config["initialize"]);
        } else {
            resolve_session_config(session_config);
        }
        config_ = dump_json(session_config);

        Master::Router router;
        router.to_worker = [this](LdWorkerId worker, const Json& command) {
            return enqueue(worker, dump_json(command)) == LD_STATUS_OK;
        };
        router.to_all = [this](const Json& command) {
            const std::string payload = dump_json(command);
            std::vector<LdWorkerId> ids;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (const auto& entry : workers_) {
                    ids.push_back(entry.first);
                }
            }
            for (LdWorkerId id : ids) {
                enqueue(id, payload);
            }
        };
        router.to_all_except = [this](LdWorkerId skip, const Json& command) {
            const std::string payload = dump_json(command);
            std::vector<LdWorkerId> ids;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (const auto& entry : workers_) {
                    if (entry.first != skip) {
                        ids.push_back(entry.first);
                    }
                }
            }
            for (LdWorkerId id : ids) {
                enqueue(id, payload);
            }
        };
        router.finish = [this](bool terminate) { finish(terminate); };
        master_       = std::make_unique<Master>(
            [this](const Json& message) { write_client(message); }, std::move(router)
        );

        // The client address is optional; when present the runtime serves DAP
        // itself instead of relying on a worker-side Lua master.
        std::string address = json_string_field(session_config, "address");
        if (!address.empty()) {
            std::string spec = client_mode ? "connect:" : "listen:";
            spec += address;
            std::string error;
            channel_ = transport::open_socket_channel(spec, error);
            has_transport_.store(channel_ != nullptr);
            if (!channel_) {
                log_error("start_session: cannot open %s: %s", spec.c_str(), error.c_str());
            }
        }
        start_session_thread();
        return LD_STATUS_OK;
    }

    void SessionHub::write_client(const Json& message) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!channel_) {
                // No transport: an in-process sink (tests, embedders) is called
                // directly.
                client_sink sink = sink_;
                if (sink) {
                    sink(message);
                }
                return;
            }
        }
        {
            std::lock_guard<std::mutex> guard(outbox_mutex_);
            outbox_.push_back(message);
        }
        outbox_cv_.notify_all();
    }

    void SessionHub::flush_outbox_locked() {
        std::deque<Json> pending;
        {
            std::lock_guard<std::mutex> guard(outbox_mutex_);
            pending.swap(outbox_);
        }
        for (const auto& message : pending) {
            channel_->send(message);
        }
        if (!pending.empty()) {
            channel_->flush();
        }
    }

    void SessionHub::handle_client_message(const Json& message) {
        if (!master_) {
            return;
        }
        master_->handle_client_message(message);
    }

    void SessionHub::session_loop() {
        while (true) {
            bool closing = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                closing = closing_;
            }
            if (closing) {
                break;
            }
            // The transport belongs to this thread: poll, read and write here.
            std::deque<Json> incoming;
            bool closed = false;
            {
                std::lock_guard<std::mutex> guard(channel_mutex_);
                if (!channel_) {
                    // No transport (in-process embedder, tests): the inbox is
                    // still drained here so worker events reach the master.
                    drain_worker_inbox();
                    std::unique_lock<std::mutex> lock(inbox_mutex_);
                    inbox_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return !worker_inbox_.empty() || closing_; });
                    continue;
                }
                flush_outbox_locked();
                channel_->update(10);
                for (;;) {
                    Json message;
                    std::string error;
                    if (!channel_->recv(message, error)) {
                        if (!error.empty()) {
                            log_error("session: %s", error.c_str());
                        }
                        break;
                    }
                    incoming.push_back(std::move(message));
                }
                closed = channel_->closed();
            }
            // Worker events wait for this thread: never touch the master (or the
            // transport) from a target thread.
            drain_worker_inbox();
            for (auto& message : incoming) {
                if (master_) {
                    master_->handle_client_message(message);
                }
            }
            if (closed) {
                break;
            }
        }
        drain_worker_inbox();
        // Final drain: a debuggee can exit right after its last event.
        std::lock_guard<std::mutex> guard(channel_mutex_);
        if (channel_) {
            for (int i = 0; i < 20; ++i) {
                flush_outbox_locked();
                bool empty = false;
                {
                    std::lock_guard<std::mutex> inner(outbox_mutex_);
                    empty = outbox_.empty();
                }
                if (empty) {
                    break;
                }
                channel_->update(1);
            }
        }
    }

    void SessionHub::start_session_thread() {
        if (!channel_ || session_thread_.joinable()) {
            return;
        }
        session_thread_ = std::thread([this] { session_loop(); });
    }

    void SessionHub::finish(bool terminate_debuggee) {
        {
            Json terminated     = Json::object();
            terminated["type"]  = "event";
            terminated["event"] = "terminated";
            terminated["seq"]   = 0;
            Json body           = Json::object();
            body["restart"]     = false;
            terminated["body"]  = std::move(body);
            write_client(terminated);
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
        }
        // Writes are synchronous, so only the poller has to be stopped.
        if (session_thread_.joinable() && session_thread_.get_id() != std::this_thread::get_id()) {
            session_thread_.join();
        }
        {
            std::lock_guard<std::mutex> guard(channel_mutex_);
            if (channel_) {
                channel_->flush();
                channel_->close();
            }
        }
        if (terminate_debuggee) {
            // Matches the Lua master's os.exit(true, true): everything the client
            // is owed has been flushed above.
            std::_Exit(0);
        }
    }

    bool SessionHub::has_session() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return active_;
    }

    std::string SessionHub::session_config() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return config_;
    }

    void SessionHub::set_client_sink(client_sink sink) {
        std::lock_guard<std::mutex> guard(mutex_);
        sink_ = std::move(sink);
    }

    bool SessionHub::emit(const Json& message) {
        client_sink sink;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            sink = sink_;
        }
        if (!sink) {
            return false;
        }
        sink(message);
        return true;
    }

    LdStatus SessionHub::register_worker(const LdBridgeApi* bridge, LdWorkerId* out) {
        if (!bridge || !out || bridge->size < sizeof(LdBridgeApi) || bridge->version != LD_RUNTIME_ABI_VERSION || !bridge->dispatch) {
            return LD_STATUS_INVALID_ARGUMENT;
        }
        LdWorkerId id = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!active_) {
                // A bridge may only join an existing session: registering into a
                // fresh hub would leave commands with nowhere to go.
                return LD_STATUS_NO_SESSION;
            }
            auto entry          = std::make_shared<WorkerEntry>();
            entry->bridge       = *bridge;
            entry->id           = next_worker_id_++;
            workers_[entry->id] = entry;
            *out                = entry->id;
            id                  = entry->id;
        }
        // Outside the hub lock: the master enqueues the initialization commands,
        // and enqueue() takes the same lock.
        if (master_) {
            master_->worker_ready(id);
        }
        return LD_STATUS_OK;
    }

    LdStatus SessionHub::unregister_worker(LdWorkerId worker) {
        std::shared_ptr<WorkerEntry> entry;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = workers_.find(worker);
            if (it == workers_.end()) {
                return LD_STATUS_NOT_REGISTERED;
            }
            entry             = it->second;
            entry->closing    = true;
            entry->registered = false;
            entry->commands.clear();
            workers_.erase(it);
            worker_cv_.notify_all();
        }
        entry->wake.notify_all();
        if (master_) {
            master_->worker_gone(worker);
        }

        // Wait for an in-flight dispatch to finish, unless we *are* that thread
        // (a VM closing from inside its own hook must not deadlock).
        for (;;) {
            bool in_flight = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                in_flight = entry->in_flight != 0;
            }
            if (!in_flight) {
                break;
            }
            if (entry->pump_thread == std::this_thread::get_id()) {
                log_warn("unregister_worker(%llu) from its own pump thread", static_cast<unsigned long long>(worker));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return LD_STATUS_OK;
    }

    LdStatus SessionHub::enqueue(LdWorkerId worker, std::string command_json) {
        std::shared_ptr<WorkerEntry> entry;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = workers_.find(worker);
            if (it == workers_.end() || !it->second->registered) {
                return LD_STATUS_NOT_REGISTERED;
            }
            entry = it->second;
            entry->commands.push_back(std::move(command_json));
        }
        entry->wake.notify_all();
        return LD_STATUS_OK;
    }

    LdStatus SessionHub::dispatch_once(std::shared_ptr<WorkerEntry> entry, std::string command) {
        Json request;
        std::string error;
        if (parse_json(command, request, error) != ParseStatus::ok) {
            // Bad command JSON is a programming error on the control side; it
            // must not be silently dropped as an empty request.
            log_error("worker %llu: invalid command json: %s", static_cast<unsigned long long>(entry->id), error.c_str());
            return LD_STATUS_INVALID_ARGUMENT;
        }

        {
            std::lock_guard<std::mutex> guard(mutex_);
            entry->in_flight += 1;
            entry->pump_thread = std::this_thread::get_id();
        }

        struct SinkState {
            SessionHub* hub   = nullptr;
            LdWorkerId worker = 0;
            long thread_id    = 0;
        } state { this, entry->id, thread_id_of(entry->id) };

        auto sink = [](void* ctx, LdSlice message) {
            auto* st = static_cast<SinkState*>(ctx);
            Json payload;
            std::string error;
            if (parse_json(std::string_view(message.data, message.size), payload, error) != ParseStatus::ok) {
                log_error("worker %llu: invalid response json: %s", static_cast<unsigned long long>(st->worker), error.c_str());
                return;
            }
            // Worker output is master input: it re-encodes references and turns
            // the worker's `cmd` messages into DAP events/responses. The runtime
            // still owns the threadId so bridges never build it.
            if (!payload.contains("threadId")) {
                payload["threadId"] = st->thread_id;
            }
            // Same rule as post_event: the master belongs to the session thread.
            st->hub->queue_worker_message(st->worker, std::move(payload));
        };

        LdSlice slice { command.data(), command.size() };
        LdStatus status = LD_STATUS_FAILED;
        try {
            status = entry->bridge.dispatch(entry->bridge.ctx, slice, sink, &state);
        } catch (...) {
            // C++ exceptions must never cross the C ABI.
            log_error("worker %llu: bridge threw during dispatch", static_cast<unsigned long long>(entry->id));
            status = LD_STATUS_FAILED;
        }

        {
            std::lock_guard<std::mutex> guard(mutex_);
            entry->in_flight -= 1;
        }

        // An in-process embedder has no session thread to hand worker output to
        // (the sink is called on the pumping thread, which is what its callers
        // expect), so drain here. With a transport the session thread does it.
        if (!has_transport_.load(std::memory_order_acquire)) {
            drain_worker_inbox();
        }

        if (status < 0) {
            Json failure           = Json::object();
            failure["type"]        = "response";
            failure["success"]     = false;
            failure["command"]     = get_string(request, "command");
            failure["request_seq"] = get_int(request, "seq", 0);
            failure["message"]     = "worker dispatch failed";
            failure["threadId"]    = state.thread_id;
            emit(failure);
        }
        (void)kThreadIdShift;
        return status;
    }

    LdStatus SessionHub::pump_worker(LdWorkerId worker, uint32_t wait_ms) {
        std::shared_ptr<WorkerEntry> entry;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = workers_.find(worker);
            if (it == workers_.end()) {
                return LD_STATUS_NOT_REGISTERED;
            }
            entry              = it->second;
            entry->pump_thread = std::this_thread::get_id();
        }

        std::string command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (entry->commands.empty()) {
                if (wait_ms == 0) {
                    return LD_STATUS_IDLE;
                }
                entry->wake.wait_for(lock, std::chrono::milliseconds(wait_ms), [&]() {
                    return !entry->commands.empty() || entry->closing;
                });
            }
            if (entry->closing) {
                return LD_STATUS_NOT_REGISTERED;
            }
            if (entry->commands.empty()) {
                return LD_STATUS_IDLE;
            }
            command = std::move(entry->commands.front());
            entry->commands.pop_front();
        }

        dispatch_once(entry, std::move(command));
        return LD_STATUS_OK;
    }

    void SessionHub::queue_worker_message(LdWorkerId worker, Json payload) {
        {
            std::lock_guard<std::mutex> guard(inbox_mutex_);
            worker_inbox_.emplace_back(worker, std::move(payload));
        }
        inbox_cv_.notify_all();
    }

    void SessionHub::drain_worker_inbox() {
        std::deque<std::pair<LdWorkerId, Json>> pending;
        {
            std::lock_guard<std::mutex> guard(inbox_mutex_);
            pending.swap(worker_inbox_);
        }
        for (const auto& entry : pending) {
            if (master_) {
                master_->handle_worker_message(entry.first, entry.second);
            }
        }
    }

    LdStatus SessionHub::post_event(LdWorkerId worker, std::string_view event_json) {
        Json payload;
        std::string error;
        if (parse_json(event_json, payload, error) != ParseStatus::ok) {
            return LD_STATUS_INVALID_ARGUMENT;
        }
        // The runtime owns the threadId encoding: bridges never build it.
        if (!payload.contains("threadId")) {
            payload["threadId"] = thread_id_of(worker);
        }
        queue_worker_message(worker, std::move(payload));
        // Without a transport there is no session thread to serialize against:
        // the in-process sink is called on the producing thread, which is what
        // embedders (and the hub tests) rely on.
        if (!has_transport_.load(std::memory_order_acquire)) {
            drain_worker_inbox();
        }
        return LD_STATUS_OK;
    }

    std::size_t SessionHub::worker_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return workers_.size();
    }

    bool SessionHub::worker_registered(LdWorkerId worker) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return workers_.find(worker) != workers_.end();
    }

}  // namespace luadebug::debugger::session
