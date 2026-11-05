#include "session/master.h"

#include "common/capabilities.h"
#include "common/log.h"

namespace luadebug::debugger::session {

    namespace {

        constexpr int kThreadShift = 24;
        constexpr int kSourceShift = 32;
        constexpr bool kValueRoot  = false;

        // 64-bit reference ids travel as JSON numbers; DAP clients may send them
        // back as doubles, so decode through double exactly once.
        long long as_int64(const Json& value) {
            if (value.is_number_integer()) {
                return value.get<long long>();
            }
            if (value.is_number_unsigned()) {
                return static_cast<long long>(value.get<unsigned long long>());
            }
            if (value.is_number_float()) {
                return static_cast<long long>(value.get<double>());
            }
            return 0;
        }

        std::string json_string(const Json& object, const char* key) {
            if (!object.is_object()) {
                return {};
            }
            auto it = object.find(key);
            if (it == object.end() || it->is_null() || !it->is_string()) {
                return {};
            }
            return it->get<std::string>();
        }

        bool json_bool(const Json& object, const char* key, bool fallback = false) {
            if (!object.is_object()) {
                return fallback;
            }
            auto it = object.find(key);
            if (it == object.end() || it->is_null() || !it->is_boolean()) {
                return fallback;
            }
            return it->get<bool>();
        }

        void strip_type_annotations(Json& breakpoint) {
            if (!breakpoint.is_object()) {
                return;
            }
            breakpoint.erase("column");
            breakpoint.erase("endColumn");
        }

        std::string number(long long value) {
            return std::to_string(value);
        }

        // "memory_<threadId>x<ref>" / "inst_<threadId>x<rest>".
        bool split_encoded(const std::string& text, const char* prefix, long& thread, std::string& rest) {
            const std::string head(prefix);
            if (text.compare(0, head.size(), head) != 0) {
                return false;
            }
            const std::size_t x = text.find('x', head.size());
            if (x == std::string::npos) {
                return false;
            }
            const std::string digits = text.substr(head.size(), x - head.size());
            if (digits.empty()) {
                return false;
            }
            for (char c : digits) {
                if (c < '0' || c > '9') {
                    return false;
                }
            }
            thread = std::atol(digits.c_str());
            rest   = text.substr(x + 1);
            return !rest.empty();
        }

    }  // namespace

    void resolve_session_config(Json& config) {
        if (!config.is_object()) {
            config = Json::object();
        }
        config["type"] = "lua";
        if (json_string(config, "request") != "attach") {
            config["request"] = "launch";
        }
        if (!config.contains("name") || !config["name"].is_string()) {
            config["name"] = "Not specified";
        }
        if (!config.contains("cwd") || !config["cwd"].is_string()) {
            const std::string workspace = json_string(config, "workspaceFolder");
            if (!workspace.empty()) {
                config["cwd"] = workspace;
            }
        }
        if (!config.contains("stopOnEntry") || !config["stopOnEntry"].is_boolean()) {
            config["stopOnEntry"] = true;
        }
        if (!config.contains("stopOnThreadEntry") || !config["stopOnThreadEntry"].is_boolean()) {
            config["stopOnThreadEntry"] = false;
        }
        if (!config.contains("keepSessionAlive") || !config["keepSessionAlive"].is_boolean()) {
            config["keepSessionAlive"] = false;
        }
        // `luaVersion` gets no default here. When the client omits it the VM is
        // detected in the target; filling in "lua54" would turn "unspecified"
        // into a hard constraint on a 5.1..5.5 or LuaJIT debuggee.
        if (config.contains("luaVersion") && !config["luaVersion"].is_string()) {
            // The constraint itself is enforced where it is used (the target
            // validates it against the detected VM), but a malformed value must not
            // pass silently: dropping it would turn an explicit constraint into
            // detection.
            luadebug::debugger::log_error(
                "session config: `luaVersion` is not a string (%s); "
                "ignoring it",
                config["luaVersion"].dump().c_str()
            );
            config.erase("luaVersion");
        }
        if (!config.contains("console") || !config["console"].is_string()) {
            config["console"] = "internalConsole";
        }
        if (!config.contains("sourceCoding") || !config["sourceCoding"].is_string()) {
            config["sourceCoding"] = "utf8";
        }
        if (!config.contains("outputCapture") || !config["outputCapture"].is_array()) {
            if (json_string(config, "console") == "internalConsole") {
                Json capture = Json::array();
                capture.push_back("print");
                capture.push_back("io.write");
                capture.push_back("stdout");
                capture.push_back("stderr");
                config["outputCapture"] = std::move(capture);
            } else {
                config["outputCapture"] = Json::array();
            }
        }
        if (!config.contains("pathFormat") || !config["pathFormat"].is_string()) {
            config["pathFormat"] = "path";
        }
        if (config.contains("sourceMaps") && !config["sourceMaps"].is_array()) {
            config.erase("sourceMaps");
        }
        if (config.contains("address") && config["address"].is_string() && (!config.contains("client") || !config["client"].is_boolean())) {
            config["client"] = true;
        }
        if (!config.contains("configuration") || !config["configuration"].is_object()) {
            Json configuration         = Json::object();
            configuration["variables"] = Json::object();
            config["configuration"]    = std::move(configuration);
        }
    }

    Master::Master(Sender send, Router router)
        : send_(std::move(send))
        , router_(std::move(router)) {
        config_["initialize"]              = Json::object();
        config_["breakpoints"]             = Json::object();
        config_["function_breakpoints"]    = Json::array();
        config_["exception_breakpoints"]   = Json::array();
        config_["instruction_breakpoints"] = Json::array();
    }

