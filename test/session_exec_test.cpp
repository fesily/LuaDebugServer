// Verifies the execution-control slice of the native worker: hooks fire, a
// breakpoint stops the target, the stop loop services commands, and stepping
// produces the expected line sequence.

#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "common/base64.h"
#include "common/json.h"
#include "lua_compat.h"
#include "worker/worker_session.h"

namespace {

    using luadebug::debugger::Json;
    using luadebug::worker::WorkerSession;

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    struct Sink {
        std::vector<Json> messages;
    };

    void sink_fn(void* ctx, LdSlice message) {
        auto* sink = static_cast<Sink*>(ctx);
        Json parsed;
        std::string error;
        if (luadebug::debugger::parse_json(std::string(message.data, message.size), parsed, error)
            == luadebug::debugger::ParseStatus::ok) {
            sink->messages.push_back(std::move(parsed));
        }
    }

    std::vector<Json> stops(const Sink& sink) {
        std::vector<Json> result;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "cmd") == "eventStop") {
                result.push_back(message);
            }
        }
        return result;
    }

    const char* kLoopSource =
        "local function add(a, b)\n"       // 1
        "    LOOP = LOOP + 1\n"           // 2
        "    return a + b\n"              // 3
        "end\n"                           // 4
        "LOOP = 0\n"                      // 5
        "for i = 1, 3 do\n"               // 6
        "    add(i, 1)\n"                 // 7
        "end\n"                           // 8
        "RESULT = LOOP\n";                // 9

    const char* kSource =
        "local function add(a, b)\n"       // 1
        "    local sum = a + b\n"          // 2
        "    return sum\n"                 // 3
        "end\n"                            // 4
        "\n"                               // 5
        "local x = 1\n"                    // 6
        "local y = add(x, 2)\n"            // 7
        "RESULT = y\n"                     // 8
        "return y\n";

    struct Driver {
        WorkerSession* session;
        Sink* sink;
        // One scripted step per pump call; when empty the pump stops.
        std::deque<std::string> commands;
        std::deque<std::function<void()>> steps;

        bool operator()() {
            if (!steps.empty()) {
                auto step = steps.front();
                steps.pop_front();
                step();
                return true;
            }
            if (commands.empty()) {
                return false;
            }
            auto command = commands.front();
            commands.pop_front();
            session->dispatch(command);
            return true;
        }

        void dispatch(const std::string& json) {
            session->dispatch(json);
        }
    };

    std::string request_eval(int seq, int frame, const char* expression) {
        Json json = Json::object();
        json["cmd"]        = "evaluate";
        json["command"]    = "evaluate";
        json["seq"]        = seq;
        json["frameId"]    = frame;
        json["expression"] = expression;
        return luadebug::debugger::dump_json(json);
    }

    std::string command(const char* name) {
        Json request = Json::object();
        request["cmd"] = name;
        request["command"] = name;
        request["seq"] = 1;
        return luadebug::debugger::dump_json(request);
    }

    void set_bp_content(WorkerSession& session, const char* content, int line) {
        Json request = Json::object();
        request["cmd"] = "setBreakpoints";
        Json source = Json::object();
        source["path"] = "/virtual/session_exec.lua";
        request["source"] = std::move(source);
        request["content"] = content;
        Json breakpoints = Json::array();
        Json bp = Json::object();
        bp["id"] = 1;
        bp["line"] = line;
        breakpoints.push_back(std::move(bp));
        request["breakpoints"] = std::move(breakpoints);
        session.dispatch(luadebug::debugger::dump_json(request));
    }

    void set_bp(WorkerSession& session, int line) {
        Json request = Json::object();
        request["cmd"] = "setBreakpoints";
        Json source = Json::object();
        source["path"] = "/virtual/session_exec.lua";
        request["source"] = std::move(source);
        request["content"] = kSource;
        Json breakpoints = Json::array();
        Json bp = Json::object();
        bp["id"] = 1;
        bp["line"] = line;
        breakpoints.push_back(std::move(bp));
        request["breakpoints"] = std::move(breakpoints);
        session.dispatch(luadebug::debugger::dump_json(request));
    }

    // Loads and runs `source` with the given scripted commands.
    bool run_source(lua_State* L, const char* source, Driver& driver, std::string& error) {
        if (luaL_loadbuffer(L, source, std::strlen(source), "@session_exec.lua") != 0) {
            error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
            return false;
        }
        if (lua_pcall(L, 0, 1, 0) != 0) {
            error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "run error";
            return false;
        }
        lua_pop(L, 1);
        (void)driver;
        return true;
    }

    bool run_chunk(lua_State* L, Driver& driver, std::string& error) {
        return run_source(L, kSource, driver, error);
    }

} // namespace

