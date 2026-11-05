#pragma once

// Port of extension/script/frontend/proxy.lua: the adapter side of the DAP
// connection. It answers `initialize` locally, then connects to the backend
// (master) on launch/attach and forwards everything else verbatim.

#include <memory>
#include <string>

#include "common/bootstrap.h"
#include "common/json.h"
#include "frontend/factory.h"
#include "frontend/inject.h"
#include "frontend/spawn.h"
#include "transport/channel.h"

namespace luadebug::debugger::frontend {

    class Proxy {
    public:
        Proxy(std::unique_ptr<transport::Channel> client, std::string workdir);
        ~Proxy();

        // Runs one loop iteration: polls both channels, forwards messages and
        // performs `runInTerminal` requests. Returns false when the adapter
        // should exit (backend connection closed).
        bool update(int timeout_ms);

        bool finished() const {
            return finished_;
        }

    private:
        // Counter for launch-time session addresses: an lldb launch creates the
        // process itself, so the target's pid is not known when the address is
        // chosen.
        unsigned int launch_serial_ = 0;

        void handle_client_message(Json message);
        void start_launch_or_attach(const Json& pkg);
        void proxy_launch(const Json& pkg);
        void proxy_attach(const Json& pkg);
        bool attach_process(const Json& pkg, long pid);
        void attach_tcp(const Json& pkg, const Json& args);
        bool launch_terminal(const Json& pkg);
        bool launch_console(const Json& pkg);
        void fail_launch(const Json& pkg, const std::string& error);
        // A launch whose debugger arrives through the loader (macOS/Linux) has no
        // injection barrier to wait for: the session socket appearing is the
        // proof that the target really loaded the launcher. Bounded, so a target
        // that silently refuses the preload (Rosetta ignores DYLD_* variables)
        // fails instead of hanging.
        bool wait_for_backend(long pid, const Json& arguments, int timeout_ms, std::string& error);

        struct ServerHandle;
        bool create_server(const Json& args, long pid, bool has_pid, std::string& error);
        void send_init_request();
        // Injection plan shared by launch and attach: whether to inject, how the
        // launch carrier travels, and the bootstrap document itself.
        struct InjectionPlan;
        bool plan_injection(long pid, InjectEntry entry, const Json& arguments, InjectionPlan& plan, std::string& error);
        InjectContext injection_context(const Json& arguments) const;

        void respond_initialize(const Json& req);
        void respond_error(const Json& req, const std::string& message);
        void request_run_in_terminal(const Json& arguments);

        std::unique_ptr<transport::Channel> client_;
        std::unique_ptr<transport::Channel> server_;
        std::string workdir_;
        Json init_request_      = Json();
        bool have_init_request_ = false;
        bool finished_          = false;
        // Set once a `terminated`/`exited` event was forwarded, so the adapter
        // does not synthesize a duplicate when the backend disappears.
        bool saw_termination_ = false;
        std::unique_ptr<ChildProcess> console_child_;
    };

}  // namespace luadebug::debugger::frontend
