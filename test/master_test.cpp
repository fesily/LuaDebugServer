// Verifies the session master's DAP rules without a socket or a debuggee:
// request gating, config resolution and replay, reference-id encoding, worker
// routing, thread registry and the disconnect/step semantics.

#include <cstdio>
#include <string>
#include <vector>

#include "common/json.h"
#include "session/master.h"

namespace {

    using luadebug::debugger::Json;
    using luadebug::debugger::session::Master;

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    std::string str(const Json& value, const char* key) {
        if (!value.is_object()) {
            return {};
        }
        auto it = value.find(key);
        if (it == value.end() || !it->is_string()) {
            return {};
        }
        return it->get<std::string>();
    }

    long long num(const Json& value, const char* key) {
        if (!value.is_object()) {
            return 0;
        }
        auto it = value.find(key);
        if (it == value.end() || !it->is_number()) {
            return 0;
        }
        return it->get<long long>();
    }

    struct SentCommand {
        unsigned long long worker = 0;
        Json command;
    };

    struct Harness {
        std::vector<Json> out;
        std::vector<SentCommand> commands;
        bool finished = false;
        bool terminate = false;

        Master make() {
            Master::Router router;
            router.to_worker = [this](unsigned long long worker, const Json& command) {
                commands.push_back(SentCommand { worker, command });
                return true;
            };
            router.to_all = [this](const Json& command) {
                commands.push_back(SentCommand { 0, command });
            };
            router.to_all_except = [this](unsigned long long worker, const Json& command) {
                commands.push_back(SentCommand { worker, command });
            };
            router.finish = [this](bool terminate_debuggee) {
                finished  = true;
                terminate = terminate_debuggee;
            };
            return Master([this](const Json& message) { out.push_back(message); }, router);
        }

        // Pops the messages produced since the last call.
        std::vector<Json> drain() {
            auto copy = out;
            out.clear();
            return copy;
        }

        std::vector<SentCommand> drain_commands() {
            auto copy = commands;
            commands.clear();
            return copy;
        }

        const Json* find(const std::vector<Json>& list, const char* type, const char* name) const {
            for (const auto& message : list) {
                if (str(message, "type") != type) {
                    continue;
                }
                const char* key = std::string(type) == "response" ? "command" : "event";
                if (str(message, key) == name) {
                    return &message;
                }
            }
            return nullptr;
        }
    };

    Json request(const char* command, int seq, Json arguments = Json()) {
        Json message     = Json::object();
        message["type"]    = "request";
        message["command"] = command;
        message["seq"]     = seq;
        if (!arguments.is_null()) {
            message["arguments"] = std::move(arguments);
        }
        return message;
    }

} // namespace