int main() {
    // ---- 1. breakpoint stop, then continue to the end
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        expect(session.hooks_attached(), "hooks attached");

        Driver driver { &session, &sink, {} };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));

        set_bp(session, 2);
        expect(!session.breakpoints().empty() && session.breakpoints()[0].verified,
               "breakpoint on line 2 verified");

        driver.commands.push_back(command("continue"));
        std::string error;
        expect(run_chunk(L, driver, error), "chunk runs to completion");
        if (!error.empty()) {
            std::fprintf(stderr, "  target error: %s\n", error.c_str());
        }

        auto events = stops(sink);
        expect(events.size() == 1, "exactly one stop for one breakpoint");
        if (!events.empty()) {
            expect(luadebug::debugger::get_string(events[0], "reason") == "breakpoint",
                   "stop reason is breakpoint");
            expect(events[0].value("line", 0) == 2, "stopped on line 2");
            expect(events[0].contains("hitBreakpointIds"), "stop reports the breakpoint id");
        }
        expect(session.last_stop_line() == 2, "session remembers the stop line");
        expect(session.state() == luadebug::worker::State::running,
               "session is running again after continue");

        // The target really finished: RESULT is set.
        lua_getglobal(L, "RESULT");
        expect(lua_tointeger(L, -1) == 3, "target computed y = 3");
        lua_pop(L, 1);
        }  // session torn down before the state
        lua_close(L);
    }

    // ---- 1b. stackTrace while stopped reports the real frame
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();

        std::deque<std::string> commands;
        Driver driver { &session, &sink, {} };
        driver.commands = { R"({"cmd":"stackTrace","command":"stackTrace","seq":9})",
                            command("continue") };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        set_bp(session, 2);

        std::string error;
        expect(run_chunk(L, driver, error), "chunk with stackTrace runs");

        Json response;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "command") == "stackTrace") {
                response = message;
            }
        }
        expect(!response.is_null(), "a stackTrace response was emitted");
        if (!response.is_null()) {
            const auto& body = response["body"];
            expect(body.contains("stackFrames") && body["stackFrames"].is_array(),
                   "response carries stackFrames");
            const auto& frames = body["stackFrames"];
            expect(!frames.empty(), "at least one frame");
            if (!frames.empty()) {
                const auto& top = frames[0];
                expect(top.value("id", -1) == 0, "innermost frame id is 0");
                expect(top.value("name", std::string()) == "add", "frame name is the function");
                expect(top.value("line", 0) == 2, "frame line is the breakpoint line");
                expect(top.contains("source") && top["source"].value("name", std::string())
                                                       == "session_exec.lua",
                       "frame source name");
                expect(top.contains("instructionPointerReference"),
                   "frame exposes an instruction reference");
            if (top.contains("instructionPointerReference")) {
                const std::string ref(
                    luadebug::debugger::get_string(top, "instructionPointerReference"));
                expect(ref.rfind("bp_", 0) == 0, "instruction reference format");
                expect(ref.find("session_exec.lua") != std::string::npos,
                       "instruction reference names the source");
            }
            expect(frames.size() >= 2, "caller frame present");
                if (frames.size() >= 2) {
                    expect(frames[1].value("name", std::string()) == "(main)",
                           "caller is the main chunk");
                }
            }
        }
        }
        lua_close(L);
    }

    // ---- 1c. scopes / variables / evaluate / setVariable while stopped
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();

        Driver driver { &session, &sink, {}, {} };
        int local_ref = 0;

        auto request = [](const char* command, int seq, const char* extra) {
            Json json = Json::object();
            json["cmd"]     = command;
            json["command"] = command;
            json["seq"]     = seq;
            if (extra) {
                Json parsed;
                std::string error;
                luadebug::debugger::parse_json(extra, parsed, error);
                for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                    json[it.key()] = it.value();
                }
            }
            return luadebug::debugger::dump_json(json);
        };

        // The whole conversation happens while the target is stopped: each pump
        // call issues the next request.
        driver.steps.push_back([&]() {
            driver.dispatch(request("evaluate", 9,
                                    R"({"frameId":0,"expression":"sum + 1"})"));
        });
        driver.steps.push_back([&]() {
            driver.dispatch(request("scopes", 10, nullptr));
            for (const auto& message : sink.messages) {
                if (luadebug::debugger::get_string(message, "command") != "scopes") {
                    continue;
                }
                for (const auto& scope : message["body"]["scopes"]) {
                    if (luadebug::debugger::get_string(scope, "name") == "Local") {
                        local_ref = static_cast<int>(
                            luadebug::debugger::get_int(scope, "variablesReference", 0));
                    }
                }
            }
        });
        driver.steps.push_back([&]() {
            Json json = Json::object();
            json["cmd"] = "variables";
            json["command"] = "variables";
            json["seq"] = 11;
            json["variablesReference"] = local_ref;
            driver.dispatch(luadebug::debugger::dump_json(json));
        });
        driver.steps.push_back([&]() {
            driver.dispatch(request("evaluate", 12,
                                    R"({"frameId":0,"expression":"sum + 1"})"));
        });
        driver.steps.push_back([&]() {
            Json json = Json::object();
            json["cmd"] = "setVariable";
            json["command"] = "setVariable";
            json["seq"] = 13;
            json["variablesReference"] = local_ref;
            json["name"] = "sum";
            json["value"] = "42";
            driver.dispatch(luadebug::debugger::dump_json(json));
        });
        driver.steps.push_back([&]() { driver.dispatch(command("continue")); });

        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        // Line 3 (`return sum`): `sum` is in scope only after line 2 executed.
        set_bp(session, 3);

        std::string error;
        expect(run_chunk(L, driver, error), "chunk with variable requests runs");
        expect(local_ref != 0, "local scope reference allocated");

        bool saw_sum = false;
        bool set_ok   = false;
        bool eval_ok  = false;
        for (const auto& message : sink.messages) {
            const auto command = luadebug::debugger::get_string(message, "command");
            if (command == "variables") {
                for (const auto& variable : message["body"]["variables"]) {
                    if (luadebug::debugger::get_string(variable, "name") == "sum") {
                        saw_sum = true;
                        expect(luadebug::debugger::get_string(variable, "value") == "3",
                               "local sum renders as 3");
                    }
                }
            }
            if (command == "setVariable") {
                set_ok = message.value("success", false);
                if (!set_ok) {
                    std::fprintf(stderr, "  setVariable: %s\n",
                                 luadebug::debugger::get_string(message, "message").data());
                }
            }
            if (command == "evaluate") {
                eval_ok = message.value("success", false);
                if (eval_ok) {
                    expect(luadebug::debugger::get_string(message["body"], "result") == "4",
                           "evaluate computed sum + 1 before the write");
                } else {
                    std::fprintf(stderr, "  evaluate: %s\n",
                                 luadebug::debugger::get_string(message, "message").data());
                }
            }
        }
        expect(saw_sum, "the stopped frame exposes `sum`");
        expect(set_ok, "setVariable succeeded");
        expect(eval_ok, "evaluate succeeded");
        }
        lua_close(L);
    }

    // ---- 1d. hit conditions and logpoints
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {}, {} };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));

        // Breakpoint on line 7 hits twice (two calls to add); only the second
        // hit should stop because of the hit condition.
        Json request = Json::object();
        request["cmd"] = "setBreakpoints";
        Json source = Json::object();
        source["path"] = "/virtual/session_exec.lua";
        request["source"] = std::move(source);
        request["content"] = kLoopSource;
        Json bps = Json::array();
        Json bp = Json::object();
        bp["id"]          = 5;
        bp["line"]        = 2;
        bp["hitCondition"] = ">= 2";
        bps.push_back(std::move(bp));
        request["breakpoints"] = std::move(bps);
        session.dispatch(luadebug::debugger::dump_json(request));

        driver.commands.push_back(command("continue"));
        std::string error;
        expect(run_source(L, kLoopSource, driver, error), "hit-condition chunk runs");

        // Three hits, condition ">= 2": stops on the 2nd and 3rd.
        auto events = stops(sink);
        expect(events.size() == 2, "the hit condition stops from the second hit on");
        if (!events.empty()) {
            expect(luadebug::debugger::get_string(events[0], "reason") == "breakpoint",
                   "hit-condition stop is a breakpoint");
        }
        expect(session.breakpoints().size() == 1, "breakpoint stored");
        if (!session.breakpoints().empty()) {
            expect(session.breakpoints()[0].hit_condition == ">= 2", "hit condition stored");
        }
        }
        lua_close(L);
    }

    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {}, {} };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));

        Json request = Json::object();
        request["cmd"] = "setBreakpoints";
        Json source = Json::object();
        source["path"] = "/virtual/session_exec.lua";
        request["source"] = std::move(source);
        request["content"] = kLoopSource;
        Json bps = Json::array();
        Json bp = Json::object();
        bp["id"]         = 6;
        bp["line"]       = 2;
        bp["logMessage"] = "hit={LOOP}";
        bps.push_back(std::move(bp));
        request["breakpoints"] = std::move(bps);
        session.dispatch(luadebug::debugger::dump_json(request));

        std::string error;
        expect(run_source(L, kLoopSource, driver, error), "logpoint chunk runs to completion");

        expect(stops(sink).empty(), "a logpoint never stops");
        std::string output;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "cmd") == "eventOutput") {
                output += luadebug::debugger::get_string(message, "output");
            }
        }
        expect(output.find("hit=0") != std::string::npos,
               "logpoint interpolated the frame value");
        }
        lua_close(L);
    }

    // ---- 1e. exception filter stops with reason `exception`
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        const char* chunk =
            "local function boom()\n"      // 1
            "    error('kaboom')\n"        // 2
            "end\n"                        // 3
            "BOOM_OK = pcall(boom)\n"      // 4
            "RESULT = 1\n";                // 5

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {}, {} };
        driver.commands = {
            R"({"cmd":"exceptionInfo","command":"exceptionInfo","seq":20})",
            command("continue"),
        };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));

        Json filter = Json::object();
        filter["cmd"] = "setExceptionBreakpoints";
        Json arguments = Json::array();
        Json one = Json::object();
        one["id"]       = 77;
        one["filterId"] = "error";
        arguments.push_back(std::move(one));
        filter["arguments"] = std::move(arguments);
        session.dispatch(luadebug::debugger::dump_json(filter));

        std::string error;
        expect(run_source(L, chunk, driver, error), "exception chunk runs");

        auto events = stops(sink);
        expect(events.size() == 1, "the exception filter stops once");
        if (!events.empty()) {
            expect(luadebug::debugger::get_string(events[0], "reason") == "exception",
                   "stop reason is exception");
            expect(events[0].contains("hitBreakpointIds"), "exception stop carries the filter id");
        }
        bool info_ok = false;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "command") != "exceptionInfo") {
                continue;
            }
            info_ok = true;
            const std::string id(luadebug::debugger::get_string(message["body"], "exceptionId"));
            expect(id.find("kaboom") != std::string::npos,
                   "exceptionInfo carries the error message");
            const std::string trace(
                luadebug::debugger::get_string(message["body"]["details"], "stackTrace"));
            expect(trace.find("session_exec.lua") != std::string::npos,
                   "exceptionInfo carries a traceback");
        }
        expect(info_ok, "exceptionInfo answered");
        }
        lua_close(L);
    }

    // ---- 1f. loaded sources are announced and listed
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {}, {} };
        driver.commands = {
            R"({"cmd":"stackTrace","command":"stackTrace","seq":21})",
            R"({"cmd":"loadedSources","command":"loadedSources","seq":22})",
            command("continue"),
        };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        set_bp(session, 2);

        std::string error;
        expect(run_chunk(L, driver, error), "loadedSources chunk runs");

        bool saw_new = false;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "cmd") == "eventLoadedSource") {
                saw_new = true;
                expect(luadebug::debugger::get_string(message["source"], "name") == "session_exec.lua",
                       "loaded source name");
            }
        }
        expect(saw_new, "a loadedSource event was emitted");

        bool listed = false;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "command") != "loadedSources") {
                continue;
            }
            listed = true;
            const auto& sources = message["body"]["sources"];
            expect(sources.is_array() && !sources.empty(), "loadedSources lists the chunk");
            if (!sources.empty()) {
                expect(luadebug::debugger::get_int(sources[0], "sourceReference", 0) > 0,
                       "loaded source has a reference");
            }
        }
        expect(listed, "loadedSources answered");
        }
        lua_close(L);
    }

    // ---- 1g. setExpression writes through and reads back
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {}, {} };

        driver.steps.push_back([&]() {
            Json json = Json::object();
            json["cmd"]       = "setExpression";
            json["command"]   = "setExpression";
            json["seq"]       = 30;
            json["frameId"]   = 0;
            json["expression"] = "sum";
            json["value"]     = "99";
            driver.dispatch(luadebug::debugger::dump_json(json));
        });
        driver.steps.push_back([&]() {
            driver.dispatch(request_eval(31, 0, "sum"));
        });
        driver.steps.push_back([&]() { driver.dispatch(command("continue")); });
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        set_bp(session, 3);

        std::string error;
        expect(run_chunk(L, driver, error), "setExpression chunk runs");

        bool set_ok = false;
        for (const auto& message : sink.messages) {
            const auto command = luadebug::debugger::get_string(message, "command");
            if (command == "setExpression") {
                set_ok = message.value("success", false);
                if (set_ok) {
                    expect(luadebug::debugger::get_string(message["body"], "result") == "99",
                           "setExpression reports the new value");
                } else {
                    std::fprintf(stderr, "  setExpression: %s\n",
                                 luadebug::debugger::get_string(message, "message").data());
                }
            }
            if (command == "evaluate" && message.value("success", false)) {
                expect(luadebug::debugger::get_string(message["body"], "result") == "99",
                       "the write reached the binding");
            }
        }
        expect(set_ok, "setExpression succeeded");
        }
        lua_close(L);
    }

    // ---- 1h. variables expose memoryReference and readMemory reads bytes
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        const char* chunk =
            "local text = 'hello-memory'\n"   // 1
            "local n = 2\n"                   // 2
            "RESULT = text\n";                // 3

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {}, {} };
        int local_ref = 0;
        std::string memory_ref;

        driver.steps.push_back([&]() {
            driver.dispatch(R"({"cmd":"scopes","command":"scopes","seq":40,"frameId":0})");
            for (const auto& message : sink.messages) {
                if (luadebug::debugger::get_string(message, "command") != "scopes") {
                    continue;
                }
                for (const auto& scope : message["body"]["scopes"]) {
                    if (luadebug::debugger::get_string(scope, "name") == "Local") {
                        local_ref = static_cast<int>(
                            luadebug::debugger::get_int(scope, "variablesReference", 0));
                    }
                }
            }
        });
        driver.steps.push_back([&]() {
            Json json = Json::object();
            json["cmd"] = "variables";
            json["command"] = "variables";
            json["seq"] = 41;
            json["variablesReference"] = local_ref;
            driver.dispatch(luadebug::debugger::dump_json(json));
            for (const auto& message : sink.messages) {
                if (luadebug::debugger::get_string(message, "command") != "variables") {
                    continue;
                }
                for (const auto& variable : message["body"]["variables"]) {
                    if (luadebug::debugger::get_string(variable, "name") == "text") {
                        memory_ref = std::string(
                            luadebug::debugger::get_string(variable, "memoryReference"));
                    }
                }
            }
        });
        driver.steps.push_back([&]() {
            Json json = Json::object();
            json["cmd"] = "readMemory";
            json["command"] = "readMemory";
            json["seq"] = 42;
            json["memoryReference"] = memory_ref;
            json["offset"] = 0;
            json["count"] = 5;
            driver.dispatch(luadebug::debugger::dump_json(json));
        });
        driver.steps.push_back([&]() { driver.dispatch(command("continue")); });
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        set_bp_content(session, chunk, 3);

        std::string error;
        expect(run_source(L, chunk, driver, error), "memory chunk runs");
        expect(!memory_ref.empty(), "the string local exposes a memoryReference");

        bool read_ok = false;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "command") != "readMemory") {
                continue;
            }
            read_ok = message.value("success", false);
            if (read_ok) {
                std::string bytes;
                expect(luadebug::debugger::base64_decode(
                           luadebug::debugger::get_string(message["body"], "data"), bytes),
                       "readMemory returns valid base64");
                expect(bytes == "hello", "readMemory returned the first five bytes");
            } else {
                std::fprintf(stderr, "  readMemory: %s\n",
                             luadebug::debugger::get_string(message, "message").data());
            }
        }
        expect(read_ok, "readMemory answered");
        }
        lua_close(L);
    }

    // ---- 2. step over after the breakpoint
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();

        Driver driver { &session, &sink, { command("stepOver"), command("continue") } };
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        set_bp(session, 2);

        std::string error;
        expect(run_chunk(L, driver, error), "stepping chunk runs to completion");

        auto events = stops(sink);
        expect(events.size() == 2, "breakpoint stop then one step stop");
        if (events.size() == 2) {
            expect(luadebug::debugger::get_string(events[0], "reason") == "breakpoint",
                   "first stop is the breakpoint");
            expect(luadebug::debugger::get_string(events[1], "reason") == "step",
                   "second stop is a step");
            expect(events[1].value("line", 0) == 3, "step advanced to line 3");
        }
        }
        lua_close(L);
    }

    // ---- 3. a session with no breakpoints does not disturb execution
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();
        Driver driver { &session, &sink, {} };
        session.set_pump(std::ref(driver));

        std::string error;
        expect(run_chunk(L, driver, error), "chunk without breakpoints runs");
        expect(stops(sink).empty(), "no stops without breakpoints");
        lua_getglobal(L, "RESULT");
        expect(lua_tointeger(L, -1) == 3, "chunk still computed its result");
        lua_pop(L, 1);
        }
        lua_close(L);
    }

    // ---- 1d. disassemble + instruction breakpoints while stopped
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);
        session.attach_hooks();

        Driver driver { &session, &sink, {} };
        std::string instruction_ref;
        Json listing;
        Json instruction_result;
        driver.commands = { command("continue") };
        driver.steps.push_back([&] {
            // 1. The frame names its bytecode position.
            Json trace = Json::object();
            trace["cmd"]     = "stackTrace";
            trace["command"] = "stackTrace";
            trace["seq"]     = 20;
            session.dispatch(luadebug::debugger::dump_json(trace));
            for (const auto& message : sink.messages) {
                if (luadebug::debugger::get_string(message, "command") != "stackTrace") {
                    continue;
                }
                const auto& frames = message["body"]["stackFrames"];
                if (frames.is_array() && !frames.empty()
                    && frames[0].contains("instructionPointerReference")) {
                    instruction_ref =
                        luadebug::debugger::get_string(frames[0], "instructionPointerReference");
                }
            }
            expect(!instruction_ref.empty(), "the stop frame carries an instruction reference");

            // 2. The listing is read from that very position.
            Json dis = Json::object();
            dis["cmd"]               = "disassemble";
            dis["command"]           = "disassemble";
            dis["seq"]               = 21;
            dis["memoryReference"]   = instruction_ref;
            dis["instructionCount"]  = 3;
            session.dispatch(luadebug::debugger::dump_json(dis));
            for (const auto& message : sink.messages) {
                if (luadebug::debugger::get_string(message, "command") == "disassemble") {
                    listing = message;
                }
            }

            Json set = Json::object();
            set["cmd"]     = "setInstructionBreakpoints";
            set["command"] = "setInstructionBreakpoints";
            set["seq"]     = 22;
            Json breakpoints = Json::array();
            // Every instruction of `add(a, b)` (ADD / MMBIN / RETURN1): whatever
            // the VM skips, one of them has to run again — the ADD sits at the
            // frame's own position, the RETURN1 is where execution continues.
            if (!instruction_ref.empty()) {
                std::string rest = instruction_ref.substr(3);
                const std::size_t first  = rest.find('_');
                const std::size_t second = rest.find('_', first + 1);
                const std::size_t third  = rest.find('_', second + 1);
                const int base_pc = std::atoi(rest.substr(second + 1, third - second - 1).c_str());
                for (int i = 0; i < 3; ++i) {
                    Json bp = Json::object();
                    bp["id"]                   = 11 + i;
                    bp["instructionReference"] = "bp_" + rest.substr(0, second + 1)
                                               + std::to_string(base_pc + i) + rest.substr(third);
                    breakpoints.push_back(std::move(bp));
                }
            }
            set["breakpoints"] = std::move(breakpoints);
            session.dispatch(luadebug::debugger::dump_json(set));
            for (const auto& message : sink.messages) {
                if (luadebug::debugger::get_string(message, "command")
                    == "setInstructionBreakpoints") {
                    instruction_result = message;
                }
            }
        });
        session.set_pump(std::ref(driver));

        Json init = Json::object();
        init["cmd"] = "initializing";
        init["config"] = Json::object();
        session.dispatch(luadebug::debugger::dump_json(init));
        set_bp(session, 2);

        std::string error;
        expect(run_chunk(L, driver, error), "chunk with instruction breakpoints runs");

        expect(!listing.is_null(), "a disassemble response was emitted");
        if (!listing.is_null()) {
            expect(listing.value("success", false), "disassemble succeeds");
            const auto& instructions = listing["body"]["instructions"];
            expect(instructions.is_array() && instructions.size() == 3,
                   "the request returns the asked instruction count");
            expect(instructions.is_array() && !instructions.empty()
                       && luadebug::debugger::get_string(instructions[1], "instruction")
                                  .rfind("MMBIN", 0)
                              == 0,
                   "the listing keeps the metamethod instruction");
            if (instructions.is_array() && !instructions.empty()) {
                const auto& first = instructions[0];
                expect(luadebug::debugger::get_string(first, "address").rfind("0x", 0) == 0,
                       "instructions carry an address");
                expect(luadebug::debugger::get_string(first, "instructionBytes").size() == 8,
                       "instructions carry their bytes");
                expect(!luadebug::debugger::get_string(first, "instruction").empty(),
                       "instructions carry their text");
                expect(first.contains("line"), "instructions carry a source line");
                expect(luadebug::debugger::get_string(first, "instructionReference")
                           == instruction_ref,
                       "the first instruction is the referenced one");
            }
        }

        expect(!instruction_result.is_null(), "setInstructionBreakpoints answers");
        if (!instruction_result.is_null()) {
            expect(instruction_result.value("success", false), "the request succeeds");
            const auto& results = instruction_result["body"]["breakpoints"];
            expect(results.is_array() && !results.empty(), "one breakpoint is reported");
            if (results.is_array() && !results.empty()) {
                expect(results[0].value("verified", false),
                       "the instruction breakpoint verified");
            }
        }

        const auto events = stops(sink);
        expect(!events.empty(), "the instruction breakpoint stopped the target");
        int instruction_stops = 0;
        for (const auto& event : events) {
            if (luadebug::debugger::get_string(event, "reason") == "instruction breakpoint") {
                ++instruction_stops;
            }
        }
        expect(instruction_stops >= 1, "the stop reason names the instruction breakpoint");
        }
        lua_close(L);
    }

    // ---- 4. print / io.write capture (outputCapture)
    {
        lua_State* L = luaL_newstate();
        luadebug::vm::State* S = test_bind(L);
        luaL_openlibs(L);

        Sink sink;
        {
        WorkerSession session(S);
        session.set_sink(&sink_fn, &sink);

        Json init      = Json::object();
        init["cmd"]    = "initializing";
        Json config    = Json::object();
        Json capture   = Json::array();
        capture.push_back("print");
        capture.push_back("io.write");
        config["outputCapture"] = capture;
        init["config"]          = config;
        expect(session.dispatch(luadebug::debugger::dump_json(init)) == LD_STATUS_OK,
               "initializing installs the output capture");

        const char* chunk =
            "print('y=3')\n"
            "print(1, nil, true)\n"
            "io.write('a', 'b')\n";
        expect(luaL_loadbuffer(L, chunk, std::strlen(chunk), "@capture.lua") == 0
                   && lua_pcall(L, 0, 0, 0) == 0,
               "the chunk runs with the patches installed");

        std::string captured;
        bool categories_ok = true;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "cmd") != "eventOutput") {
                continue;
            }
            if (luadebug::debugger::get_string(message, "category") != "stdout") {
                categories_ok = false;
            }
            captured.append(luadebug::debugger::get_string(message, "output"));
        }
        expect(captured.find("y=3\n") != std::string::npos, "print output is captured");
        expect(captured.find("1\tnil\ttrue\n") != std::string::npos,
               "print renders tabs and every argument");
        // io.write joins its arguments with tabs and adds no newline.
        expect(captured.find("a\tb") != std::string::npos, "io.write output is captured");
        expect(categories_ok, "captured output is reported as stdout");

        // Turning capture off restores the original functions.
        Json off        = Json::object();
        off["cmd"]      = "initializing";
        Json empty      = Json::object();
        empty["outputCapture"] = Json::array();
        off["config"]          = empty;
        session.dispatch(luadebug::debugger::dump_json(off));
        const int before = static_cast<int>(sink.messages.size());
        const char* again = "print('raw')\n";
        expect(luaL_loadbuffer(L, again, std::strlen(again), "@capture2.lua") == 0
                   && lua_pcall(L, 0, 0, 0) == 0,
               "the chunk runs after capture is disabled");
        expect(static_cast<int>(sink.messages.size()) == before,
               "nothing is reported once the capture is off");
        }
        lua_close(L);
    }

    if (failures == 0) {
        std::printf("session_exec_test: OK\n");
        return 0;
    }
    std::printf("session_exec_test: %d failure(s)\n", failures);
    return 1;
}
