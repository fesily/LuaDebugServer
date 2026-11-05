// Verifies the bridge-side worker session: lifecycle, command dispatch and
// breakpoint verification driven by the target's own Protos.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
        auto body = std::string(message.data, message.size);
        if (luadebug::debugger::parse_json(body, parsed, error)
            == luadebug::debugger::ParseStatus::ok) {
            sink->messages.push_back(std::move(parsed));
        }
    }

    const char* kSource =
        "local function add(a, b)\n"
        "    local sum = a + b\n"
        "\n"
        "    return sum\n"
        "end\n"
        "\n"
        "local x = add(1, 2)\n"
        "print(x)\n";

    std::string set_bp_command(int id, int line, const char* condition = nullptr) {
        Json request = Json::object();
        request["cmd"] = "setBreakpoints";
        Json source = Json::object();
        source["path"] = "/nonexistent/placeholder.lua";
        request["source"] = std::move(source);
        request["content"] = kSource;
        Json breakpoints = Json::array();
        Json bp = Json::object();
        bp["id"]   = id;
        bp["line"] = line;
        if (condition) {
            bp["condition"] = condition;
        }
        breakpoints.push_back(std::move(bp));
        request["breakpoints"] = std::move(breakpoints);
        return luadebug::debugger::dump_json(request);
    }

    std::vector<Json> breakpoint_events(const Sink& sink) {
        std::vector<Json> events;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "cmd") == "eventBreakpoint") {
                events.push_back(message);
            }
        }
        return events;
    }

} // namespace

int main() {
    lua_State* L = luaL_newstate();
    luadebug::vm::State* S = test_bind(L);
    luaL_openlibs(L);

    Sink sink;
    WorkerSession session(S);
    session.set_sink(&sink_fn, &sink);

    // 1. Lifecycle: initializing config first, then breakpoints.
    {
        Json init = Json::object();
        init["cmd"]    = "initializing";
        init["config"] = Json::object();
        expect(session.dispatch(luadebug::debugger::dump_json(init)) == LD_STATUS_OK,
               "initializing is handled");
        expect(session.state() == luadebug::worker::State::initializing,
               "state is initializing after config replay");
        bool saw_initialized = false;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "cmd") == "eventInitialized") {
                saw_initialized = true;
            }
        }
        expect(saw_initialized, "initializing emits an initialized event");
    }

    // 2. A breakpoint on a blank line must move forward to the active line.
    {
        sink.messages.clear();
        expect(session.dispatch(set_bp_command(1, 3)) == LD_STATUS_OK, "setBreakpoints handled");
        auto events = breakpoint_events(sink);
        expect(events.size() == 1, "one breakpoint event");
        if (!events.empty()) {
            const auto& body = events[0]["breakpoint"];
            expect(body.value("verified", false), "blank line remaps to a verified line");
            expect(body.value("line", 0) == 4, "line 3 remaps to the active line 4");
            expect(body.value("id", 0) == 1, "the client's id is echoed");
        }
        expect(session.breakpoints().size() == 1, "the session keeps the breakpoint");
        if (!session.breakpoints().empty()) {
            expect(session.breakpoints()[0].real_line == 4, "stored real line");
        }
    }

    // 3. A line outside any function is rejected with a message.
    {
        sink.messages.clear();
        expect(session.dispatch(set_bp_command(2, 80)) == LD_STATUS_OK, "setBreakpoints handled");
        auto events = breakpoint_events(sink);
        expect(events.size() == 1, "one breakpoint event");
        if (!events.empty()) {
            const auto& body = events[0]["breakpoint"];
            expect(!body.value("verified", true), "out-of-range line is unverified");
            expect(body.value("message", std::string()).find("valid line") != std::string::npos,
                   "out-of-range line explains itself");
        }
    }

    // 4. A broken condition is reported as a condition error.
    {
        sink.messages.clear();
        expect(session.dispatch(set_bp_command(3, 4, "a ==")) == LD_STATUS_OK,
               "setBreakpoints with a bad condition is handled");
        auto events = breakpoint_events(sink);
        expect(events.size() == 1, "one breakpoint event");
        if (!events.empty()) {
            const auto& body = events[0]["breakpoint"];
            expect(!body.value("verified", true), "bad condition is unverified");
            expect(body.value("message", std::string()).find("Condition Error") != std::string::npos,
                   "message names the condition error");
        }
    }

    // 5. Unimplemented commands answer with an error, never a fake success.
    {
        sink.messages.clear();
        Json request = Json::object();
        request["cmd"]     = "stepBack";
        request["seq"]     = 7;
        request["command"] = "disassemble";
        expect(session.dispatch(luadebug::debugger::dump_json(request)) == LD_STATUS_FAILED,
               "unimplemented command reports a failure status");
        bool saw_error = false;
        for (const auto& message : sink.messages) {
            if (luadebug::debugger::get_string(message, "type") == "response"
                && !message.value("success", true)) {
                saw_error = true;
                expect(message.value("request_seq", 0) == 7, "error echoes the request seq");
            }
        }
        expect(saw_error, "unimplemented command emits an error response");
    }

    // 6. Invalid JSON is a hard error, not an empty request.
    {
        expect(session.dispatch("not json") == LD_STATUS_INVALID_ARGUMENT,
               "invalid command json is rejected");
    }

    // 7. Disconnect clears state and closes the session.
    {
        Json request = Json::object();
        request["cmd"] = "disconnect";
        expect(session.dispatch(luadebug::debugger::dump_json(request)) == LD_STATUS_OK,
               "disconnect is handled");
        expect(session.state() == luadebug::worker::State::closed, "state is closed");
        expect(session.breakpoints().empty(), "breakpoints dropped on disconnect");
    }

    lua_close(L);
    if (failures == 0) {
        std::printf("worker_session_test: OK\n");
        return 0;
    }
    std::printf("worker_session_test: %d failure(s)\n", failures);
    return 1;
}