int main() {
    Harness harness;
    Master master = harness.make();

    // ---- 1. gating before initialize
    master.handle_client_message(request("threads", 1));
    {
        auto out = harness.drain();
        const Json* response = harness.find(out, "response", "threads");
        expect(response != nullptr, "a request before initialize is answered");
        if (response) {
            expect(!(*response)["success"].get<bool>(), "it is rejected");
            expect(str(*response, "message").find("birth") != std::string::npos,
                   "the message names the pre-initialize state");
        }
    }

    // ---- 2. initialize
    master.handle_client_message(request("initialize", 2, Json::object()));
    {
        auto out = harness.drain();
        const Json* response = harness.find(out, "response", "initialize");
        expect(response != nullptr, "initialize is answered");
        if (response) {
            expect((*response)["success"].get<bool>(), "initialize succeeds");
            expect(response->contains("body") && (*response)["body"].contains("supportsConfigurationDoneRequest"),
                   "the response carries the DAP capabilities");
        }
        expect(harness.find(out, "event", "initialized") != nullptr, "initialized event follows");
        const Json* capabilities_event = harness.find(out, "event", "capabilities");
        expect(capabilities_event != nullptr, "capabilities event follows");
        if (capabilities_event) {
            expect((*capabilities_event)["body"].contains("capabilities"),
                   "the capabilities event carries the table");
        }
    }

    // ---- 3. launch resolves the config defaults
    Json launch_args     = Json::object();
    launch_args["name"]  = "test";
    launch_args["request"] = "launch";
    launch_args["program"] = "/tmp/x.lua";
    master.handle_client_message(request("launch", 3, launch_args));
    {
        auto out = harness.drain();
        const Json* response = harness.find(out, "response", "launch");
        expect(response != nullptr && (*response)["success"].get<bool>(), "launch succeeds");
        const Json& config = master.config();
        expect(config.contains("launch") && config["launch"].is_boolean()
                   && config["launch"].get<bool>(),
               "launch mode is cached");
    }
    {
        // Defaults must be filled exactly like resolve_config.lua.
        const Json& initialize = master.config()["initialize"];
        expect(str(initialize, "type") == "lua", "type defaults to lua");
        expect(initialize["stopOnEntry"].get<bool>(), "stopOnEntry defaults to true");
        expect(initialize["outputCapture"].is_array() && initialize["outputCapture"].size() == 4,
               "internalConsole captures the four streams");
        expect(str(initialize, "pathFormat") == "path", "pathFormat defaults to path");
    }

    // ---- 4. breakpoints are cached and replayed to a late worker
    Json source        = Json::object();
    source["path"]     = "/tmp/x.lua";
    Json breakpoints   = Json::array();
    Json first_bp      = Json::object();
    first_bp["line"]   = 3;
    breakpoints.push_back(first_bp);
    breakpoints.push_back(Json::object());
    breakpoints[1]["line"] = 9;
    Json args              = Json::object();
    args["source"]         = source;
    args["breakpoints"]    = breakpoints;
    master.handle_client_message(request("setBreakpoints", 4, args));
    {
        auto out = harness.drain();
        const Json* response = harness.find(out, "response", "setBreakpoints");
        expect(response != nullptr, "setBreakpoints is answered immediately");
        if (response) {
            const Json& bps = (*response)["body"]["breakpoints"];
            expect(bps.is_array() && bps.size() == 2, "both breakpoints are echoed");
            expect(num(bps[0], "id") == 1 && num(bps[1], "id") == 2,
                   "ids are allocated in order");
            expect(!bps[0]["verified"].get<bool>(), "pending breakpoints start unverified");
            expect(str(bps[0], "message").find("Wait verify") != std::string::npos,
                   "the message explains the pending state");
        }
        expect(harness.drain_commands().empty(),
               "nothing is forwarded before configurationDone");
    }

    // ---- 5. configurationDone initializes the connected worker
    master.worker_ready(2);
    {
        // worker_ready before configurationDone must not initialize yet.
        expect(harness.drain_commands().empty(), "a worker joining early waits for configurationDone");
    }
    master.handle_client_message(request("configurationDone", 5));
    {
        auto commands = harness.drain_commands();
        bool saw_initializing = false;
        bool saw_breakpoints  = false;
        bool saw_functions    = false;
        bool saw_exceptions   = false;
        bool saw_instructions = false;
        bool saw_stop         = false;
        for (const auto& entry : commands) {
            expect(entry.worker == 2, "every replay command targets the joined worker");
            const std::string cmd = str(entry.command, "cmd");
            if (cmd == "initializing") {
                saw_initializing = true;
                expect(entry.command["config"].is_object(), "initializing carries the config");
            } else if (cmd == "setBreakpoints") {
                saw_breakpoints = true;
                expect(entry.command["breakpoints"].size() == 2,
                       "the cached breakpoints are replayed");
            } else if (cmd == "setFunctionBreakpoints") {
                saw_functions = true;
            } else if (cmd == "setExceptionBreakpoints") {
                saw_exceptions = true;
            } else if (cmd == "setInstructionBreakpoints") {
                saw_instructions = true;
            } else if (cmd == "stop") {
                saw_stop = true;
                expect(str(entry.command, "reason") == "entry", "launch stops on entry");
            }
        }
        expect(saw_initializing, "initializing comes first");
        expect(saw_breakpoints, "breakpoints are replayed");
        expect(saw_functions && saw_exceptions && saw_instructions,
               "the other breakpoint kinds are replayed");
        expect(saw_stop, "stopOnEntry is applied");
        harness.drain();
    }

    // ---- 6. instruction references are stripped of their thread prefix
    Json inst_bps      = Json::array();
    Json inst_bp       = Json::object();
    inst_bp["instructionReference"] = "inst_2xbp_1_4_0_x.lua";
    inst_bps.push_back(inst_bp);
    Json inst_args         = Json::object();
    inst_args["breakpoints"] = inst_bps;
    master.handle_client_message(request("setInstructionBreakpoints", 6, inst_args));
    {
        auto out = harness.drain();
        const Json* response = harness.find(out, "response", "setInstructionBreakpoints");
        expect(response != nullptr && (*response)["success"].get<bool>(),
               "setInstructionBreakpoints succeeds");
        auto commands = harness.drain_commands();
        bool forwarded = false;
        for (const auto& entry : commands) {
            if (str(entry.command, "cmd") == "setInstructionBreakpoints") {
                forwarded = true;
                const Json& bps = entry.command["breakpoints"];
                expect(bps.is_array() && bps.size() == 1, "one reference is forwarded");
                if (bps.is_array() && !bps.empty()) {
                    expect(str(bps[0], "instructionReference") == "bp_1_4_0_x.lua",
                           "the thread prefix is stripped for the worker");
                }
            }
        }
        expect(forwarded, "the worker receives the instruction breakpoints");
    }

    // ---- 7. routing with encoded ids
    master.handle_client_message(request("stackTrace", 7, Json::object()));
    {
        auto out = harness.drain();
        const Json* response = harness.find(out, "response", "stackTrace");
        expect(response != nullptr && !(*response)["success"].get<bool>(),
               "stackTrace without a thread is rejected");
    }
    {
        Json args         = Json::object();
        args["threadId"]  = 2;
        master.handle_client_message(request("stackTrace", 8, args));
        auto commands = harness.drain_commands();
        expect(commands.size() == 1 && commands[0].worker == 2, "stackTrace reaches the worker");
        if (!commands.empty()) {
            expect(str(commands[0].command, "cmd") == "stackTrace", "the command is stackTrace");
            expect(num(commands[0].command, "seq") == 8, "the client seq is passed through");
        }
        harness.drain();
    }
    {
        Json args        = Json::object();
        args["frameId"]  = (2LL << 24) | 7;
        master.handle_client_message(request("scopes", 9, args));
        auto commands = harness.drain_commands();
        expect(commands.size() == 1 && commands[0].worker == 2, "scopes reaches the worker");
        if (!commands.empty()) {
            expect(num(commands[0].command, "frameId") == 7, "the thread prefix is removed");
        }
        harness.drain();
    }
    {
        Json args                  = Json::object();
        args["variablesReference"] = (2LL << 24) | 9;
        master.handle_client_message(request("variables", 10, args));
        auto commands = harness.drain_commands();
        expect(commands.size() == 1, "variables reaches the worker");
        if (!commands.empty()) {
            expect(num(commands[0].command, "valueId") == 9, "the value id is decoded");
        }
        harness.drain();
    }
    {
        Json args         = Json::object();
        args["threadId"]  = 99;
        master.handle_client_message(request("pause", 11, args));
        auto out             = harness.drain();
        const Json* response = harness.find(out, "response", "pause");
        expect(response != nullptr && !(*response)["success"].get<bool>(),
               "an unknown thread is refused");
        expect(str(*response, "message").find("Not found thread [99]") != std::string::npos,
               "the refusal names the thread");
        harness.drain_commands();
    }

    // ---- 8. worker responses are re-encoded
    {
        Json body = Json::object();
        Json frame = Json::object();
        frame["id"]    = 3;
        frame["name"]  = "f";
        frame["line"]  = 1;
        Json src = Json::object();
        src["sourceReference"] = 5;
        src["path"]            = "/tmp/x.lua";
        frame["source"]        = src;
        frame["instructionPointerReference"] = "bp_1_4_0_x.lua";
        Json frames        = Json::array();
        frames.push_back(frame);
        body["stackFrames"] = frames;
        Json message = Json::object();
        message["type"]    = "response";
        message["command"] = "stackTrace";
        message["seq"]     = 8;
        message["success"] = true;
        message["body"]    = body;
        master.handle_worker_message(2, message);
        auto out             = harness.drain();
        const Json* response = harness.find(out, "response", "stackTrace");
        expect(response != nullptr && (*response)["success"].get<bool>(),
               "the worker stackTrace response is forwarded");
        if (response) {
            const Json& sent = (*response)["body"]["stackFrames"][0];
            expect(num(sent, "id") == ((2LL << 24) | 3), "frame ids carry the thread");
            expect(num(sent["source"], "sourceReference") == ((2LL << 32) | 5),
                   "source references carry the thread");
            expect(str(sent, "instructionPointerReference") == "inst_2xbp_1_4_0_x.lua",
                   "instruction references carry the thread");
            expect(num(*response, "request_seq") == 8, "the response echoes the request seq");
        }
    }
    {
        Json body      = Json::object();
        Json variable  = Json::object();
        variable["name"]               = "t";
        variable["variablesReference"] = 11;
        variable["memoryReference"]    = "42";
        Json variables = Json::array();
        variables.push_back(variable);
        body["variables"] = variables;
        Json message      = Json::object();
        message["type"]    = "response";
        message["command"] = "variables";
        message["seq"]     = 10;
        message["success"] = true;
        message["body"]    = body;
        master.handle_worker_message(2, message);
        auto out             = harness.drain();
        const Json* response = harness.find(out, "response", "variables");
        expect(response != nullptr, "the variables response is forwarded");
        if (response) {
            const Json& sent = (*response)["body"]["variables"][0];
            expect(num(sent, "variablesReference") == ((2LL << 24) | 11),
                   "variable references carry the thread");
            expect(str(sent, "memoryReference") == "memory_2x42",
                   "memory references carry the thread");
        }
    }
    {
        Json message      = Json::object();
        message["cmd"]    = "eventStop";
        message["reason"] = "breakpoint";
        message["line"]   = 4;
        master.handle_worker_message(2, message);
        auto out            = harness.drain();
        const Json* event   = harness.find(out, "event", "stopped");
        expect(event != nullptr, "a stop event is forwarded");
        if (event) {
            const Json& body = (*event)["body"];
            expect(num(body, "threadId") == 2, "the stop carries its thread id");
            expect(num(body, "line") == 4, "the stop keeps the line");
            expect(str(body, "reason") == "breakpoint", "the stop keeps its reason");
        }
    }

    // ---- 9. the thread registry
    master.set_thread_name(2, "worker ${id}");
    master.worker_ready(3);
    harness.drain_commands();  // the late worker's replay is not under test here
    master.handle_client_message(request("threads", 12));
    {
        auto out             = harness.drain();
        const Json* response = harness.find(out, "response", "threads");
        expect(response != nullptr, "threads is answered");
        if (response) {
            const Json& threads = (*response)["body"]["threads"];
            expect(threads.is_array() && threads.size() == 2, "both workers are listed");
            if (threads.is_array() && threads.size() == 2) {
                expect(str(threads[0], "name") == "Thread (3)", "unnamed workers keep the default");
                expect(str(threads[1], "name") == "worker 2", "${id} is substituted");
                expect(num(threads[0], "id") == 3, "ids stay the worker ids");
            }
        }
    }

    // ---- 10. step and continue semantics
    {
        Json args        = Json::object();
        args["threadId"] = 2;
        master.handle_client_message(request("continue", 13, args));
        auto out             = harness.drain();
        const Json* response = harness.find(out, "response", "continue");
        expect(response != nullptr && (*response)["success"].get<bool>(), "continue succeeds");
        if (response) {
            expect((*response)["body"]["allThreadsContinued"].get<bool>(),
                   "continue resumes every thread");
        }
        auto commands = harness.drain_commands();
        // Only the worker that reported a stop is resumed: a `run` queued for a
        // worker that had not stopped yet would release its *next* stop.
        expect(commands.size() == 1 && commands[0].worker == 2,
               "continue resumes the stopped worker only");
        if (!commands.empty()) {
            expect(str(commands[0].command, "cmd") == "run", "the command is run");
        }
    }
    {
        // With two stopped workers, the same request reaches both.
        Json stop      = Json::object();
        stop["cmd"]    = "eventStop";
        stop["reason"] = "breakpoint";
        stop["line"]   = 9;
        master.handle_worker_message(2, stop);
        master.handle_worker_message(3, stop);
        harness.drain();
        Json args        = Json::object();
        args["threadId"] = 2;
        master.handle_client_message(request("continue", 15, args));
        harness.drain();
        auto commands = harness.drain_commands();
        expect(commands.size() == 2, "continue reaches every stopped worker");
        bool saw2 = false;
        bool saw3 = false;
        for (const auto& entry : commands) {
            saw2 = saw2 || entry.worker == 2;
            saw3 = saw3 || entry.worker == 3;
        }
        expect(saw2 && saw3, "both stopped workers are resumed");
    }
    {
        Json args        = Json::object();
        args["threadId"] = 2;
        master.handle_client_message(request("next", 14, args));
        auto out = harness.drain();
        expect(harness.find(out, "response", "next") != nullptr, "next is answered");
        expect(harness.find(out, "event", "continued") != nullptr, "next reports continued");
        auto commands = harness.drain_commands();
        bool saw_step = false;
        size_t runs   = 0;
        for (const auto& entry : commands) {
            const std::string cmd = str(entry.command, "cmd");
            if (cmd == "stepOver") {
                saw_step = true;
                expect(entry.worker == 2, "the step goes to the requested thread");
            }
            if (cmd == "run") {
                ++runs;
                expect(entry.worker != 2, "run never goes to the stepping thread");
            }
        }
        expect(runs == 0, "a step leaves workers that are not stopped alone");
        expect(saw_step, "the step is issued");
    }

    // ---- 11. disconnect terminates a launch session
    master.handle_client_message(request("disconnect", 15, Json::object()));
    {
        auto out             = harness.drain();
        const Json* response = harness.find(out, "response", "disconnect");
        expect(response != nullptr && (*response)["success"].get<bool>(),
               "disconnect is answered");
        auto commands = harness.drain_commands();
        expect(!commands.empty() && str(commands[0].command, "cmd") == "disconnect",
               "workers are told to disconnect");
        expect(harness.finished && harness.terminate,
               "a launch session terminates the debuggee");
    }

    // ---- 12. an empty worker list keeps a keepSessionAlive session alive
    {
        Harness quiet;
        Master other = quiet.make();
        other.handle_client_message(request("initialize", 1, Json::object()));
        Json args                 = Json::object();
        args["request"]           = "attach";
        args["keepSessionAlive"]  = true;
        other.handle_client_message(request("attach", 2, args));
        other.worker_ready(1);
        other.worker_gone(1);
        expect(!quiet.finished, "keepSessionAlive keeps the session open");
        quiet.drain();
    }

    if (failures == 0) {
        std::printf("master_test: OK\n");
        return 0;
    }
    std::printf("master_test: %d failure(s)\n", failures);
    return 1;
}