    long Master::next_seq() {
        return ++seq_;
    }

    void Master::send(const Json& message) {
        if (!send_) {
            return;
        }
        Json stamped   = message;
        stamped["seq"] = next_seq();
        send_(stamped);
    }

    namespace {
        // A master-side request carries the client's `seq`; a worker response
        // carries the `request_seq` it was built from. Both round-trip to the
        // client's own sequence number.
        long long request_seq_of(const Json& message) {
            if (!message.is_object()) {
                return 0;
            }
            if (message.contains("request_seq")) {
                return as_int64(message["request_seq"]);
            }
            if (message.contains("seq")) {
                return as_int64(message["seq"]);
            }
            return 0;
        }
    }  // namespace

    void Master::respond_success(const Json& request, Json body) {
        Json response           = Json::object();
        response["type"]        = "response";
        response["command"]     = json_string(request, "command");
        response["request_seq"] = request_seq_of(request);
        response["success"]     = true;
        if (!body.is_null()) {
            response["body"] = std::move(body);
        }
        send(response);
    }

    void Master::respond_error(const Json& request, const std::string& message) {
        Json response           = Json::object();
        response["type"]        = "response";
        response["command"]     = json_string(request, "command");
        response["request_seq"] = request_seq_of(request);
        response["success"]     = false;
        response["message"]     = message;
        send(response);
    }

    void Master::emit_event(const std::string& event, Json body) {
        Json message     = Json::object();
        message["type"]  = "event";
        message["event"] = event;
        if (!body.is_null()) {
            message["body"] = std::move(body);
        }
        send(message);
    }

    int Master::next_breakpoint_id() {
        return ++next_breakpoint_;
    }

    void Master::send_to(LdWorkerId worker, const Json& command) {
        if (router_.to_worker) {
            router_.to_worker(worker, command);
        }
    }

    void Master::broadcast(const Json& command) {
        if (router_.to_all) {
            router_.to_all(command);
        }
    }

    void Master::broadcast_except(LdWorkerId worker, const Json& command) {
        if (router_.to_all_except) {
            router_.to_all_except(worker, command);
        }
    }

    void Master::resume_stopped_workers() {
        Json command   = Json::object();
        command["cmd"] = "run";
        for (auto& entry : workers_) {
            if (!entry.second.stopped) {
                continue;
            }
            entry.second.stopped = false;
            send_to(entry.first, command);
        }
    }

    void Master::broadcast_stopped_except(LdWorkerId worker, const Json& command) {
        for (auto& entry : workers_) {
            if (entry.first == worker || !entry.second.stopped) {
                continue;
            }
            entry.second.stopped = false;
            send_to(entry.first, command);
        }
    }

    bool Master::check_thread(const Json& request, long thread_id) {
        if (thread_id <= 0) {
            respond_error(request, "No threadId");
            return false;
        }
        if (workers_.find(static_cast<LdWorkerId>(thread_id)) == workers_.end()) {
            respond_error(request, "Not found thread [" + number(thread_id) + "]");
            return false;
        }
        return true;
    }

    long Master::id_thread(long long value, int shift) {
        return static_cast<long>(value >> shift);
    }

    long long Master::id_local(long long value, int shift) {
        return value & ((1LL << shift) - 1);
    }

    void Master::worker_ready(LdWorkerId worker) {
        WorkerState& state = workers_[worker];
        state.connected    = true;
        if (state_.compare("initialized") == 0) {
            initialize_worker(worker);
        }
    }

    void Master::worker_gone(LdWorkerId worker) {
        workers_.erase(worker);
        if (!keep_session_alive_ && workers_.empty()) {
            if (router_.finish) {
                router_.finish(false);
            }
        }
    }

    void Master::set_thread_name(LdWorkerId worker, const std::string& name) {
        workers_[worker].name = name;
    }

    void Master::initialize_worker_breakpoints(LdWorkerId worker, const Json& source, const Json& breakpoints, const Json& content) {
        Json command           = Json::object();
        command["cmd"]         = "setBreakpoints";
        command["source"]      = source;
        command["breakpoints"] = breakpoints;
        command["content"]     = content;
        send_to(worker, command);
    }

    void Master::try_stop(LdWorkerId worker) {
        const Json& initialize = config_["initialize"];
        if (first_worker_ && json_bool(initialize, "stopOnEntry")) {
            Json command      = Json::object();
            command["cmd"]    = "stop";
            command["reason"] = "entry";
            send_to(worker, command);
            return;
        }
        if (json_bool(initialize, "stopOnThreadEntry")) {
            Json command      = Json::object();
            command["cmd"]    = "stop";
            command["reason"] = "entry";
            send_to(worker, command);
        }
    }

