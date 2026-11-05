#include "frontend/proxy.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <unistd.h>
#endif

#include "common/capabilities.h"
#include "common/log.h"
#include "frontend/inject.h"
#include "frontend/injector_backend.h"
#include "frontend/injector_helper.h"
#include "frontend/platform.h"
#include "frontend/spawn.h"
#include "transport/reactor.h"

namespace luadebug::debugger::frontend {

    namespace {

        std::string unix_address_for_pid(long pid) {
            return "@$tmp/luadbg_" + std::to_string(pid);
        }

        long current_pid() {
#if defined(_WIN32)
            return static_cast<long>(GetCurrentProcessId());
#else
            return static_cast<long>(getpid());
#endif
        }

        bool starts_with(const std::string& text, const char* prefix) {
            return text.rfind(prefix, 0) == 0;
        }

        Json make_response(const Json& request, bool success, const char* message) {
            Json response           = Json::object();
            response["type"]        = "response";
            response["seq"]         = 0;
            response["command"]     = get_string(request, "command");
            response["request_seq"] = get_int(request, "seq", 0);
            response["success"]     = success;
            if (!success && message) {
                response["message"] = message;
            }
            return response;
        }

    }  // namespace

    Proxy::Proxy(std::unique_ptr<transport::Channel> client, std::string workdir)
        : client_(std::move(client))
        , workdir_(std::move(workdir)) {
        const char* trace = std::getenv("LUA_DEBUG_TRACE_DAP");
        if (trace && *trace && *trace != '0') {
            client_->debug = true;
        }
        // A gated (frida-spawned) child writes to a pipe we own: forward it as
        // DAP output. Once the debugger's own capture is installed it takes over
        // fd 1/2 inside the target, so nothing is delivered twice.
        set_child_output_sink([this](int fd, const std::string& bytes) {
            if (!client_) {
                return;
            }
            Json event                = Json::object();
            event["type"]             = "event";
            event["seq"]              = 0;
            event["event"]            = "output";
            event["body"]["category"] = fd == 2 ? "stderr" : "stdout";
            event["body"]["output"]   = bytes;
            client_->send(event);
        });
    }

    Proxy::~Proxy() {
        // The sink captures `this`; frida's device outlives this object.
        set_child_output_sink(nullptr);
    }

    void Proxy::respond_initialize(const Json& req) {
        Json response    = make_response(req, true, nullptr);
        response["body"] = capabilities();
        client_->send(response);
    }

    void Proxy::respond_error(const Json& req, const std::string& message) {
        client_->send(make_response(req, false, message.c_str()));
    }

    void Proxy::request_run_in_terminal(const Json& arguments) {
        Json request         = Json::object();
        request["type"]      = "request";
        request["seq"]       = 0;
        request["command"]   = "runInTerminal";
        request["arguments"] = arguments;
        client_->send(request);
    }

    void Proxy::send_init_request() {
        if (have_init_request_ && server_) {
            server_->send(init_request_);
        }
    }

    bool Proxy::create_server(const Json& args, long pid, bool has_pid, std::string& error) {
        std::string spec;
        bool client_mode = get_bool(args, "client", false);
        auto address     = get_string(args, "address");
        if (has(args, "address")) {
            spec = (client_mode ? "connect:" : "listen:") + std::string(address);
        } else {
            long target = has_pid ? pid : current_pid();
            spec        = "connect:" + unix_address_for_pid(target);
        }
        log_info("frontend: backend socket %s", spec.c_str());
        server_ = transport::open_socket_channel(spec, error);
        if (server_) {
            const char* trace = std::getenv("LUA_DEBUG_TRACE_DAP");
            if (trace && *trace && *trace != '0') {
                server_->debug = true;
            }
        }
        if (!server_) {
            log_error("frontend: open %s failed: %s", spec.c_str(), error.c_str());
            return false;
        }
        server_->set_on_close([this]() {
            // The backend went away: that ends the debugging session.
            finished_ = true;
        });
        return true;
    }

