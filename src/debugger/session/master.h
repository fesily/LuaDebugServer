#pragma once

// The DAP-side session master: the native replacement for
// extension/script/backend/master/{mgr,request,response,event,threads}.lua.
//
// It owns everything that is *not* per-VM:
//   * the DAP conversation (request routing, responses, events, seq numbers),
//   * the session config cache and the breakpoint replay for workers that join
//     later (a new Lua VM must see the breakpoints the client already set),
//   * reference-id encoding ((threadId << 24) | local, (threadId << 32) | source,
//     memory_<threadId>x<ref>, inst_<threadId>x<rest>),
//   * the thread registry (names, status, ordering).
//
// No I/O and no Lua: transports and worker queues are injected. That keeps the
// rules testable without a socket or a debuggee.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/json.h"
#include "runtime/abi.h"

namespace luadebug::debugger::session {

    // Session config resolution (master/resolve_config.lua): fills the defaults
    // the worker and the client rely on.
    void resolve_session_config(Json& config);

    class Master {
    public:
        struct Router {
            // Queues one command for one worker; false when it is not registered.
            std::function<bool(LdWorkerId, const Json&)> to_worker;
            std::function<void(const Json&)> to_all;
            std::function<void(LdWorkerId, const Json&)> to_all_except;
            // Ends the session: `terminate_debuggee` asks the process to exit once
            // everything the client is owed has been written.
            std::function<void(bool terminate_debuggee)> finish;
        };

        using Sender = std::function<void(const Json&)>;

        Master(Sender send, Router router);

        // A message that arrived from the client.
        void handle_client_message(const Json& message);
        // A message a worker produced (response/event), tagged with its worker id.
        void handle_worker_message(LdWorkerId worker, const Json& message);

        // Worker lifecycle, called by the hub.
        void worker_ready(LdWorkerId worker);
        void worker_gone(LdWorkerId worker);

        // Thread naming (the old `setThreadName` worker command).
        void set_thread_name(LdWorkerId worker, const std::string& name);

        // Cached config, handed to bridges so a late VM starts with the same
        // settings the session began with.
        const Json& session_config() const {
            return config_;
        }

        bool initialized() const {
            return initialized_;
        }

        // Test hooks.
        const Json& config() const {
            return config_;
        }

    private:
        // --- DAP -------------------------------------------------------------
        void send(const Json& message);
        void respond_success(const Json& request, Json body = Json());
        void respond_error(const Json& request, const std::string& message);
        void emit_event(const std::string& event, Json body = Json());

        // --- requests --------------------------------------------------------
        void on_initialize(const Json& request);
        void on_attach(const Json& request, bool launch);
        void on_configuration_done(const Json& request);
        void on_set_breakpoints(const Json& request);
        void on_set_function_breakpoints(const Json& request);
        void on_set_exception_breakpoints(const Json& request);
        void on_set_instruction_breakpoints(const Json& request);
        void on_stack_trace(const Json& request);
        void on_scopes(const Json& request);
        void on_variables(const Json& request);
        void on_evaluate(const Json& request);
        void on_threads(const Json& request);
        void on_disconnect(const Json& request);
        void on_terminate(const Json& request);
        void on_restart(const Json& request);
        void on_terminate_threads(const Json& request);
        void on_pause(const Json& request);
        void on_continue(const Json& request);
        void on_step(const Json& request, const char* command);
        void on_source(const Json& request);
        void on_exception_info(const Json& request);
        void on_set_variable(const Json& request);
        void on_set_expression(const Json& request);
        void on_loaded_sources(const Json& request);
        void on_restart_frame(const Json& request);
        void on_read_memory(const Json& request);
        void on_write_memory(const Json& request);
        void on_disassemble(const Json& request);
        void on_custom_integer(const Json& request, bool hex);

        // --- helpers ---------------------------------------------------------
        bool check_thread(const Json& request, long thread_id);
        void initialize_worker(LdWorkerId worker);
        void initialize_worker_breakpoints(LdWorkerId worker, const Json& source, const Json& breakpoints, const Json& content);
        void broadcast(const Json& command);
        void broadcast_except(LdWorkerId worker, const Json& command);
        // Resume only the workers that are stopped right now.
        void resume_stopped_workers();
        void broadcast_stopped_except(LdWorkerId worker, const Json& command);
        void send_to(LdWorkerId worker, const Json& command);
        void try_stop(LdWorkerId worker);
        int next_breakpoint_id();
        long next_seq();

        // Encodes/decodes the composite reference ids.
        static long id_thread(long long value, int shift);
        static long long id_local(long long value, int shift);

        struct WorkerState {
            std::string name;
            bool connected = false;
            // Set when this worker reports a stop and cleared when it is told to
            // resume. `continue` only reaches workers that are actually stopped:
            // a queued `run` for a worker that had not stopped yet would release
            // its *next* stop (two VMs in one session stop independently).
            bool stopped = false;
        };

        Sender send_;
        Router router_;
        long seq_          = 0;
        bool initialized_  = false;
        bool first_worker_ = true;
        // "none" -> "initializing" -> "initialized"
        std::string state_       = "none";
        bool close_process_      = false;
        bool keep_session_alive_ = false;
        Json client_             = Json::object();
        // initialize / launch arguments plus the replayed breakpoints.
        Json config_         = Json::object();
        int next_breakpoint_ = 0;
        std::map<LdWorkerId, WorkerState> workers_;
    };

}  // namespace luadebug::debugger::session