    void Master::initialize_worker(LdWorkerId worker) {
        {
            Json command      = Json::object();
            command["cmd"]    = "initializing";
            command["config"] = config_["initialize"];
            send_to(worker, command);
        }
        // Breakpoints set before this VM existed must reach it too.
        if (config_["breakpoints"].is_object()) {
            for (auto it = config_["breakpoints"].begin(); it != config_["breakpoints"].end(); ++it) {
                const Json& entry = it.value();
                if (!entry.is_array() || entry.size() < 3) {
                    continue;
                }
                const long long key = std::atoll(it.key().c_str());
                const bool for_all  = it.key().rfind("path:", 0) == 0;
                const LdWorkerId owner =
                    for_all ? 0 : static_cast<LdWorkerId>(key >> kSourceShift);
                if (!for_all && owner != worker) {
                    continue;
                }
                initialize_worker_breakpoints(worker, entry[0], entry[1], entry[2]);
            }
        }
        {
            Json command           = Json::object();
            command["cmd"]         = "setFunctionBreakpoints";
            command["breakpoints"] = config_["function_breakpoints"];
            send_to(worker, command);
        }
        {
            Json command         = Json::object();
            command["cmd"]       = "setExceptionBreakpoints";
            command["arguments"] = config_["exception_breakpoints"];
            send_to(worker, command);
        }
        {
            Json command           = Json::object();
            command["cmd"]         = "setInstructionBreakpoints";
            command["breakpoints"] = config_["instruction_breakpoints"];
            send_to(worker, command);
        }
        if (first_worker_ && json_bool(config_, "launch")) {
            Json command           = Json::object();
            command["cmd"]         = "setSearchPath";
            const Json& initialize = config_["initialize"];
            if (initialize.is_object() && initialize.contains("path")) {
                command["path"] = initialize["path"];
            }
            if (initialize.is_object() && initialize.contains("cpath")) {
                command["cpath"] = initialize["cpath"];
            }
            send_to(worker, command);
        }
        try_stop(worker);
        {
            Json command   = Json::object();
            command["cmd"] = "initialized";
            send_to(worker, command);
        }
        first_worker_ = false;
    }

    void Master::handle_client_message(const Json& message) {
        if (!message.is_object()) {
            return;
        }
        const std::string type = json_string(message, "type");
        if (!type.empty() && type != "request") {
            return;
        }
        const std::string command = json_string(message, "command");
        if (command.empty()) {
            return;
        }

        if (!initialized_) {
            if (command == "initialize") {
                initialized_ = true;
                on_initialize(message);
            } else {
                respond_error(message, "`" + command + "` not yet implemented.(birth)");
            }
            return;
        }
        if (command == "initialize") {
            respond_error(message, "`initialize` not yet implemented.(idle)");
            return;
        }

        if (command == "attach") {
            on_attach(message, false);
        } else if (command == "launch") {
            on_attach(message, true);
        } else if (command == "configurationDone") {
            on_configuration_done(message);
        } else if (command == "setBreakpoints") {
            on_set_breakpoints(message);
        } else if (command == "setFunctionBreakpoints") {
            on_set_function_breakpoints(message);
        } else if (command == "setExceptionBreakpoints") {
            on_set_exception_breakpoints(message);
        } else if (command == "setInstructionBreakpoints") {
            on_set_instruction_breakpoints(message);
        } else if (command == "stackTrace") {
            on_stack_trace(message);
        } else if (command == "scopes") {
            on_scopes(message);
        } else if (command == "variables") {
            on_variables(message);
        } else if (command == "evaluate") {
            on_evaluate(message);
        } else if (command == "threads") {
            on_threads(message);
        } else if (command == "disconnect") {
            on_disconnect(message);
        } else if (command == "terminate") {
            on_terminate(message);
        } else if (command == "restart") {
            on_restart(message);
        } else if (command == "terminateThreads") {
            on_terminate_threads(message);
        } else if (command == "pause") {
            on_pause(message);
        } else if (command == "continue") {
            on_continue(message);
        } else if (command == "next") {
            on_step(message, "stepOver");
        } else if (command == "stepOut") {
            on_step(message, "stepOut");
        } else if (command == "stepIn") {
            on_step(message, "stepIn");
        } else if (command == "source") {
            on_source(message);
        } else if (command == "exceptionInfo") {
            on_exception_info(message);
        } else if (command == "setVariable") {
            on_set_variable(message);
        } else if (command == "setExpression") {
            on_set_expression(message);
        } else if (command == "loadedSources") {
            on_loaded_sources(message);
        } else if (command == "restartFrame") {
            on_restart_frame(message);
        } else if (command == "readMemory") {
            on_read_memory(message);
        } else if (command == "writeMemory") {
            on_write_memory(message);
        } else if (command == "disassemble") {
            on_disassemble(message);
        } else if (command == "customRequestShowIntegerAsDec") {
            on_custom_integer(message, false);
        } else if (command == "customRequestShowIntegerAsHex") {
            on_custom_integer(message, true);
        } else {
            respond_error(message, "`" + command + "` not yet implemented.(idle)");
        }
    }

    void Master::on_initialize(const Json& request) {
        first_worker_    = true;
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        client_          = args.is_object() ? args : Json::object();
        respond_success(request, capabilities());
        emit_event("initialized");
        Json body            = Json::object();
        body["capabilities"] = capabilities();
        emit_event("capabilities", std::move(body));
    }

    void Master::on_attach(const Json& request, bool launch) {
        Json args = request.contains("arguments") && request["arguments"].is_object()
                        ? request["arguments"]
                        : Json::object();
        resolve_session_config(args);
        respond_success(request);
        state_                             = "initializing";
        keep_session_alive_                = json_bool(args, "keepSessionAlive");
        config_["initialize"]              = args;
        config_["breakpoints"]             = Json::object();
        config_["function_breakpoints"]    = Json::array();
        config_["exception_breakpoints"]   = Json::array();
        config_["instruction_breakpoints"] = Json::array();
        if (launch) {
            config_["launch"] = true;
        } else if (config_.contains("launch")) {
            config_.erase("launch");
        }
    }

    void Master::on_configuration_done(const Json& request) {
        respond_success(request);
        state_ = "initialized";
        std::vector<LdWorkerId> workers;
        for (const auto& entry : workers_) {
            workers.push_back(entry.first);
        }
        for (LdWorkerId worker : workers) {
            initialize_worker(worker);
        }
    }

