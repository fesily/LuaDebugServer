#pragma once

// Session hub owned by `luadebug_runtime`.
//
// One instance per process (per platform/arch), shared by the
// `luadebug` bridge and by the adapter executable. It owns:
//   * the DAP session (config, client sink, reference-id allocation),
//   * the worker registry (one entry per Lua VM that registered a bridge),
//   * per-worker command queues that are drained by the worker's own thread.
//
// Threading contract:
//   * control thread  - start_session/post_event/enqueue/unregister,
//   * target thread   - pump_worker only, for that thread's own worker.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "common/json.h"
#include "runtime/abi.h"
#include "session/master.h"
#include "transport/channel.h"

namespace luadebug::debugger::session {

    using client_sink = std::function<void(const Json&)>;

    struct WorkerEntry {
        LdBridgeApi bridge {};
        std::deque<std::string> commands;
        std::condition_variable wake;
        std::thread::id pump_thread {};
        int in_flight   = 0;
        bool registered = true;
        bool closing    = false;
        // Echoed back into responses/events so they reach the right DAP thread.
        LdWorkerId id = 0;
    };

    class SessionHub {
    public:
        ~SessionHub();

        static SessionHub& instance();

        LdStatus start_session(std::string_view config_json, bool attach_only);
        bool has_session() const;
        std::string session_config() const;

        // The adapter installs the sink that writes DAP frames to the client.
        void set_client_sink(client_sink sink);
        bool emit(const Json& message);

        LdStatus register_worker(const LdBridgeApi* bridge, LdWorkerId* out);
        LdStatus unregister_worker(LdWorkerId worker);
        LdStatus pump_worker(LdWorkerId worker, uint32_t wait_ms);
        LdStatus post_event(LdWorkerId worker, std::string_view event_json);

        // Control-thread entry point: queue a Worker command for `worker`.
        LdStatus enqueue(LdWorkerId worker, std::string command_json);

        // One DAP message from the client (transport threads feed this).
        void handle_client_message(const Json& message);

        // Test/inspection helpers.
        std::size_t worker_count() const;
        bool worker_registered(LdWorkerId worker) const;

    private:
        SessionHub() = default;

        // The sink created per dispatch needs to reach the master; it is a
        // member so the lambda can hand worker output over without a lookup.
        friend struct WorkerSinkAccess;

        LdStatus dispatch_once(std::shared_ptr<WorkerEntry> entry, std::string command);

        // Writes one DAP message to the client. Thread-safe: worker events are
        // produced on target threads while the session thread reads requests.
        void write_client(const Json& message);
        // Reads requests until the channel closes.
        void session_loop();
        // Moves queued events to the channel. Caller holds channel_mutex_.
        void flush_outbox_locked();
        // Ends the session (terminated event, channel close, optional exit).
        void finish(bool terminate_debuggee);
        void start_session_thread();

        std::unique_ptr<Master> master_;
        // The socket is used only by the session thread (poll, read, write).
        // Target threads queue their events in `outbox_` and wake it.
        std::unique_ptr<transport::Channel> channel_;
        std::thread session_thread_;
        // Guards every channel operation: the transport is single-threaded, but
        // events are produced on target threads while the session thread reads.
        std::mutex channel_mutex_;
        // Set when the transport exists; read on worker threads without taking a
        // lock, so a hook never waits on the session thread's socket work.
        std::atomic<bool> has_transport_ { false };
        std::mutex outbox_mutex_;
        std::condition_variable outbox_cv_;
        std::deque<Json> outbox_;

        // Worker-originated messages (hook events, captured output, dispatch
        // responses) are produced on target threads, but only the session thread
        // may drive the master and the transport. They wait here until it drains
        // them, so two threads never mutate one master state machine.
        void drain_worker_inbox();
        // Queues one worker-originated message for the session thread.
        void queue_worker_message(LdWorkerId worker, Json payload);
        std::mutex inbox_mutex_;
        std::condition_variable inbox_cv_;
        std::deque<std::pair<LdWorkerId, Json>> worker_inbox_;
        bool closing_ = false;

        mutable std::mutex mutex_;
        std::condition_variable worker_cv_;
        bool active_ = false;
        std::string config_;
        client_sink sink_;
        LdWorkerId next_worker_id_ = 1;
        std::unordered_map<LdWorkerId, std::shared_ptr<WorkerEntry>> workers_;
    };

}  // namespace luadebug::debugger::session