    InjectContext Proxy::injection_context(const Json& arguments) const {
        InjectContext context;
        context.workdir = workdir_;
        factory_initialize(arguments, context.platform);
        return context;
    }

    struct Proxy::InjectionPlan {
        bool enabled = false;
        bool preload = false;
        std::string launcher;
        BootstrapConfig config;
    };

    bool Proxy::plan_injection(long pid, InjectEntry entry, const Json& arguments, InjectionPlan& plan, std::string& error) {
        plan = InjectionPlan {};
        std::string mode;
        if (!resolve_inject_mode(arguments, mode, error)) {
            return false;
        }
        if (mode == "none") {
            return true;
        }
        plan.enabled = true;
        std::string lua_arch;
        if (!detect_target_arch(pid, lua_arch, error)) {
            return false;
        }
        const auto injection = injection_context(arguments);
        if (!launcher_path(injection, lua_arch, plan.launcher, error)) {
            return false;
        }
        if (!make_bootstrap_config(pid, entry, arguments, injection, lua_arch, plan.config, error)) {
            return false;
        }
        // The loader cannot carry a document on Windows, so launches there are
        // injected through frida at the gated spawn point.
        plan.preload = entry == InjectEntry::launch && host_os() != HostOs::windows;
        return true;
    }

    bool Proxy::attach_process(const Json& pkg, long pid) {
        const auto& args = pkg["arguments"];
        InjectionPlan plan;
        std::string error;
        if (!plan_injection(pid, InjectEntry::attach, args, plan, error)) {
            respond_error(pkg, error);
            return false;
        }
        if (plan.enabled && !inject_process(pid, InjectEntry::attach, args, injection_context(args), error)) {
            respond_error(pkg, error);
            return false;
        }
        std::string socket_error;
        if (!create_server(args, pid, true, socket_error)) {
            respond_error(pkg, socket_error);
            return false;
        }
        send_init_request();
        server_->send(pkg);
        return true;
    }

    void Proxy::attach_tcp(const Json& pkg, const Json& args) {
        std::string error;
        bool client_mode = get_bool(args, "client", false);
        auto address     = get_string(args, "address");
        std::string spec = (client_mode ? "connect:" : "listen:") + std::string(address);
        server_          = transport::open_socket_channel(spec, error);
        if (!server_) {
            respond_error(pkg, error);
            return;
        }
        server_->set_on_close([this]() { finished_ = true; });
        send_init_request();
        server_->send(pkg);
    }

    void Proxy::proxy_attach(const Json& pkg) {
        const auto& args = pkg["arguments"];
        {
            std::string error;
            if (!validate_vm_source_usage(args, "attach", error)) {
                respond_error(pkg, error);
                return;
            }
        }
        auto process_id = get_string(args, "processId");
        if (has(args, "processId")) {
            // VS Code sends `processId` as a string ("${command:pickProcess}"),
            // direct DAP clients usually send a number; both mean the same pid.
            long pid = static_cast<long>(get_int(args, "processId", 0));
            if (pid == 0 && !process_id.empty()) {
                // Keep the buffer alive: `end` points into it.
                const std::string text(process_id);
                char* end         = nullptr;
                const long parsed = std::strtol(text.c_str(), &end, 10);
                if (end && *end == '\0' && parsed > 0) {
                    pid = parsed;
                }
            }
            Json args_copy = args;
            (void)process_id;
            if (!attach_process(pkg, pid)) {
                return;
            }
            return;
        }
        auto process_name = get_string(args, "processName");
        if (!process_name.empty()) {
            auto pids = query_processes(std::string(process_name));
            if (pids.empty()) {
                respond_error(pkg, "Cannot found process `" + std::string(process_name) + "`.");
                return;
            }
            if (pids.size() > 1) {
                respond_error(pkg, "There are " + std::to_string(pids.size()) + " processes `" + std::string(process_name) + "`.");
                return;
            }
            attach_process(pkg, pids[0]);
            return;
        }
        attach_tcp(pkg, args);
    }