    void Master::on_set_breakpoints(const Json& request) {
        Json args              = request.contains("arguments") && request["arguments"].is_object()
                                     ? request["arguments"]
                                     : Json::object();
        const std::string path = args.contains("source") ? json_string(args["source"], "path") : "";

        bool invalid_path = false;
        if (!path.empty()) {
            const std::size_t colon = path.find(':');
            if (colon != std::string::npos && colon > 1) {
                invalid_path = true;
            }
        }

        Json breakpoints = args.contains("breakpoints") && args["breakpoints"].is_array()
                               ? args["breakpoints"]
                               : Json::array();
        for (auto& bp : breakpoints) {
            strip_type_annotations(bp);
            bp["id"]       = next_breakpoint_id();
            bp["verified"] = false;
            bp["message"]  = invalid_path
                                 ? "Does not support path: `" + path + "`"
                                 : "Wait verify. (The source file is not loaded.)";
        }
        Json body           = Json::object();
        body["breakpoints"] = breakpoints;
        respond_success(request, std::move(body));
        if (invalid_path) {
            return;
        }

        Json content;
        if (args.contains("sourceContent") && args["sourceContent"].is_string()) {
            content = args["sourceContent"];
        }
        Json source = args.contains("source") ? args["source"] : Json::object();

        long long source_reference = 0;
        if (source.is_object() && source.contains("sourceReference") && source["sourceReference"].is_number()) {
            source_reference = as_int64(source["sourceReference"]);
        }

        Json entry = Json::array();
        entry.push_back(source);       // [0] worker-facing source
        entry.push_back(breakpoints);  // [1] breakpoints
        entry.push_back(content);      // [2] source content

        if (source_reference != 0) {
            const long long worker_bits = source_reference >> kSourceShift;
            Json worker_source          = source;
            worker_source["sourceReference"] =
                id_local(source_reference, kSourceShift);
            entry[0]                                         = worker_source;
            config_["breakpoints"][number(source_reference)] = entry;
            if (state_ == "initialized") {
                initialize_worker_breakpoints(static_cast<LdWorkerId>(worker_bits), worker_source, breakpoints, content);
            }
        } else {
            config_["breakpoints"]["path:" + path] = entry;
            if (state_ == "initialized") {
                Json command           = Json::object();
                command["cmd"]         = "setBreakpoints";
                command["source"]      = source;
                command["breakpoints"] = breakpoints;
                command["content"]     = content;
                broadcast(command);
            }
        }
    }

    void Master::on_set_function_breakpoints(const Json& request) {
        Json args        = request.contains("arguments") && request["arguments"].is_object()
                               ? request["arguments"]
                               : Json::object();
        Json breakpoints = args.contains("breakpoints") && args["breakpoints"].is_array()
                               ? args["breakpoints"]
                               : Json::array();
        for (auto& bp : breakpoints) {
            bp["id"]       = next_breakpoint_id();
            bp["verified"] = false;
            bp["message"]  = "Wait verify.";
        }
        Json body           = Json::object();
        body["breakpoints"] = breakpoints;
        respond_success(request, std::move(body));
        config_["function_breakpoints"] = breakpoints;
        if (state_ == "initialized") {
            Json command           = Json::object();
            command["cmd"]         = "setFunctionBreakpoints";
            command["breakpoints"] = breakpoints;
            broadcast(command);
        }
    }

    void Master::on_set_exception_breakpoints(const Json& request) {
        Json args        = request.contains("arguments") && request["arguments"].is_object()
                               ? request["arguments"]
                               : Json::object();
        Json breakpoints = Json::array();
        Json filters     = Json::array();
        if (args.contains("filters") && args["filters"].is_array()) {
            for (const auto& filter : args["filters"]) {
                const int id   = next_breakpoint_id();
                Json bp        = Json::object();
                bp["id"]       = id;
                bp["verified"] = false;
                bp["message"]  = "Wait verify.";
                breakpoints.push_back(std::move(bp));
                Json item        = Json::object();
                item["id"]       = id;
                item["filterId"] = filter;
                filters.push_back(std::move(item));
            }
        }
        if (args.contains("filterOptions") && args["filterOptions"].is_array()) {
            for (const auto& option : args["filterOptions"]) {
                const int id   = next_breakpoint_id();
                Json bp        = Json::object();
                bp["id"]       = id;
                bp["verified"] = false;
                bp["message"]  = "Wait verify.";
                breakpoints.push_back(std::move(bp));
                Json item  = Json::object();
                item["id"] = id;
                if (option.is_object()) {
                    if (option.contains("filterId")) {
                        item["filterId"] = option["filterId"];
                    }
                    if (option.contains("condition")) {
                        item["condition"] = option["condition"];
                    }
                }
                filters.push_back(std::move(item));
            }
        }
        Json body           = Json::object();
        body["breakpoints"] = breakpoints;
        respond_success(request, std::move(body));
        config_["exception_breakpoints"] = filters;
        if (state_ == "initialized") {
            Json command         = Json::object();
            command["cmd"]       = "setExceptionBreakpoints";
            command["arguments"] = filters;
            broadcast(command);
        }
    }

    void Master::on_set_instruction_breakpoints(const Json& request) {
        Json args        = request.contains("arguments") && request["arguments"].is_object()
                               ? request["arguments"]
                               : Json::object();
        Json breakpoints = args.contains("breakpoints") && args["breakpoints"].is_array()
                               ? args["breakpoints"]
                               : Json::array();
        for (auto& bp : breakpoints) {
            bp["id"]       = next_breakpoint_id();
            bp["verified"] = false;
            bp["message"]  = "Wait verify.";
        }
        Json body           = Json::object();
        body["breakpoints"] = breakpoints;
        respond_success(request, std::move(body));

        // Only references handed out by a worker can be forwarded; the thread
        // prefix is stripped because each worker matches on its own identity.
        Json forwarded = Json::array();
        for (auto& bp : breakpoints) {
            const std::string reference = json_string(bp, "instructionReference");
            long thread                 = 0;
            std::string rest;
            if (split_encoded(reference, "inst_", thread, rest)) {
                bp["instructionReference"] = rest;
                forwarded.push_back(bp);
            }
        }
        config_["instruction_breakpoints"] = forwarded;
        if (state_ == "initialized") {
            for (const auto& entry : workers_) {
                Json command           = Json::object();
                command["cmd"]         = "setInstructionBreakpoints";
                command["breakpoints"] = forwarded;
                send_to(entry.first, command);
            }
        }
    }

    void Master::on_stack_trace(const Json& request) {
        const Json& args  = request.contains("arguments") ? request["arguments"] : Json();
        const long thread = static_cast<long>(as_int64(args.is_object() && args.contains("threadId") ? args["threadId"] : Json(0)));
        if (!check_thread(request, thread)) {
            return;
        }
        Json command          = Json::object();
        command["cmd"]        = "stackTrace";
        command["command"]    = json_string(request, "command");
        command["seq"]        = as_int64(request["seq"]);
        command["startFrame"] = args.contains("startFrame") ? args["startFrame"] : Json();
        command["levels"]     = args.contains("levels") ? args["levels"] : Json();
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_scopes(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        if (!args.is_object() || !args.contains("frameId") || !args["frameId"].is_number()) {
            respond_error(request, "No frameId");
            return;
        }
        const long long frame = as_int64(args["frameId"]);
        const long thread     = id_thread(frame, kThreadShift);
        if (!check_thread(request, thread)) {
            return;
        }
        Json command       = Json::object();
        command["cmd"]     = "scopes";
        command["command"] = json_string(request, "command");
        command["seq"]     = as_int64(request["seq"]);
        command["frameId"] = id_local(frame, kThreadShift);
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_variables(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        const long long reference =
            as_int64(args.is_object() && args.contains("variablesReference") ? args["variablesReference"] : Json(0));
        const long thread = id_thread(reference, kThreadShift);
        if (!check_thread(request, thread)) {
            return;
        }
        Json command       = Json::object();
        command["cmd"]     = "variables";
        command["command"] = json_string(request, "command");
        command["seq"]     = as_int64(request["seq"]);
        command["valueId"] = id_local(reference, kThreadShift);
        if (args.contains("filter")) {
            command["filter"] = args["filter"];
        }
        if (args.contains("start")) {
            command["start"] = args["start"];
        }
        if (args.contains("count")) {
            command["count"] = args["count"];
        }
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_evaluate(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        if (!args.is_object() || !args.contains("frameId") || !args["frameId"].is_number()) {
            respond_error(request, "Please pause to evaluate expressions");
            return;
        }
        if (!args.contains("expression") || !args["expression"].is_string()) {
            respond_error(request, "Error expression");
            return;
        }
        const long long frame = as_int64(args["frameId"]);
        const long thread     = id_thread(frame, kThreadShift);
        if (!check_thread(request, thread)) {
            return;
        }
        Json command          = Json::object();
        command["cmd"]        = "evaluate";
        command["command"]    = json_string(request, "command");
        command["seq"]        = as_int64(request["seq"]);
        command["frameId"]    = id_local(frame, kThreadShift);
        command["expression"] = args["expression"];
        command["context"]    = args.contains("context") && args["context"].is_string()
                                    ? args["context"]
                                    : Json("repl");
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_threads(const Json& request) {
        Json threads = Json::array();
        for (const auto& entry : workers_) {
            if (!entry.second.connected) {
                continue;
            }
            const std::string id = number(static_cast<long long>(entry.first));
            std::string name     = entry.second.name;
            if (name.empty()) {
                name = "Thread (" + id + ")";
            } else {
                // "${id}" keeps the old placeholder contract.
                for (std::size_t pos = name.find("${id}"); pos != std::string::npos;
                     pos             = name.find("${id}", pos)) {
                    name.replace(pos, 5, id);
                }
            }
            Json thread    = Json::object();
            thread["name"] = name;
            thread["id"]   = entry.first;
            threads.push_back(std::move(thread));
        }
        std::sort(threads.begin(), threads.end(), [](const Json& a, const Json& b) {
            return json_string(a, "name") < json_string(b, "name");
        });
        Json body       = Json::object();
        body["threads"] = std::move(threads);
        respond_success(request, std::move(body));
    }

    void Master::on_disconnect(const Json& request) {
        respond_success(request);
        Json args            = request.contains("arguments") && request["arguments"].is_object()
                                   ? request["arguments"]
                                   : Json::object();
        const bool terminate = args.contains("terminateDebuggee")
                                   ? json_bool(args, "terminateDebuggee")
                                   : json_bool(config_, "launch");
        {
            Json command   = Json::object();
            command["cmd"] = "disconnect";
            broadcast(command);
        }
        if (json_bool(args, "suspendDebuggee")) {
            Json command   = Json::object();
            command["cmd"] = "suspend";
            broadcast(command);
            return;
        }
        if (terminate && router_.finish) {
            router_.finish(true);
        }
    }

    void Master::on_terminate(const Json& request) {
        respond_success(request);
        {
            Json command   = Json::object();
            command["cmd"] = "disconnect";
            broadcast(command);
        }
        close_process_ = true;
        if (router_.finish) {
            router_.finish(true);
        }
    }

    void Master::on_restart(const Json& request) {
        Json args        = request.contains("arguments") && request["arguments"].is_object()
                               ? request["arguments"]
                               : Json::object();
        Json launch_args = args.contains("arguments") && args["arguments"].is_object()
                               ? args["arguments"]
                               : Json();
        respond_success(request);
        {
            Json command   = Json::object();
            command["cmd"] = "disconnect";
            broadcast(command);
        }
        if (!launch_args.is_null()) {
            resolve_session_config(launch_args);
            config_["initialize"] = launch_args;
        }
        std::vector<LdWorkerId> workers;
        for (const auto& entry : workers_) {
            workers.push_back(entry.first);
        }
        for (LdWorkerId worker : workers) {
            initialize_worker(worker);
        }
    }

    void Master::on_terminate_threads(const Json& request) {
        respond_success(request);
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        if (!args.is_object() || !args.contains("threadIds") || !args["threadIds"].is_array()) {
            return;
        }
        for (const auto& id : args["threadIds"]) {
            Json command   = Json::object();
            command["cmd"] = "disconnect";
            send_to(static_cast<LdWorkerId>(as_int64(id)), command);
        }
    }

    void Master::on_pause(const Json& request) {
        const Json& args  = request.contains("arguments") ? request["arguments"] : Json();
        const long thread = static_cast<long>(as_int64(args.is_object() && args.contains("threadId") ? args["threadId"] : Json(0)));
        if (!check_thread(request, thread)) {
            return;
        }
        Json command      = Json::object();
        command["cmd"]    = "stop";
        command["reason"] = "pause";
        send_to(static_cast<LdWorkerId>(thread), command);
        respond_success(request);
    }

    void Master::on_continue(const Json& request) {
        resume_stopped_workers();
        Json body                   = Json::object();
        body["allThreadsContinued"] = true;
        respond_success(request, std::move(body));
    }

    void Master::on_step(const Json& request, const char* command_name) {
        const Json& args  = request.contains("arguments") ? request["arguments"] : Json();
        const long thread = static_cast<long>(as_int64(args.is_object() && args.contains("threadId") ? args["threadId"] : Json(0)));
        if (!check_thread(request, thread)) {
            return;
        }
        {
            workers_[static_cast<LdWorkerId>(thread)].stopped = false;
            Json command                                      = Json::object();
            command["cmd"]                                    = command_name;
            send_to(static_cast<LdWorkerId>(thread), command);
        }
        {
            Json command   = Json::object();
            command["cmd"] = "run";
            broadcast_stopped_except(static_cast<LdWorkerId>(thread), command);
        }
        {
            Json body                   = Json::object();
            body["allThreadsContinued"] = true;
            emit_event("continued", std::move(body));
        }
        respond_success(request);
    }

    void Master::on_source(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        const long long reference =
            as_int64(args.is_object() && args.contains("sourceReference") ? args["sourceReference"] : Json(0));
        const long thread = id_thread(reference, kSourceShift);
        if (!check_thread(request, thread)) {
            return;
        }
        Json command               = Json::object();
        command["cmd"]             = "source";
        command["command"]         = json_string(request, "command");
        command["seq"]             = as_int64(request["seq"]);
        command["sourceReference"] = id_local(reference, kSourceShift);
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_exception_info(const Json& request) {
        const Json& args  = request.contains("arguments") ? request["arguments"] : Json();
        const long thread = static_cast<long>(as_int64(args.is_object() && args.contains("threadId") ? args["threadId"] : Json(0)));
        if (!check_thread(request, thread)) {
            return;
        }
        Json command       = Json::object();
        command["cmd"]     = "exceptionInfo";
        command["command"] = json_string(request, "command");
        command["seq"]     = as_int64(request["seq"]);
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_set_variable(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        const long long reference =
            as_int64(args.is_object() && args.contains("variablesReference") ? args["variablesReference"] : Json(0));
        const long thread = id_thread(reference, kThreadShift);
        if (!check_thread(request, thread)) {
            return;
        }
        Json command       = Json::object();
        command["cmd"]     = "setVariable";
        command["command"] = json_string(request, "command");
        command["seq"]     = as_int64(request["seq"]);
        command["valueId"] = id_local(reference, kThreadShift);
        if (args.contains("name")) {
            command["name"] = args["name"];
        }
        if (args.contains("value")) {
            command["value"] = args["value"];
        }
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_set_expression(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        const long long frame =
            as_int64(args.is_object() && args.contains("frameId") ? args["frameId"] : Json(0));
        const long thread = id_thread(frame, kThreadShift);
        if (!check_thread(request, thread)) {
            return;
        }
        Json command       = Json::object();
        command["cmd"]     = "setExpression";
        command["command"] = json_string(request, "command");
        command["seq"]     = as_int64(request["seq"]);
        command["frameId"] = id_local(frame, kThreadShift);
        if (args.contains("expression")) {
            command["expression"] = args["expression"];
        }
        if (args.contains("value")) {
            command["value"] = args["value"];
        }
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_loaded_sources(const Json& request) {
        Json body       = Json::object();
        body["sources"] = Json::array();
        respond_success(request, std::move(body));
        Json command   = Json::object();
        command["cmd"] = "loadedSources";
        broadcast(command);
    }

    void Master::on_restart_frame(const Json& request) {
        const Json& args = request.contains("arguments") ? request["arguments"] : Json();
        const long long frame =
            as_int64(args.is_object() && args.contains("frameId") ? args["frameId"] : Json(0));
        const long thread = id_thread(frame, kThreadShift);
        if (!check_thread(request, thread)) {
            return;
        }
        respond_success(request);
        Json command       = Json::object();
        command["cmd"]     = "restartFrame";
        command["frameId"] = id_local(frame, kThreadShift);
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_read_memory(const Json& request) {
        const Json& args            = request.contains("arguments") ? request["arguments"] : Json();
        const std::string reference = args.is_object() ? json_string(args, "memoryReference") : "";
        long thread                 = 0;
        std::string rest;
        if (!split_encoded(reference, "memory_", thread, rest)) {
            respond_error(request, "Error memoryReference");
            return;
        }
        if (!check_thread(request, thread)) {
            return;
        }
        Json command               = Json::object();
        command["cmd"]             = "readMemory";
        command["command"]         = json_string(request, "command");
        command["seq"]             = as_int64(request["seq"]);
        command["memoryReference"] = std::atoll(rest.c_str());
        if (args.contains("offset")) {
            command["offset"] = args["offset"];
        }
        if (args.contains("count")) {
            command["count"] = args["count"];
        }
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_write_memory(const Json& request) {
        const Json& args            = request.contains("arguments") ? request["arguments"] : Json();
        const std::string reference = args.is_object() ? json_string(args, "memoryReference") : "";
        long thread                 = 0;
        std::string rest;
        if (!split_encoded(reference, "memory_", thread, rest)) {
            respond_error(request, "Error memoryReference");
            return;
        }
        if (!check_thread(request, thread)) {
            return;
        }
        Json command               = Json::object();
        command["cmd"]             = "writeMemory";
        command["command"]         = json_string(request, "command");
        command["seq"]             = as_int64(request["seq"]);
        command["memoryReference"] = std::atoll(rest.c_str());
        if (args.contains("offset")) {
            command["offset"] = args["offset"];
        }
        if (args.contains("data")) {
            command["data"] = args["data"];
        }
        if (args.contains("allowPartial")) {
            command["allowPartial"] = args["allowPartial"];
        }
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_disassemble(const Json& request) {
        const Json& args            = request.contains("arguments") ? request["arguments"] : Json();
        const std::string reference = args.is_object() ? json_string(args, "memoryReference") : "";
        long thread                 = 0;
        std::string rest;
        if (!split_encoded(reference, "inst_", thread, rest)) {
            std::string memory;
            if (!split_encoded(reference, "memory_", thread, memory)) {
                respond_error(request, "Invalid memoryReference");
                return;
            }
            rest = memory;
        }
        if (!check_thread(request, thread)) {
            return;
        }
        Json command       = Json::object();
        command["cmd"]     = "disassemble";
        command["command"] = json_string(request, "command");
        command["seq"]     = as_int64(request["seq"]);
        command["refId"]   = rest;
        if (args.contains("offset")) {
            command["offset"] = args["offset"];
        }
        if (args.contains("instructionOffset")) {
            command["instructionOffset"] = args["instructionOffset"];
        }
        if (args.contains("instructionCount")) {
            command["instructionCount"] = args["instructionCount"];
        }
        if (args.contains("resolveSymbols")) {
            command["resolveSymbols"] = args["resolveSymbols"];
        }
        send_to(static_cast<LdWorkerId>(thread), command);
    }

    void Master::on_custom_integer(const Json& request, bool hex) {
        respond_success(request);
        Json command   = Json::object();
        command["cmd"] = hex ? "customRequestShowIntegerAsHex" : "customRequestShowIntegerAsDec";
        broadcast(command);
    }

    void Master::handle_worker_message(LdWorkerId worker, const Json& message) {
        if (!message.is_object()) {
            return;
        }
        const std::string command = json_string(message, "command");
        const std::string cmd     = json_string(message, "cmd");
        const std::string event   = json_string(message, "event");
        const std::string key     = !command.empty() ? command : (!cmd.empty() ? cmd : event);

        const bool success        = json_bool(message, "success", true);
        const std::string failure = json_string(message, "message");
        Json body                 = message.contains("body") && message["body"].is_object() ? message["body"]
                                                                                            : Json::object();

        if (key == "stackTrace") {
            if (!success) {
                respond_error(message, failure);
                return;
            }
            if (body.contains("stackFrames") && body["stackFrames"].is_array()) {
                for (auto& frame : body["stackFrames"]) {
                    frame["id"] = (static_cast<long long>(worker) << kThreadShift) | as_int64(frame.contains("id") ? frame["id"] : Json(0));
                    if (frame.contains("source") && frame["source"].is_object() && frame["source"].contains("sourceReference") && as_int64(frame["source"]["sourceReference"]) != 0) {
                        frame["source"]["sourceReference"] =
                            (static_cast<long long>(worker) << kSourceShift) | as_int64(frame["source"]["sourceReference"]);
                    }
                    const std::string reference = json_string(frame, "instructionPointerReference");
                    if (!reference.empty()) {
                        frame["instructionPointerReference"] =
                            "inst_" + number(static_cast<long long>(worker)) + "x" + reference;
                    }
                }
            }
            respond_success(message, body);
            return;
        }
        if (key == "scopes") {
            if (body.contains("scopes") && body["scopes"].is_array()) {
                for (auto& scope : body["scopes"]) {
                    const long long reference = scope.contains("variablesReference")
                                                    ? as_int64(scope["variablesReference"])
                                                    : 0;
                    scope["variablesReference"] =
                        reference == 0
                            ? Json(0)
                            : Json((static_cast<long long>(worker) << kThreadShift) | reference);
                }
            }
            respond_success(message, body);
            return;
        }
        if (key == "variables") {
            if (!success) {
                respond_error(message, failure);
                return;
            }
            if (body.contains("variables") && body["variables"].is_array()) {
                for (auto& variable : body["variables"]) {
                    const long long reference = variable.contains("variablesReference")
                                                    ? as_int64(variable["variablesReference"])
                                                    : 0;
                    variable["variablesReference"] =
                        reference == 0
                            ? Json(0)
                            : Json((static_cast<long long>(worker) << kThreadShift) | reference);
                    const std::string memory = json_string(variable, "memoryReference");
                    if (!memory.empty()) {
                        variable["memoryReference"] =
                            "memory_" + number(static_cast<long long>(worker)) + "x" + memory;
                    }
                }
            }
            respond_success(message, body);
            return;
        }
        if (key == "evaluate") {
            if (!success) {
                respond_error(message, failure);
                return;
            }
            const long long reference = body.contains("variablesReference")
                                            ? as_int64(body["variablesReference"])
                                            : 0;
            body["variablesReference"] =
                reference == 0 ? Json(0)
                               : Json((static_cast<long long>(worker) << kThreadShift) | reference);
            const std::string memory = json_string(body, "memoryReference");
            if (!memory.empty()) {
                body["memoryReference"] =
                    "memory_" + number(static_cast<long long>(worker)) + "x" + memory;
            }
            respond_success(message, body);
            return;
        }
        if (key == "setExpression") {
            if (!success) {
                respond_error(message, failure);
                return;
            }
            const long long reference = body.contains("variablesReference")
                                            ? as_int64(body["variablesReference"])
                                            : 0;
            body["variablesReference"] =
                reference == 0 ? Json(0)
                               : Json((static_cast<long long>(worker) << kThreadShift) | reference);
            respond_success(message, body);
            return;
        }
        if (key == "source") {
            Json content           = Json::object();
            const std::string text = json_string(body, "content");
            content["content"] =
                text.empty() ? Json("Source not available") : Json(text);
            content["mimeType"] = "text/x-lua";
            respond_success(message, std::move(content));
            return;
        }
        if (key == "disassemble") {
            if (!success) {
                respond_error(message, failure);
                return;
            }
            if (body.contains("instructions") && body["instructions"].is_array()) {
                for (auto& inst : body["instructions"]) {
                    const std::string reference = json_string(inst, "instructionReference");
                    if (!reference.empty()) {
                        inst["instructionReference"] =
                            "inst_" + number(static_cast<long long>(worker)) + "x" + reference;
                    }
                }
            }
            respond_success(message, body);
            return;
        }
        if (key == "exceptionInfo" || key == "setVariable" || key == "readMemory" || key == "writeMemory") {
            if (!success) {
                respond_error(message, failure);
                return;
            }
            respond_success(message, body);
            return;
        }
        if (key == "setThreadName") {
            // The worker may name itself; the payload is either "name" or the raw
            // string the old channel carried.
            std::string name = json_string(message, "name");
            if (name.empty() && message.contains("value") && message["value"].is_string()) {
                name = message["value"].get<std::string>();
            }
            if (!name.empty()) {
                set_thread_name(worker, name);
            }
            return;
        }

        // Events ---------------------------------------------------------
        // Worker events are flat (`{cmd, reason, line, ...}`), and the old master
        // forwarded that whole table as the DAP body, so the payload is the
        // message itself rather than a nested `body`.
        if (key == "eventStop") {
            workers_[worker].stopped = true;
            Json event               = message;
            event["threadId"]        = worker;
            emit_event("stopped", std::move(event));
            return;
        }
        if (key == "eventBreakpoint") {
            emit_event("breakpoint", message);
            return;
        }
        if (key == "eventOutput") {
            Json event                = message;
            const long long reference = event.contains("variablesReference")
                                            ? as_int64(event["variablesReference"])
                                            : 0;
            if (reference != 0) {
                event["variablesReference"] =
                    (static_cast<long long>(worker) << kThreadShift) | reference;
            }
            emit_event("output", std::move(event));
            return;
        }
        if (key == "eventThread") {
            Json event               = message;
            event["threadId"]        = worker;
            const std::string reason = json_string(event, "reason");
            if (reason == "started") {
                workers_[worker].connected = true;
            } else if (reason == "exited") {
                workers_[worker].connected = false;
            }
            emit_event("thread", std::move(event));
            return;
        }
        if (key == "eventInvalidated") {
            if (!json_bool(client_, "supportsInvalidatedEvent")) {
                return;
            }
            Json event        = message;
            event["threadId"] = worker;
            emit_event("invalidated", std::move(event));
            return;
        }
        if (key == "eventLoadedSource") {
            Json event = message;
            if (event.contains("source") && event["source"].is_object() && event["source"].contains("sourceReference")) {
                event["source"]["sourceReference"] =
                    (static_cast<long long>(worker) << kSourceShift) | as_int64(event["source"]["sourceReference"]);
            }
            emit_event("loadedSource", std::move(event));
            return;
        }
        if (key == "eventMemory") {
            Json event                  = message;
            const std::string reference = json_string(event, "memoryReference");
            if (!reference.empty()) {
                event["memoryReference"] =
                    "memory_" + number(static_cast<long long>(worker)) + "x" + reference;
            }
            emit_event("memory", std::move(event));
            return;
        }
        if (key == "terminated") {
            emit_event("terminated", body);
            return;
        }
        // Only responses are forwarded verbatim: a worker may answer a command
        // the master has no encoding rule for, and the client is waiting on it.
        // Unknown *events* are dropped (the Lua master did the same), otherwise a
        // worker-side notification would surface as an unknown DAP event.
        if (json_string(message, "type") == "response" || (!message.contains("type") && !message.contains("event") && !message.contains("cmd") && !message.contains("command"))) {
            send(message);
            return;
        }
        // A fully formed DAP message from a worker is forwarded as-is.
        if (message.contains("type") || message.contains("event")) {
            send(message);
            return;
        }
        log_warn("master: unhandled worker message '%s'", key.c_str());
    }

}  // namespace luadebug::debugger::session