    bool Proxy::launch_terminal(const Json& pkg) {
        const auto& args = pkg["arguments"];
        FactoryContext context;
        context.workdir = workdir_;
        factory_initialize(args, context.platform);

        if (has(args, "runtimeExecutable")) {
            if (get_string(args, "inject") != "none") {
                respond_error(pkg, "`inject` is not supported in `" + std::string(get_string(args, "console")) + "`.");
                return false;
            }
            std::string error;
            if (!create_server(args, 0, false, error)) {
                respond_error(pkg, error);
                return false;
            }
            TerminalPlan plan;
            if (!create_process_in_terminal(init_request_, args, context, plan, error)) {
                respond_error(pkg, error);
                return false;
            }
            request_run_in_terminal(plan.arguments);
            return true;
        }

        std::string error;
        if (!create_server(args, 0, false, error)) {
            respond_error(pkg, error);
            return false;
        }
        // The child connects back to us at `$tmp/luadbg_<adapter pid>`.
        std::string address = std::to_string(current_pid());
        TerminalPlan plan;
        if (!create_luaexe_in_terminal(init_request_, args, context, address, plan, error)) {
            respond_error(pkg, error);
            return false;
        }
        request_run_in_terminal(plan.arguments);
        return true;
    }

    bool Proxy::wait_for_backend(long pid, const Json& arguments, int timeout_ms, std::string& error) {
        (void)pid;
        (void)arguments;
        // A preload launch has no injection barrier: the only proof that the
        // target really loaded the launcher is the backend connection itself. This
        // covers connect and listen addresses alike, because it observes the
        // channel rather than the socket file.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (server_ && server_->connected()) {
                return true;
            }
            if (console_child_ && !is_child_running(*console_child_)) {
                error =
                    "The target exited before the debugger connected. "
                    "Injection did not take effect.";
                return false;
            }
            transport::Reactor::instance().update(50);
            if (server_) {
                server_->update(0);
            }
        }
        error =
            "The target never connected to the debugger. If this target runs "
            "translated (Rosetta), start it and attach instead of launching.";
        return false;
    }

    bool Proxy::launch_console(const Json& pkg) {
        const auto& args = pkg["arguments"];
        FactoryContext context;
        context.workdir = workdir_;
        factory_initialize(args, context.platform);

        if (has(args, "runtimeExecutable")) {
            std::string error;
            InjectionPlan plan;
            const auto injection = injection_context(args);
            std::string mode;
            if (!resolve_inject_mode(args, mode, error)) {
                respond_error(pkg, error);
                return false;
            }
            // The backend decides how the launcher reaches a launched target:
            // preload (loader-carried) or an lldb launch that owns the spawn. The
            // executable's own architecture is what matters here, because no pid
            // exists yet.
            Json launch_args = args;
            bool lldb_launch = false;
            if (mode == "frida") {
                std::string lua_arch    = executable_arch(std::string(get_string(args, "runtimeExecutable")));
                InjectorBackend backend = InjectorBackend::frida;
                bool automatic          = false;
                if (!resolve_injector_backend(args, lua_arch, backend, automatic, error)) {
                    respond_error(pkg, error);
                    return false;
                }
                lldb_launch = backend == InjectorBackend::lldb && host_os() == HostOs::macos;
                log_info("frontend: launch %s: executable architecture %s, injector backend %s", std::string(get_string(args, "runtimeExecutable")).c_str(), lua_arch.empty() ? "(unknown)" : lua_arch.c_str(), injector_backend_name(backend));
                if (lua_arch.empty() && !lldb_launch) {
                    // No readable Mach-O: fall back to the adapter's own
                    // architecture, which is what the preload carrier wants.
                    if (!detect_target_arch(0, lua_arch, error)) {
                        respond_error(pkg, error);
                        return false;
                    }
                }
                if (!launcher_path(injection, lua_arch, plan.launcher, error)) {
                    respond_error(pkg, error);
                    return false;
                }
                if (lldb_launch && !has(args, "address")) {
                    // lldb owns the spawn, so the session address cannot be
                    // derived from the target's pid. Pick one here; the adapter
                    // listens on it and the launcher connects (the default
                    // `client` direction for an explicit address).
                    launch_args["address"] = "@$tmp/luadbg_launch_" + std::to_string(current_pid()) + "_" + std::to_string(++launch_serial_);
                }
                plan.enabled = true;
                plan.preload = !lldb_launch && host_os() != HostOs::windows;
                if (!make_bootstrap_config(0, InjectEntry::launch, launch_args, injection, lua_arch, plan.config, error)) {
                    respond_error(pkg, error);
                    return false;
                }
            } else if (!has(args, "address")) {
                respond_error(pkg, "`runtimeExecutable` need specify `inject` or `address`.");
                return false;
            }

            if (lldb_launch) {
                // lldb starts the target stopped at its entry point, loads the
                // launcher, calls its entry and detaches; the target then runs on
                // its own and connects to us.
                SpawnOptions spawn;
                LaunchInjection none;  // no preload carrier on this path
                if (!create_process_in_console(args, context, none, spawn, error)) {
                    respond_error(pkg, error);
                    return false;
                }
                LaunchRequest request;
                request.argv     = spawn.argv;
                request.cwd      = spawn.cwd;
                request.env      = spawn.env;
                request.launcher = plan.launcher;
                request.data     = dump_bootstrap(plan.config);
                if (!launch_with_lldb(request, error)) {
                    respond_error(pkg, error);
                    return false;
                }
                if (!create_server(launch_args, 0, false, error)) {
                    respond_error(pkg, error);
                    return false;
                }
                if (!wait_for_backend(0, launch_args, 10000, error)) {
                    respond_error(pkg, error);
                    return false;
                }
                return true;
            }

            LaunchInjection launch;
            launch.enabled  = plan.enabled;
            launch.preload  = plan.preload;
            launch.launcher = plan.launcher;
            launch.config   = plan.config;

            SpawnOptions spawn;
            if (!create_process_in_console(args, context, launch, spawn, error)) {
                respond_error(pkg, error);
                return false;
            }

            auto child         = std::make_unique<ChildProcess>();
            const bool gated   = plan.enabled && !plan.preload;
            const bool spawned = gated ? spawn_child_gated(spawn, *child, error)
                                       : spawn_child(spawn, *child, error);
            if (!spawned) {
                respond_error(pkg, error);
                return false;
            }
            console_child_ = std::move(child);
            long pid       = console_child_->pid;

            if (gated) {
                // The bootstrap document travels through frida's `data`, and the
                // target stays suspended until its entry point finished.
                InjectionPlan resolved;
                if (!plan_injection(pid, InjectEntry::launch, args, resolved, error)) {
                    fail_launch(pkg, error);
                    return false;
                }
                if (resolved.enabled && !inject_process(pid, InjectEntry::launch, args, injection_context(args), error)) {
                    fail_launch(pkg, error);
                    return false;
                }
            }

            std::string addr_error;
            if (!create_server(launch_args, pid, true, addr_error)) {
                fail_launch(pkg, addr_error);
                return false;
            }
            if (gated && !resume_child(*console_child_)) {
                fail_launch(pkg, "resume failed");
                return false;
            }
            if (plan.enabled && plan.preload && !wait_for_backend(pid, launch_args, 10000, error)) {
                fail_launch(pkg, error);
                return false;
            }
            return true;
        }

        std::string error;
        if (!create_server(args, 0, false, error)) {
            respond_error(pkg, error);
            return false;
        }
        ConsolePlan plan;
        std::string address = std::to_string(current_pid());
        if (!create_luaexe_in_console(args, context, address, plan, error)) {
            respond_error(pkg, error);
            return false;
        }
        auto child = std::make_unique<ChildProcess>();
        if (!spawn_child(plan.spawn, *child, error)) {
            respond_error(pkg, error);
            return false;
        }
        console_child_ = std::move(child);
        return true;
    }

    void Proxy::fail_launch(const Json& pkg, const std::string& error) {
        // A launch owns the process it created: never leave a suspended debuggee
        // behind when the injection or the resume failed.
        if (console_child_) {
            terminate_child(*console_child_);
            console_child_.reset();
        }
        // Drop the half-open backend channel too: keeping it would forward later
        // client requests (disconnect, a retry) into a backend that never came up.
        if (server_) {
            server_->close();
            server_.reset();
        }
        respond_error(pkg, error);
    }

    void Proxy::proxy_launch(const Json& pkg) {
        auto args = pkg["arguments"];
        {
            std::string error;
            if (!validate_vm_source_usage(args, "launch", error)) {
                respond_error(pkg, error);
                return;
            }
        }
        PlatformState state;
        factory_initialize(args, state);

        std::string console = std::string(get_string(args, "console"));
        if (has(args, "runtimeExecutable") && get_string(args, "inject") != "none") {
            console = "internalConsole";
        }
        bool ok = false;
        if (console == "integratedTerminal" || console == "externalTerminal") {
            ok = launch_terminal(pkg);
        } else {
            ok = launch_console(pkg);
        }
        if (!ok) {
            return;
        }
        if (!server_) {
            return;
        }
        send_init_request();
        server_->send(pkg);
    }

    void Proxy::start_launch_or_attach(const Json& pkg) {
        const auto& args = pkg["arguments"];
        auto request     = get_string(args, "request");
        if (request == "attach") {
            proxy_attach(pkg);
        } else if (request == "launch") {
            proxy_launch(pkg);
        }
    }

    void Proxy::handle_client_message(Json message) {
        if (server_) {
            // Responses to `runInTerminal` belong to us, never the backend.
            if (get_string(message, "type") == "response" && get_string(message, "command") == "runInTerminal") {
                return;
            }
            server_->send(message);
            return;
        }

        if (!have_init_request_) {
            if (get_string(message, "type") == "request" && get_string(message, "command") == "initialize") {
                message["__norepl"] = true;
                init_request_       = message;
                have_init_request_  = true;
                respond_initialize(message);
            } else {
                respond_error(message, "not initialized");
            }
            return;
        }

        if (get_string(message, "type") == "request") {
            auto command = get_string(message, "command");
            if (command == "attach" || command == "launch") {
                start_launch_or_attach(message);
            } else {
                respond_error(message, "error request");
            }
        }
    }

    bool Proxy::update(int timeout_ms) {
        if (finished_) {
            return false;
        }
        pump_frida_events();

        for (;;) {
            if (server_) {
                server_->update(timeout_ms);
                timeout_ms = 0;
                Json message;
                std::string error;
                while (server_->recv(message, error)) {
                    if (get_string(message, "event") == "terminated" || get_string(message, "event") == "exited") {
                        saw_termination_ = true;
                    }
                    client_->send(message);
                }
                if (!error.empty()) {
                    log_error("backend protocol error: %s", error.c_str());
                    finished_ = true;
                    return false;
                }
                if (server_->closed()) {
                    // The debuggee (or the backend hosting it) went away without
                    // a final event. The adapter owes the client a session end,
                    // so synthesize one instead of just dropping the
                    // connection.
                    if (!saw_termination_) {
                        Json terminated     = Json::object();
                        terminated["type"]  = "event";
                        terminated["event"] = "terminated";
                        terminated["seq"]   = 0;
                        terminated["body"]  = Json::object();
                        client_->send(terminated);
                        saw_termination_ = true;
                        // Give a queued socket write a chance to drain before
                        // the adapter process goes away.
                        client_->update(0);
                    }
                    finished_ = true;
                    return false;
                }
            } else {
                transport::Reactor::instance().update(timeout_ms);
                timeout_ms = 0;
                // A gated launch has no backend channel yet; its frida signals
                // (child output) still have to be dispatched every iteration.
                pump_frida_events();
            }

            client_->update(0);
            Json message;
            std::string error;
            while (client_->recv(message, error)) {
                handle_client_message(std::move(message));
                if (finished_) {
                    return false;
                }
            }
            if (!error.empty()) {
                log_error("client protocol error: %s", error.c_str());
                finished_ = true;
                return false;
            }
            pump_frida_events();
            if (client_->closed() || !server_) {
                break;
            }
        }
        return !finished_;
    }

}  // namespace luadebug::debugger::frontend
