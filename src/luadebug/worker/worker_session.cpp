#include "worker/worker_session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "common/base64.h"
#include "common/log.h"
#include "hook/hook_core.h"
#include "target/compile.h"
#include "target/disasm.h"
#include "target/evaluator.h"
#include "target/proto_view.h"

namespace luadebug::worker {

    namespace {
        using namespace luadebug::debugger;

        // DAP carries memoryReference as a string, while the runtime forwards it
        // as a number; accept both.
        int int_field(const Json& value, std::string_view key, int fallback) {
            if (!value.is_object()) {
                return fallback;
            }
            auto it = value.find(key);
            if (it == value.end()) {
                return fallback;
            }
            json_int raw = 0;
            if (find_int(value, key, raw)) {
                return static_cast<int>(raw);
            }
            if (it->is_string()) {
                char* end              = nullptr;
                const std::string text = it->get<std::string>();
                const long parsed      = std::strtol(text.c_str(), &end, 10);
                if (end && end != text.c_str() && *end == '\0') {
                    return static_cast<int>(parsed);
                }
            }
            return fallback;
        }

        std::string read_file(const std::string& path, bool& ok) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                ok = false;
                return {};
            }
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            ok = true;
            return buffer.str();
        }

        // parser.lua's NormalizeErrorMessage(): strip the compiler's location
        // prefix so the client sees the condition error itself.
        std::string normalize_message(const char* what, const std::string& error) {
            std::string message = error;
            // "=(EVAL):3: rest" or ":3: (EVAL):3: rest" styles.
            auto pos = message.find(": ");
            while (pos != std::string::npos) {
                const std::string head = message.substr(0, pos);
                if (head.find("(EVAL)") != std::string::npos || head.find("=(EVAL)") != std::string::npos) {
                    message = message.substr(pos + 2);
                    break;
                }
                pos = message.find(": ", pos + 2);
            }
            return std::string(what) + ": " + message + ".";
        }

    }  // namespace

    const char* state_name(State state) {
        switch (state) {
        case State::initializing:
            return "initializing";
        case State::running:
            return "running";
        case State::stopped:
            return "stopped";
        case State::terminating:
            return "terminating";
        case State::closed:
            return "closed";
        }
        return "unknown";
    }

    namespace {
        // DAP value rendering, kept close to what variables.lua produced: a
        // quoted string, a type word, or a short summary for containers.
        const char* short_type(int type) {
            switch (type) {
            case vm::type::nil:
                return "nil";
            case vm::type::boolean:
                return "boolean";
            case vm::type::number:
                return "number";
            case vm::type::string:
                return "string";
            case vm::type::table:
                return "table";
            case vm::type::function:
                return "function";
            case vm::type::userdata:
                return "userdata";
            case vm::type::thread:
                return "thread";
            case vm::type::lightuserdata:
                return "lightuserdata";
            default:
                return "unknown";
            }
        }
    }  // namespace

    struct WorkerSession::HookState {
        std::unique_ptr<luadebug::hook::Core> core;
    };

    WorkerSession::WorkerSession(vm::State* target)
        : target_(target)
        , store_(new luadebug::target::ValueStore(target)) {
    }

    WorkerSession::~WorkerSession() {
        reset_references();
        delete store_;
    }

    void WorkerSession::reset_references() {
        refs_.clear();
        next_ref_ = 1;
        if (store_ && target_) {
            for (int ref : key_refs_) {
                target_->unref(ref);
            }
        }
        key_refs_.clear();
    }

    int WorkerSession::allocate_scope(RefKind kind, int depth) {
        ValueRef ref;
        ref.kind     = kind;
        ref.depth    = depth;
        const int id = next_ref_++;
        refs_.emplace(id, std::move(ref));
        return id;
    }

    int WorkerSession::allocate_value(luadebug::target::HandleId handle, std::string evaluate_name) {
        ValueRef ref;
        ref.kind          = RefKind::value;
        ref.handle        = handle;
        ref.evaluate_name = std::move(evaluate_name);
        const int id      = next_ref_++;
        refs_.emplace(id, std::move(ref));
        return id;
    }

    int WorkerSession::allocate_child(const ValueRef& parent, luadebug::refvalue::value member) {
        // The parent's path is recovered from the store by handle; ValueStore
        // owns the path, so a child is just "parent path + member".
        auto handle = parent.handle;
        auto child  = store_->extend_path(handle, std::move(member));
        return allocate_value(child, parent.evaluate_name);
    }

    bool WorkerSession::push_ref(const ValueRef& ref) {
        if (!store_ || ref.handle == 0) {
            return false;
        }
        std::string error;
        return store_->push(ref.handle, error) == luadebug::target::EvalStatus::ok;
    }

    // Values are read from the state that owns the stack at the stop: a stop
    // inside a coroutine runs on the coroutine, not on the main thread.
    std::string WorkerSession::format_value(int stack_index) {
        vm::State* L = frame_state();
        return format_value_on(L ? L : target_, stack_index);
    }

    std::string WorkerSession::format_value_on(vm::State* L, int stack_index) const {
        if (!L) {
            return {};
        }
        stack_index = L->absindex(stack_index);
        switch (L->type(stack_index)) {
        case vm::type::nil:
            return "nil";
        case vm::type::boolean:
            return L->toboolean(stack_index) ? "true" : "false";
        case vm::type::number: {
            // Versions without an integer subtype simply never report one, so
            // this needs no per-version branch.
            if (L->isinteger(stack_index)) {
                const long long value = static_cast<long long>(L->tointeger(stack_index));
                if (show_integer_as_hex_) {
                    char hex[32];
                    std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(value));
                    return hex;
                }
                return std::to_string(value);
            }
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.14g", static_cast<double>(L->tonumber(stack_index)));
            return buffer;
        }
        case vm::type::string: {
            size_t length    = 0;
            const char* text = L->tolstring(stack_index, &length);
            std::string value(text ? text : "", length);
            if (value.size() > 200) {
                value = value.substr(0, 200) + "...";
            }
            return "'" + value + "'";
        }
        case vm::type::table: {
            const size_t length = L->rawlen(stack_index);
            return "{...} (" + std::to_string(length) + " entries)";
        }
        default: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s: %p", short_type(L->type(stack_index)), L->topointer(stack_index));
            return buffer;
        }
        }
    }

    int WorkerSession::child_reference(const ValueRef& parent, int key_stack_index) {
        // Keep the key in the registry so the child path stays valid while the
        // value is on screen; released with the stop.
        target_->pushvalue(key_stack_index);
        const int key_ref = target_->ref();
        key_refs_.push_back(key_ref);
        return allocate_child(parent, luadebug::refvalue::TABLE_KEY { key_ref });
    }

    void WorkerSession::enumerate_container(const ValueRef& ref, int stack_index, Json& variables) {
        stack_index    = target_->absindex(stack_index);
        const int type = target_->type(stack_index);
        if (type != vm::type::table && type != vm::type::userdata) {
            return;
        }
        if (type == vm::type::table) {
            const int table = stack_index;
            target_->pushnil();
            while (target_->next(table) != 0) {
                const int value = target_->gettop();
                const int key   = value - 1;
                Json entry      = Json::object();
                std::string name;
                if (target_->type(key) == vm::type::string) {
                    name = target_->tostring(key);
                } else if (target_->type(key) == vm::type::number) {
                    name = format_value(key);
                } else {
                    name = "[" + format_value(key) + "]";
                }
                entry["name"]        = name;
                entry["value"]       = format_value(value);
                entry["type"]        = short_type(target_->type(value));
                const int value_type = target_->type(value);
                entry["variablesReference"] =
                    (value_type == vm::type::table || value_type == vm::type::userdata)
                        ? child_reference(ref, key)
                        : 0;
                if (value_type == vm::type::string || value_type == vm::type::userdata) {
                    target_->pushvalue(key);
                    const int key_ref = target_->ref();
                    key_refs_.push_back(key_ref);
                    auto member = store_->extend_path(
                        ref.handle, luadebug::refvalue::TABLE_KEY { key_ref }
                    );
                    if (member != 0) {
                        entry["memoryReference"] =
                            std::to_string(allocate_memory_ref(member, value_type == vm::type::string));
                    }
                }
                if (!ref.evaluate_name.empty()) {
                    entry["evaluateName"] = ref.evaluate_name + "." + name;
                }
                variables.push_back(std::move(entry));
                target_->pop(1);  // keep the key for lua_next
            }
        }
    }

    bool WorkerSession::hooks_attached() const {
        return hooks_ && hooks_->core && hooks_->core->valid();
    }

    void WorkerSession::attach_hooks() {
        if (hooks_attached() || !target_) {
            return;
        }
        hooks_.reset(new HookState());
        hooks_->core.reset(new luadebug::hook::Core(target_, this));
        // A coroutine has its own state and, from 5.2 on, its own hook mask, so a
        // hook installed on the main thread never fires inside it. The patched VM
        // reports resume/return, which is what lets the core install the same hook
        // on the coroutine before any of its lines run.
        hooks_->core->thread_open(target_, true);
    }

    void WorkerSession::set_pump(std::function<bool()> pump) {
        pump_ = std::move(pump);
    }

    void WorkerSession::set_idle_pump(std::function<void()> pump) {
        idle_pump_ = std::move(pump);
    }

    std::string WorkerSession::current_source(vm::State* L) const {
        vm::FrameInfo ar;
        if (!L || !L->getstack(0, &ar) || !L->getinfo("S", &ar)) {
            return {};
        }
        return ar.source && ar.source[0] == '@' ? std::string(ar.source + 1) : std::string(ar.source ? ar.source : "");
    }

    bool WorkerSession::wants_line_hooks(int start, int end, int& breakpoint_id) const {
        for (const auto& bp : breakpoints_) {
            if (!bp.verified) {
                continue;
            }
            if (bp.real_line >= start && bp.real_line <= end) {
                breakpoint_id = bp.id;
                return true;
            }
        }
        breakpoint_id = 0;
        return false;
    }

    namespace {
        std::string basename_of(const std::string& path) {
            auto pos = path.find_last_of("/\\");
            return pos == std::string::npos ? path : path.substr(pos + 1);
        }
    }  // namespace

    // Runs a self-contained predicate on a fresh thread: `return (count <cond>)`.
    static bool eval_count_predicate(vm::State* L, int count, const std::string& condition) {
        if (!L) {
            return false;
        }
        vm::State* co          = L->newthread();
        const int thread_slot  = L->gettop();
        const std::string text = "local count = " + std::to_string(count) + "\nreturn (count " + condition + ")\n";
        if (co->loadbuffer(text.data(), text.size(), "=(HITCOND)") != 0) {
            L->settop(thread_slot - 1);
            return false;
        }
        const int status = co->pcall(0, 1, 0);
        const bool ok    = status == 0 && co->toboolean(-1) != 0;
        L->settop(thread_slot - 1);
        return ok;
    }

    bool WorkerSession::hit_condition_met(const Breakpoint& bp) {
        return hit_condition_met(bp.id, bp.hit_condition);
    }

    bool WorkerSession::hit_condition_met(int id, const std::string& hit_condition) {
        if (hit_condition.empty()) {
            return true;
        }
        return eval_count_predicate(frame_state(), breakpoint_hits_[id], hit_condition);
    }

    bool WorkerSession::eval_condition(const std::string& expression) {
        if (expression.empty()) {
            return true;
        }
        luadebug::target::Evaluator evaluator(frame_state(), stop_base_level_);
        auto outcome  = evaluator.evaluate(expression);
        const bool ok = outcome.status == luadebug::target::CallStatus::ok && outcome.nresults > 0 && target_->toboolean(-1) != 0;
        if (outcome.nresults > 0) {
            target_->settop(target_->gettop() - 1);
        }
        return ok;
    }

    static int find_lua_base_level(vm::State* L);

    std::string WorkerSession::expand_log_message(const Breakpoint& bp) {
        return expand_log_message(bp.log_message);
    }

    std::string WorkerSession::expand_log_message(const std::string& log_message) {
        return expand_log_message_at(frame_state(), log_message);
    }

    std::string WorkerSession::expand_log_message_at(vm::State* L, const std::string& log_message) {
        std::string message;
        if (!L) {
            return log_message + "\n";
        }
        const std::string& text = log_message;
        std::size_t pos         = 0;
        while (pos < text.size()) {
            const std::size_t open = text.find('{', pos);
            if (open == std::string::npos) {
                message.append(text, pos, std::string::npos);
                break;
            }
            const std::size_t close = text.find('}', open + 1);
            if (close == std::string::npos) {
                message.append(text, pos, std::string::npos);
                break;
            }
            message.append(text, pos, open - pos);
            const std::string expression = text.substr(open + 1, close - open - 1);
            // Logpoints expand against the line they fire on, not against the
            // previous stop (the old worker's evaluate did the same).
            luadebug::target::Evaluator evaluator(L, find_lua_base_level(L));
            auto outcome = evaluator.evaluate(expression);
            if (outcome.status == luadebug::target::CallStatus::ok && outcome.nresults > 0) {
                message += format_value_on(L, L->gettop());
                L->settop(L->gettop() - 1);
            } else {
                message += "{" + expression + "}";
            }
            pos = close + 1;
        }
        return message + "\n";
    }

    bool WorkerSession::eval_condition_at(vm::State* L, const std::string& expression) {
        if (expression.empty()) {
            return true;
        }
        if (!L) {
            return false;
        }
        luadebug::target::Evaluator evaluator(L, find_lua_base_level(L));
        auto outcome  = evaluator.evaluate(expression);
        const bool ok = outcome.status == luadebug::target::CallStatus::ok && outcome.nresults > 0 && L->toboolean(-1) != 0;
        if (outcome.nresults > 0) {
            L->settop(L->gettop() - 1);
        }
        return ok;
    }

    int WorkerSession::breakpoint_at_line(vm::State* L, const std::string& source, int line, std::string& log_output) {
        log_output.clear();
        const std::string source_name = basename_of(source);
        for (auto& bp : breakpoints_) {
            if (!bp.verified || bp.real_line != line) {
                continue;
            }
            // The chunk name the target reports is not always the client's path
            // (relative paths, @ prefixes), so compare basenames and treat an
            // empty stored path as "whatever chunk the client sent".
            if (!(bp.source_path.empty() || source_name.empty() || basename_of(bp.source_path) == source_name)) {
                continue;
            }
            breakpoint_hits_[bp.id] += 1;
            if (!hit_condition_met(bp)) {
                continue;
            }
            // Conditional breakpoints are evaluated in the context of the line
            // they stop on (worker.lua's m.exec): a false condition keeps running.
            if (!eval_condition_at(L, bp.condition)) {
                continue;
            }
            if (!bp.log_message.empty()) {
                // Logpoints emit output and never stop.
                log_output = expand_log_message_at(L, bp.log_message);
                return 0;
            }
            return bp.id;
        }
        return 0;
    }

    bool WorkerSession::new_proto(vm::State* L, void* proto, int level) {
        // The old worker asked the target for the function's line range; do the
        // same here, then decide from our own breakpoint table.
        vm::FrameInfo ar;
        if (!L->getstack(level, &ar) || !L->getinfo("S", &ar)) {
            return false;
        }
        note_proto(proto);
        // Lua leaves a main chunk's lastlinedefined at 0; treat it as "up to the
        // end of the file" so breakpoints in the chunk body arm its line hook.
        const int start   = ar.linedefined == 0 ? 1 : ar.linedefined;
        const int end     = ar.lastlinedefined;
        int id            = 0;
        const bool wanted = end == 0 ? wants_line_hooks(start, 0x7FFFFFFF, id)
                                     : wants_line_hooks(start, end, id);
        if (hooks_ && hooks_->core) {
            // Break marks the proto as "decision made"; only armed protos get
            // line hooks. Non-armed ones are marked ignore so the call hook does
            // not re-ask on every call.
            if (wanted) {
                hooks_->core->break_add(L, proto);
            } else {
                hooks_->core->break_del(L, proto);
            }
        }
        return wanted;
    }

    void WorkerSession::function_break(vm::State* L) {
        if (function_breakpoints_.empty() || !L) {
            return;
        }
        // A call hook fires with the new frame already installed: level 0 is the
        // function being called, and `getinfo("n")` names it from the call site.
        vm::FrameInfo ar;
        if (!L->getstack(0, &ar) || !L->getinfo("n", &ar)) {
            return;
        }
        const std::string name = ar.name ? ar.name : "";
        for (const auto& bp : function_breakpoints_) {
            if (bp.name == name) {
                enter_stop("function breakpoint", 0, bp.id, L);
                return;
            }
        }
    }

    void WorkerSession::instruction_break(vm::State* L, void* proto) {
        const auto found = inst_breakpoints_.find(proto);
        if (found == inst_breakpoints_.end() || !L) {
            return;
        }
        const int pc   = current_pc(L);
        const auto hit = found->second.find(pc);
        if (hit == found->second.end()) {
            return;
        }
        InstructionBreakpoint& bp = hit->second;
        breakpoint_hits_[bp.id] += 1;
        if (!hit_condition_met(bp.id, bp.hit_condition)) {
            return;
        }
        if (!eval_condition(bp.condition)) {
            return;
        }
        std::string log_output;
        if (!bp.log_message.empty()) {
            // Logpoints emit output and never stop.
            Json event        = Json::object();
            event["cmd"]      = "eventOutput";
            event["category"] = "stdout";
            event["output"]   = expand_log_message(bp.log_message);
            emit(event);
            return;
        }
        enter_stop("instruction breakpoint", 0, bp.id, L);
    }

    void WorkerSession::exception(vm::State* L, int errcode) {
        if (exception_filters_.empty() || !L) {
            return;
        }
        const std::string type   = exception_type(L, errcode);
        const std::string caught = exception_caught(L, errcode);

        const ExceptionFilter* matched = nullptr;
        for (const auto& entry : exception_filters_) {
            if (entry.first != type && entry.first != caught) {
                continue;
            }
            if (exception_filter_matches(L, type, caught, entry.second)) {
                matched = &entry.second;
                break;
            }
        }
        if (!matched) {
            return;
        }

        exception_message_ = format_value(-1);
        exception_trace_   = exception_trace(L, 1);
        enter_stop("exception", 0, matched->id, L);
    }

    void WorkerSession::emit_stop(const std::string& reason, int line, int breakpoint_id) {
        Json event      = Json::object();
        event["cmd"]    = "eventStop";
        event["reason"] = reason;
        if (line > 0) {
            event["line"] = line;
        }
        if (breakpoint_id != 0) {
            Json ids = Json::array();
            ids.push_back(breakpoint_id);
            event["hitBreakpointIds"] = std::move(ids);
        }
        emit(event);
    }

    // The hook runs as a C frame above the stopped Lua code, so the innermost
    // Lua frame is not level 0. Find it once per stop and index from there.
    static int find_lua_base_level(vm::State* L) {
        for (int level = 0; level < 16; ++level) {
            vm::FrameInfo ar;
            if (!L->getstack(level, &ar)) {
                return 0;
            }
            if (!L->getinfo("S", &ar)) {
                return 0;
            }
            if (ar.what && std::strcmp(ar.what, "C") != 0) {
                return level;
            }
        }
        return 0;
    }

    void WorkerSession::enter_stop(const std::string& reason, int line, int breakpoint_id, vm::State* hL) {
        if (stop_depth_ != 0) {
            // Already servicing a stop (a command triggered another hook).
            return;
        }
        ++stop_depth_;
        stopped_          = true;
        resume_requested_ = false;
        pending_step_     = luadebug::hook::StepMode::none;
        state_            = State::stopped;
        last_stop_line_   = line;
        last_stop_reason_ = reason;
        // Frames are indexed from the innermost Lua frame of the stop, and the
        // hook's thread owns them (a coroutine, when the target uses one).
        stop_thread_     = hL ? hL : target_;
        stop_base_level_ = stop_thread_ ? find_lua_base_level(stop_thread_) : 0;
        if (store_) {
            store_->begin_stop();
        }
        reset_references();

        emit_stop(reason, line, breakpoint_id);

        while (!resume_requested_ && pump_) {
            if (!pump_()) {
                break;
            }
        }

        if (pending_step_ != luadebug::hook::StepMode::none && hooks_ && hooks_->core) {
            hooks_->core->step(pending_step_);
        } else if (hooks_ && hooks_->core) {
            hooks_->core->step(luadebug::hook::StepMode::none);
        }
        pending_step_ = luadebug::hook::StepMode::none;
        stopped_      = false;
        --stop_depth_;
        if (state_ == State::stopped) {
            state_ = State::running;
        }
    }

    void WorkerSession::resume(luadebug::hook::StepMode mode) {
        pending_step_     = mode;
        resume_requested_ = true;
    }

    void WorkerSession::line_stop(vm::State* L, bool stepping, int line, void* proto) {
        // The line mask is state-wide (hook::Core::break_open), so line events for
        // every prototype arrive here. A prototype already classified as holding
        // no breakpoint can neither stop nor emit a logpoint, so skip the source
        // lookup and the table scan for it. The classification is made from the
        // frame its call hook reports; a call that never reaches the hook (a
        // LuaJIT trace, a C function) leaves the prototype unclassified, and an
        // unclassified prototype always takes the full path.
        if (!stepping && proto && hooks_ && hooks_->core && !hooks_->core->proto_wants_line_events(proto)) {
            return;
        }
        const std::string source = current_source(L);
        std::string log_output;
        const int breakpoint_id = breakpoint_at_line(L, source, line, log_output);
        if (!log_output.empty()) {
            Json event        = Json::object();
            event["cmd"]      = "eventOutput";
            event["category"] = "stdout";
            event["output"]   = log_output;
            if (!source.empty()) {
                Json src        = Json::object();
                src["name"]     = basename_of(source);
                src["path"]     = source;
                event["source"] = std::move(src);
            }
            event["line"] = line;
            emit(event);
            return;
        }
        if (breakpoint_id != 0) {
            enter_stop("breakpoint", line, breakpoint_id, L);
            return;
        }
        if (stepping) {
            const std::string reason =
                requested_stop_reason_.empty() ? "step" : requested_stop_reason_;
            requested_stop_reason_.clear();
            enter_stop(reason, line, 0, L);
        }
    }

    namespace {
        // `print`/`io.write` argument rendering: strings are raw (this is console
        // output, not a value inspector), everything else uses the display form.
        std::string output_argument(vm::State* L, int index) {
            if (L->type(index) == vm::type::string) {
                size_t size      = 0;
                const char* text = L->tolstring(index, &size);
                return std::string(text ? text : "", size);
            }
            switch (L->type(index)) {
            case vm::type::nil:
                return "nil";
            case vm::type::boolean:
                return L->toboolean(index) ? "true" : "false";
            case vm::type::number: {
                if (L->isinteger(index)) {
                    return std::to_string(static_cast<long long>(L->tointeger(index)));
                }
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%.14g", static_cast<double>(L->tonumber(index)));
                return buffer;
            }
            default: {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%s: %p", L->typename_(L->type(index)), L->topointer(index));
                return buffer;
            }
            }
        }
    }  // namespace

    void WorkerSession::apply_output_capture(const Json& config) {
        if (!target_) {
            return;
        }
        auto wanted = [&config](const char* name) {
            if (!config.is_object() || !config.contains("outputCapture") || !config["outputCapture"].is_array()) {
                return false;
            }
            for (const auto& item : config["outputCapture"]) {
                if (item.is_string() && item.get<std::string>() == name) {
                    return true;
                }
            }
            return false;
        };

        const bool want_print   = wanted("print");
        const bool want_iowrite = wanted("io.write");
        if (want_print != capture_print_) {
            capture_print_ = want_print;
            luadebug::stdio::open_print(target_, want_print);
        }
        if (want_iowrite != capture_iowrite_) {
            capture_iowrite_ = want_iowrite;
            luadebug::stdio::open_iowrite(target_, want_iowrite);
        }
        // Only one session can own the process streams.
        luadebug::stdio::set_output_sink(target_->vm(), (want_print || want_iowrite) ? this : nullptr);

        if (wanted("stdout") && !stdout_redirect_) {
            stdout_redirect_ = std::make_unique<luadebug::stdio::std_redirect>();
            if (!stdout_redirect_->open(luadebug::stdio::std_fd::STDOUT)) {
                stdout_redirect_.reset();
            }
        } else if (!wanted("stdout")) {
            if (stdout_redirect_) {
                stdout_redirect_->close();
                stdout_redirect_.reset();
            }
        }
        if (wanted("stderr") && !stderr_redirect_) {
            stderr_redirect_ = std::make_unique<luadebug::stdio::std_redirect>();
            if (!stderr_redirect_->open(luadebug::stdio::std_fd::STDERR)) {
                stderr_redirect_.reset();
            }
        } else if (!wanted("stderr")) {
            if (stderr_redirect_) {
                stderr_redirect_->close();
                stderr_redirect_.reset();
            }
        }
    }

    void WorkerSession::drain_redirects() {
        const auto drain = [this](luadebug::stdio::std_redirect* redirect, const char* category) {
            if (!redirect) {
                return;
            }
            char buffer[4096];
            for (;;) {
                const size_t size = redirect->read(buffer, sizeof(buffer));
                if (size == 0) {
                    break;
                }
                Json event        = Json::object();
                event["cmd"]      = "eventOutput";
                event["category"] = category;
                event["output"]   = std::string(buffer, size);
                emit(event);
            }
        };
        drain(stdout_redirect_.get(), "stdout");
        drain(stderr_redirect_.get(), "stderr");
    }

    bool WorkerSession::output(vm::State* hL, const char* name, int first) {
        if (!hL) {
            return false;
        }
        std::string text;
        const int top = hL->gettop();
        for (int index = first; index <= top; ++index) {
            if (index > first) {
                text.push_back('\t');
            }
            text += output_argument(hL, index);
        }
        // `print` terminates the line; `io.write` does not.
        if (std::string(name) == "print") {
            text.push_back('\n');
        }
        Json event        = Json::object();
        event["cmd"]      = "eventOutput";
        event["category"] = "stdout";
        event["output"]   = text;
        emit(event);
        return true;
    }

    void WorkerSession::update(vm::State* /*L*/) {
        // Hook-tick drain: while the target runs this is the only place commands
        // can be applied, so a client that sets breakpoints mid-run does not have
        // to wait for a stop that may never come.
        drain_redirects();
        if (idle_pump_) {
            idle_pump_();
        }
    }

    void WorkerSession::emit(const Json& message) {
        if (!sink_) {
            return;
        }
        const std::string payload = dump_json(message);
        sink_(sink_ctx_, LdSlice { payload.data(), payload.size() });
    }

    void WorkerSession::emit_breakpoint_changed(const Breakpoint& bp) {
        Json change      = Json::object();
        change["cmd"]    = "eventBreakpoint";
        change["reason"] = "changed";
        Json body        = Json::object();
        body["id"]       = bp.id;
        body["verified"] = bp.verified;
        if (bp.real_line > 0) {
            body["line"] = bp.real_line;
        }
        if (!bp.message.empty()) {
            body["message"] = bp.message;
        }
        change["breakpoint"] = std::move(body);
        emit(change);
    }

    void WorkerSession::emit_response(const Json& request, Json body) {
        // Replay commands (config cache) carry no client seq: the master already
        // answered that client request, so answering again would duplicate it.
        if (!request.is_object() || !request.contains("seq")) {
            return;
        }
        Json response           = Json::object();
        response["type"]        = "response";
        response["success"]     = true;
        response["command"]     = get_string(request, "command");
        response["request_seq"] = get_int(request, "seq", 0);
        response["body"]        = std::move(body);
        emit(response);
    }

    // Mirrors worker.lua's getFuncName(): the label a client shows for a frame.
    std::string WorkerSession::frame_name(vm::State* L, int depth, const vm::FrameInfo& ar) const {
        (void)L;
        (void)depth;
        const std::string what = ar.what ? ar.what : "";
        if (what == "main") {
            return "(main)";
        }
        const std::string namewhat = ar.namewhat ? ar.namewhat : "";
        const std::string name     = ar.name ? ar.name : "";
        if (namewhat.empty()) {
            if (what == "C") {
                return "(C)";
            }
            return what == "Lua" ? "(anonymous function)" : ("(" + what + " ?)");
        }
        if (namewhat == "for iterator") {
            return "(for iterator)";
        }
        if (namewhat == "hook") {
            return "(hook)";
        }
        if (namewhat == "metamethod") {
            return "(metamethod " + name + ")";
        }
        if (namewhat == "field" && name == "integer index") {
            return "(field ?)";
        }
        if (name == "?") {
            return "(" + namewhat + " ?)";
        }
        return name;
    }

    void WorkerSession::stack_trace(const Json& request) {
        Json frames = Json::array();
        int total   = 0;
        if (target_) {
            const int start  = static_cast<int>(get_int(request, "startFrame", 0));
            const int levels = static_cast<int>(get_int(request, "levels", 200));
            const int limit  = levels > 0 ? levels : 200;
            for (int depth = start; depth < start + limit; ++depth) {
                vm::FrameInfo ar;
                if (depth < 8) {
                }
                if (!frame_state()->getstack(stop_base_level_ + depth, &ar)) {
                    total = depth;
                    break;
                }
                if (depth < 8) {
                }
                if (!frame_state()->getinfo("Sln", &ar)) {
                    break;
                }
                if (depth < 8) {
                }
                Json frame             = Json::object();
                frame["id"]            = depth;
                frame["name"]          = frame_name(target_, depth, ar);
                frame["line"]          = 0;
                frame["column"]        = 0;
                const std::string what = ar.what ? ar.what : "";
                if (what != "C") {
                    frame["column"] = 1;
                    frame["line"]   = ar.currentline;
                    if (ar.source) {
                        const std::string source = ar.source;
                        if (!source.empty() && source[0] == '@') {
                            const std::string path = source.substr(1);
                            Json src               = Json::object();
                            auto pos               = path.find_last_of("/\\");
                            src["name"]            = pos == std::string::npos ? path : path.substr(pos + 1);
                            src["path"]            = path;
                            note_source(frame_state(), path);
                            frame["source"] = std::move(src);
                        } else if (!source.empty()) {
                            Json src        = Json::object();
                            src["name"]     = source;
                            frame["source"] = std::move(src);
                        }
                    }
                    frame["presentationHint"] = "normal";
                    // Instruction references let the client disassemble and set
                    // instruction breakpoints; they need the prototype identity
                    // plus the frame's pc.
                    {
                        const int level = stop_base_level_ + depth;
                        const int pc    = luadebug::target::frame_pc(frame_state(), level);
                        if (pc >= 0) {
                            vm::FrameInfo fn;
                            if (frame_state()->getstack(level, &fn) && frame_state()->getinfo("f", &fn)) {
                                // getinfo("f") pushed the function.
                                const int func_index = frame_state()->gettop();
                                const std::string ref =
                                    luadebug::target::instruction_reference(frame_state(), func_index, pc);
                                if (!ref.empty()) {
                                    frame["instructionPointerReference"] = ref;
                                }
                                frame_state()->pop(1);
                            }
                        }
                    }
                } else {
                    frame["presentationHint"] = "label";
                }
                frames.push_back(std::move(frame));
                total = depth + 1;
            }
        }

        // Trailing C frames are noise for the client (the old worker's
        // skipCFunction): a stop inside a C function shows the Lua frames below
        // it, and a tail of "(C)" labels would break frame-count assumptions.
        while (!frames.empty()) {
            const auto& last = frames.back();
            if (std::string(get_string(last, "presentationHint")) == "label") {
                frames.erase(frames.size() - 1);
                continue;
            }
            break;
        }

        Json body           = Json::object();
        body["totalFrames"] = total;
        body["stackFrames"] = std::move(frames);
        emit_response(request, std::move(body));
    }

    void WorkerSession::threads(const Json& request) {
        Json threads  = Json::array();
        Json entry    = Json::object();
        entry["id"]   = 0;
        entry["name"] = "Thread (1)";
        threads.push_back(std::move(entry));
        Json body       = Json::object();
        body["threads"] = std::move(threads);
        emit_response(request, std::move(body));
    }

    // --- instruction breakpoints -------------------------------------------

    void WorkerSession::note_proto(void* proto) {
        if (!proto || proto_keys_.count(proto)) {
            return;
        }
        int linedefined     = 0;
        int lastlinedefined = 0;
        std::string source;
        if (!luadebug::target::proto_identity_with(target_, proto, linedefined, lastlinedefined, source)) {
            return;
        }
        ProtoKey key;
        key.linedefined     = linedefined;
        key.lastlinedefined = lastlinedefined;
        key.source          = source;
        proto_keys_.emplace(proto, key);
        proto_ranges_[{ linedefined, lastlinedefined }].emplace_back(source, proto);

        // A pending breakpoint verifies as soon as its function shows up.
        auto pending = pending_inst_breakpoints_.find({ linedefined, lastlinedefined });
        if (pending == pending_inst_breakpoints_.end()) {
            return;
        }
        auto& waiting = pending->second;
        for (auto it = waiting.begin(); it != waiting.end();) {
            const bool same_source =
                it->reference.size() >= key.source.size() && it->reference.compare(it->reference.size() - key.source.size(), key.source.size(), key.source) == 0;
            if (!same_source) {
                ++it;
                continue;
            }
            InstructionBreakpoint bp        = *it;
            bp.condition                    = it->condition;
            inst_breakpoints_[proto][bp.pc] = bp;
            if (hooks_ && hooks_->core && target_) {
                hooks_->core->instbreak_open(target_, true);
            }
            emit_instruction_breakpoint(bp, true, std::string());
            it = waiting.erase(it);
        }
        if (waiting.empty()) {
            pending_inst_breakpoints_.erase(pending);
        }
    }

    void* WorkerSession::find_proto(const ProtoKey& key) const {
        auto found = proto_ranges_.find({ key.linedefined, key.lastlinedefined });
        if (found == proto_ranges_.end()) {
            return nullptr;
        }
        // Several functions can share a line range; the source picks the one the
        // client actually references.
        for (const auto& entry : found->second) {
            if (entry.first == key.source) {
                return entry.second;
            }
        }
        for (const auto& entry : found->second) {
            if (entry.first.size() >= key.source.size() && entry.first.compare(entry.first.size() - key.source.size(), key.source.size(), key.source) == 0) {
                return entry.second;
            }
        }
        return found->second.empty() ? nullptr : found->second.front().second;
    }

    int WorkerSession::current_pc(vm::State* L) const {
        return luadebug::target::frame_pc(L, 0);
    }

    void WorkerSession::emit_instruction_breakpoint(const InstructionBreakpoint& bp, bool verified, const std::string& message) {
        Json change      = Json::object();
        change["cmd"]    = "eventBreakpoint";
        change["reason"] = "changed";
        Json body        = Json::object();
        body["id"]       = bp.id;
        body["verified"] = verified;
        if (!message.empty()) {
            body["message"] = message;
        }
        change["breakpoint"] = std::move(body);
        emit(change);
    }

    namespace {
        // Instruction references are "bp_<ld>_<lld>_<pc>_<source>", optionally
        // prefixed with "inst_<threadId>x" by the runtime.
        bool parse_instruction_reference(const std::string& text, int& linedefined, int& lastlinedefined, int& pc, std::string& source) {
            std::string rest = text;
            if (rest.compare(0, 5, "inst_") == 0) {
                const std::size_t x = rest.find('x');
                if (x != std::string::npos) {
                    rest = rest.substr(x + 1);
                }
            }
            if (rest.compare(0, 3, "bp_") != 0) {
                return false;
            }
            rest              = rest.substr(3);
            std::size_t pos   = 0;
            const auto number = [&rest, &pos](int& out) {
                const std::size_t end = rest.find('_', pos);
                if (end == std::string::npos) {
                    return false;
                }
                out = std::atoi(rest.substr(pos, end - pos).c_str());
                pos = end + 1;
                return true;
            };
            if (!number(linedefined) || !number(lastlinedefined) || !number(pc)) {
                return false;
            }
            source = rest.substr(pos);
            return !source.empty();
        }
    }  // namespace

    void WorkerSession::set_instruction_breakpoints(const Json& request) {
        inst_breakpoints_.clear();
        pending_inst_breakpoints_.clear();
        int next_id  = 1;
        Json results = Json::array();
        if (request["breakpoints"].is_array()) {
            for (const auto& item : request["breakpoints"]) {
                InstructionBreakpoint bp;
                bp.id            = static_cast<int>(get_int(item, "id", next_id));
                next_id          = bp.id + 1;
                bp.reference     = std::string(get_string(item, "instructionReference"));
                bp.condition     = std::string(get_string(item, "condition"));
                bp.hit_condition = std::string(get_string(item, "hitCondition"));
                bp.log_message   = std::string(get_string(item, "logMessage"));

                ProtoKey key;
                bool ok = parse_instruction_reference(bp.reference, key.linedefined, key.lastlinedefined, bp.pc, key.source);
                if (!ok) {
                    Json entry        = Json::object();
                    entry["id"]       = bp.id;
                    entry["verified"] = false;
                    entry["message"]  = "Invalid instruction reference";
                    results.push_back(std::move(entry));
                    continue;
                }
                bp.pc += static_cast<int>(get_int(item, "offset", 0));

                void* proto = find_proto(key);
                if (!proto) {
                    pending_inst_breakpoints_[{ key.linedefined, key.lastlinedefined }].push_back(bp);
                    Json entry        = Json::object();
                    entry["id"]       = bp.id;
                    entry["verified"] = false;
                    entry["message"]  = "Function not yet loaded";
                    results.push_back(std::move(entry));
                    continue;
                }
                inst_breakpoints_[proto][bp.pc] = bp;
                Json entry                      = Json::object();
                entry["id"]                     = bp.id;
                entry["verified"]               = true;
                results.push_back(std::move(entry));
            }
        }
        if (hooks_ && hooks_->core && target_) {
            hooks_->core->instbreak_open(target_, !inst_breakpoints_.empty());
        }

        Json body           = Json::object();
        body["breakpoints"] = std::move(results);
        emit_response(request, std::move(body));
    }

    void WorkerSession::disassemble(const Json& request) {
        // The master strips the thread prefix and forwards the local part as
        // `refId` (the old worker protocol); tests use the raw field.
        const std::string reference { get_string(request, "refId") };
        const std::string raw { reference.empty() ? get_string(request, "memoryReference")
                                                  : std::string_view(reference) };
        int linedefined     = 0;
        int lastlinedefined = 0;
        int default_pc      = 0;
        std::string source;
        if (!parse_instruction_reference(raw, linedefined, lastlinedefined, default_pc, source)) {
            emit_error(request, "Invalid instruction reference");
            return;
        }
        ProtoKey key;
        key.linedefined     = linedefined;
        key.lastlinedefined = lastlinedefined;
        key.source          = source;
        void* proto         = find_proto(key);
        if (!proto) {
            emit_error(request, "Function not yet loaded");
            return;
        }
        if (!luadebug::target::disassembly_supported(target_)) {
            emit_error(request, "Cannot dump function");
            return;
        }

        luadebug::target::Disassembly listing;
        std::string error;
        if (!luadebug::target::disassemble_proto(target_, proto, listing, error)) {
            emit_error(request, error.empty() ? "Cannot dump function" : error);
            return;
        }

        const int byte_offset = static_cast<int>(get_int(request, "offset", 0) / 4);
        const int start       = default_pc + byte_offset + static_cast<int>(get_int(request, "instructionOffset", 0));
        int count             = static_cast<int>(get_int(request, "instructionCount", 0));
        if (count <= 0) {
            count = static_cast<int>(listing.instructions.size());
        }
        const int total = static_cast<int>(listing.instructions.size());

        Json instructions   = Json::array();
        bool first_location = true;
        for (int pc = std::max(0, start); pc < total && pc < std::max(0, start) + count; ++pc) {
            const auto& inst          = listing.instructions[static_cast<std::size_t>(pc)];
            Json entry                = Json::object();
            entry["address"]          = inst.address;
            entry["instructionBytes"] = inst.bytes;
            std::string text          = inst.op_name + "  " + inst.operands;
            entry["instruction"]      = text;
            if (inst.has_line) {
                entry["line"]   = inst.line;
                entry["column"] = 1;
            }
            if (!inst.symbol.empty()) {
                entry["symbol"] = inst.symbol;
            }
            if (inst.invalid) {
                entry["presentationHint"] = "invalid";
            }
            if (first_location) {
                Json location     = Json::object();
                location["name"]  = source;
                location["path"]  = source;
                entry["location"] = std::move(location);
                first_location    = false;
            }
            entry["instructionReference"] = luadebug::target::instruction_reference_for(target_, proto, pc);
            instructions.push_back(std::move(entry));
        }

        Json body            = Json::object();
        body["instructions"] = std::move(instructions);
        emit_response(request, std::move(body));
    }

    void WorkerSession::set_search_path(const Json& request) {
        const Json& args = request.contains("arguments") && request["arguments"].is_object()
                               ? request["arguments"]
                               : request;
        if (!target_) {
            return;
        }
        const int saved_top = target_->gettop();
        target_->getglobal("package");
        if (target_->istable(-1)) {
            const int package = target_->gettop();
            for (const char* key : { "path", "cpath" }) {
                auto it = args.find(key);
                if (it == args.end() || it->is_null()) {
                    continue;
                }
                std::string joined;
                if (it->is_array()) {
                    for (const auto& item : *it) {
                        if (!item.is_string()) {
                            continue;
                        }
                        if (!joined.empty()) {
                            joined.push_back(';');
                        }
                        joined += item.get<std::string>();
                    }
                } else if (it->is_string()) {
                    joined = it->get<std::string>();
                } else {
                    continue;
                }
                target_->pushlstring(joined.data(), joined.size());
                target_->setfield(package, key);
            }
        }
        target_->settop(saved_top);
    }

    void WorkerSession::show_integers(bool hex) {
        show_integer_as_hex_ = hex;
        Json event           = Json::object();
        event["cmd"]         = "eventInvalidated";
        event["reason"]      = "variables";
        event["areas"]       = "variables";
        emit(event);
    }

    void WorkerSession::set_function_breakpoints(const Json& request) {
        function_breakpoints_.clear();
        int next_id = 1;
        if (request["breakpoints"].is_array()) {
            for (const auto& item : request["breakpoints"]) {
                FunctionBreakpoint bp;
                bp.name = std::string(get_string(item, "name"));
                if (bp.name.empty()) {
                    continue;
                }
                bp.id       = static_cast<int>(get_int(item, "id", next_id));
                bp.verified = true;
                next_id     = bp.id + 1;
                function_breakpoints_.push_back(std::move(bp));
            }
        }
        if (hooks_ && hooks_->core && target_) {
            hooks_->core->funcbp_open(target_, !function_breakpoints_.empty());
        }

        Json results = Json::array();
        for (const auto& bp : function_breakpoints_) {
            Json entry        = Json::object();
            entry["id"]       = bp.id;
            entry["verified"] = bp.verified;
            results.push_back(std::move(entry));
        }
        Json body           = Json::object();
        body["breakpoints"] = std::move(results);
        emit_response(request, std::move(body));
    }

    void WorkerSession::set_exception_breakpoints(const Json& request) {
        exception_filters_.clear();
        Json results = Json::array();
        if (request["arguments"].is_array()) {
            for (const auto& item : request["arguments"]) {
                ExceptionFilter filter;
                filter.id        = static_cast<int>(get_int(item, "id", 0));
                filter.condition = std::string(get_string(item, "condition"));
                const std::string filter_id(get_string(item, "filterId"));
                if (filter_id.empty()) {
                    continue;
                }
                exception_filters_[filter_id] = filter;

                Json change          = Json::object();
                change["cmd"]        = "eventBreakpoint";
                change["reason"]     = "changed";
                Json body            = Json::object();
                body["id"]           = filter.id;
                body["verified"]     = true;
                change["breakpoint"] = std::move(body);
                emit(change);
            }
        }
        if (hooks_ && hooks_->core && target_) {
            hooks_->core->exception_open(target_, !exception_filters_.empty());
        } else {
        }

        Json body           = Json::object();
        body["breakpoints"] = std::move(results);
        emit_response(request, std::move(body));
    }

    void WorkerSession::exception_info(const Json& request) {
        Json details          = Json::object();
        details["stackTrace"] = exception_trace_;
        Json body             = Json::object();
        body["breakMode"]     = "always";
        body["exceptionId"]   = exception_message_;
        body["details"]       = std::move(details);
        emit_response(request, std::move(body));
    }

    void WorkerSession::loaded_sources(const Json& request) {
        Json sources = Json::array();
        for (const auto& entry : sources_) {
            Json source               = Json::object();
            source["name"]            = entry.second.name;
            source["path"]            = entry.second.path;
            source["sourceReference"] = entry.second.reference;
            sources.push_back(std::move(source));
        }
        Json body       = Json::object();
        body["sources"] = std::move(sources);
        emit_response(request, std::move(body));
    }

    void WorkerSession::source_content(const Json& request) {
        const int reference = static_cast<int>(get_int(request, "sourceReference", 0));
        std::string path;
        for (const auto& entry : sources_) {
            if (entry.second.reference == reference) {
                path = entry.second.path;
                break;
            }
        }
        std::string content = "Source not available";
        if (!path.empty()) {
            bool ok                = false;
            const std::string text = read_file(path, ok);
            if (ok) {
                content = text;
            }
        }
        Json body        = Json::object();
        body["content"]  = content;
        body["mimeType"] = "text/x-lua";
        emit_response(request, std::move(body));
    }

    void WorkerSession::note_source(vm::State* L, const std::string& path) {
        if (path.empty() || sources_.find(path) != sources_.end()) {
            return;
        }
        SourceEntry entry;
        auto pos        = path.find_last_of("/\\");
        entry.name      = pos == std::string::npos ? path : path.substr(pos + 1);
        entry.path      = path;
        entry.reference = next_source_reference_++;
        sources_.emplace(path, entry);

        Json event                = Json::object();
        event["cmd"]              = "eventLoadedSource";
        event["reason"]           = "new";
        Json source               = Json::object();
        source["name"]            = entry.name;
        source["path"]            = entry.path;
        source["sourceReference"] = entry.reference;
        event["source"]           = std::move(source);
        emit(event);
        (void)L;
    }

    std::string WorkerSession::exception_type(vm::State* L, int errcode) {
        constexpr int ERREVENT_ERRRUN    = 0x02;
        constexpr int ERREVENT_ERRSYNTAX = 0x03;
        const int kind                   = errcode & 0xF;
        if (kind == ERREVENT_ERRSYNTAX) {
            return "syntax";
        }
        if (kind != ERREVENT_ERRRUN) {
            return "unknown";
        }
        if (!L) {
            return "other";
        }
        // Find the raising site: `error`/`assert` show up as C frames right
        // above the code that called them; otherwise the type is decided by the
        // first Lua frame.
        for (int level = 0; level < 6; ++level) {
            vm::FrameInfo ar;
            if (!L->getstack(level, &ar) || !L->getinfo("Sf", &ar)) {
                break;
            }
            const int fn = L->gettop();
            L->getglobal("error");
            L->getglobal("assert");
            const bool is_error  = L->rawequal(fn, -2) != 0;
            const bool is_assert = L->rawequal(fn, -1) != 0;
            const bool is_c      = ar.what && std::strcmp(ar.what, "C") == 0;
            L->pop(3);
            if (is_error) {
                return "error";
            }
            if (is_assert) {
                return "assert";
            }
            if (!is_c) {
                return level == 0 ? "runtime" : "other";
            }
        }
        return "other";
    }

    std::string WorkerSession::exception_caught(vm::State* L, int errcode) {
        constexpr int ERREVENT_PANIC = 0x10;
        if ((errcode & ERREVENT_PANIC) != 0) {
            return "panic";
        }
        if (L) {
            for (int level = 1; level < 100; ++level) {
                vm::FrameInfo ar;
                if (!L->getstack(level, &ar) || !L->getinfo("f", &ar)) {
                    break;
                }
                L->getglobal("pcall");
                L->getglobal("xpcall");
                const bool is_pcall  = L->rawequal(-3, -2) != 0;
                const bool is_xpcall = L->rawequal(-3, -1) != 0;
                L->pop(4);
                if (is_pcall || is_xpcall) {
                    return "lua";
                }
            }
        }
        return "native";
    }

    bool WorkerSession::exception_filter_matches(vm::State* L, const std::string& type, const std::string& caught, const ExceptionFilter& filter) {
        if (filter.condition.empty()) {
            return true;
        }
        // The condition is evaluated with `error` bound to the error object, the
        // way the Lua worker called eval with a symbol table.
        const int saved_top       = L ? L->gettop() : 0;
        vm::State* co             = L->newthread();
        const int thread_slot     = L->gettop();
        const std::string wrapper = "local error = ...\nreturn (" + filter.condition + ")\n";
        if (co->loadbuffer(wrapper.data(), wrapper.size(), "=(EXCEPTION)") != 0) {
            L->settop(saved_top);
            return false;
        }
        // The error object is the top value of the target stack.
        L->pushvalue(-1);
        L->xmove(co, 1);
        const int status   = co->pcall(1, 1, 0);
        const bool matched = status == 0 && co->toboolean(-1) != 0;
        L->settop(saved_top);
        (void)thread_slot;
        return matched;
    }

    std::string WorkerSession::exception_trace(vm::State* L, int skip) {
        std::string trace;
        if (!L) {
            return trace;
        }
        for (int level = skip;; ++level) {
            vm::FrameInfo ar;
            if (!L->getstack(level, &ar) || !L->getinfo("Sl", &ar)) {
                break;
            }
            trace += "\n\t";
            if (ar.source && ar.source[0] == '@') {
                trace += ar.source + 1;
            } else {
                trace += ar.short_src ? ar.short_src : "?";
            }
            trace += ":" + std::to_string(ar.currentline);
            if (ar.name) {
                trace += " in " + std::string(ar.name);
            }
        }
        return trace;
    }

    int WorkerSession::allocate_memory_ref(luadebug::target::HandleId handle, bool is_string) {
        MemoryRef ref;
        ref.handle    = handle;
        ref.is_string = is_string;
        const int id  = next_memory_ref_++;
        memory_refs_.emplace(id, ref);
        return id;
    }

    void WorkerSession::read_memory(const Json& request) {
        const int ref_id = int_field(request, "memoryReference", 0);
        const int offset = static_cast<int>(get_int(request, "offset", 0));
        const int count  = static_cast<int>(get_int(request, "count", 0));

        auto it = memory_refs_.find(ref_id);
        if (it == memory_refs_.end() || !store_ || !frame_state()) {
            emit_error(request, "Error memoryReference");
            return;
        }

        const int saved_top = frame_state()->gettop();
        std::string error;
        if (store_->push(it->second.handle, error) != luadebug::target::EvalStatus::ok) {
            frame_state()->settop(saved_top);
            emit_error(request, error.empty() ? "Error memoryReference" : error);
            return;
        }

        std::string bytes;
        Json body       = Json::object();
        body["address"] = std::to_string(offset);
        if (it->second.is_string) {
            size_t length    = 0;
            const char* text = frame_state()->tolstring(-1, &length);
            std::string value(text ? text : "", length);
            if (offset >= 0 && static_cast<std::size_t>(offset) < value.size() && count > 0) {
                const std::size_t available = value.size() - static_cast<std::size_t>(offset);
                const std::size_t take      = (std::min)(available, static_cast<std::size_t>(count));
                bytes                       = value.substr(static_cast<std::size_t>(offset), take);
            }
        } else {
            void* data        = frame_state()->touserdata(-1);
            const size_t size = frame_state()->rawlen(-1);
            if (data && offset >= 0 && static_cast<std::size_t>(offset) < size && count > 0) {
                const std::size_t available = size - static_cast<std::size_t>(offset);
                const std::size_t take      = (std::min)(available, static_cast<std::size_t>(count));
                bytes.assign(static_cast<const char*>(data) + offset, take);
            }
        }
        frame_state()->settop(saved_top);

        const std::size_t requested = count > 0 ? static_cast<std::size_t>(count) : 0;
        body["data"]                = luadebug::debugger::base64_encode(bytes);
        body["unreadableBytes"]     = requested > bytes.size() ? requested - bytes.size() : 0;
        emit_response(request, std::move(body));
    }

    void WorkerSession::write_memory(const Json& request) {
        const int ref_id = int_field(request, "memoryReference", 0);
        const int offset = static_cast<int>(get_int(request, "offset", 0));
        const std::string encoded(get_string(request, "data"));

        auto it = memory_refs_.find(ref_id);
        if (it == memory_refs_.end() || !store_ || !frame_state()) {
            emit_error(request, "Error memoryReference");
            return;
        }
        std::string bytes;
        if (!luadebug::debugger::base64_decode(encoded, bytes)) {
            emit_error(request, "Invalid base64 data");
            return;
        }

        const int saved_top = frame_state()->gettop();
        std::string error;
        if (store_->push(it->second.handle, error) != luadebug::target::EvalStatus::ok) {
            frame_state()->settop(saved_top);
            emit_error(request, error.empty() ? "Error memoryReference" : error);
            return;
        }

        bool wrote = false;
        if (!it->second.is_string) {
            void* data        = frame_state()->touserdata(-1);
            const size_t size = frame_state()->rawlen(-1);
            if (data && offset >= 0 && static_cast<std::size_t>(offset) < size) {
                const std::size_t available = size - static_cast<std::size_t>(offset);
                const std::size_t take      = (std::min)(available, bytes.size());
                std::memcpy(static_cast<char*>(data) + offset, bytes.data(), take);
                wrote = true;
            }
        }
        frame_state()->settop(saved_top);

        if (!wrote) {
            emit_error(request, "Memory is not writable");
            return;
        }
        Json body            = Json::object();
        body["offset"]       = offset;
        body["bytesWritten"] = static_cast<int>(bytes.size());
        emit_response(request, std::move(body));
    }

    void WorkerSession::set_expression(const Json& request) {
        const int depth = static_cast<int>(get_int(request, "frameId", -1));
        const std::string expression(get_string(request, "expression"));
        const std::string value(get_string(request, "value"));
        if (depth < 0 || !target_ || expression.empty()) {
            emit_error(request, "no frame to assign in");
            return;
        }

        luadebug::target::Evaluator evaluator(frame_state(), stop_base_level_ + depth);
        evaluator.set_write_back([this](const luadebug::refvalue::value* path, std::size_t size, vm::State* from, int index) {
            if (!store_ || !from || size == 0) {
                return false;
            }
            std::vector<luadebug::refvalue::value> copy(path, path + size);
            auto handle     = store_->add_path(std::move(copy));
            const int saved = frame_state()->gettop();
            from->pushvalue(index);
            from->xmove(frame_state(), 1);
            std::string error;
            const auto status = store_->assign(handle, error);
            frame_state()->settop(saved);
            return status == luadebug::target::EvalStatus::ok;
        });

        // Statement mode: `expr = value` writes back through the store, and the
        // expression result is read back afterwards for the response.
        auto outcome = evaluator.run(expression + " = " + value, luadebug::target::EvalMode::statement);
        if (outcome.status != luadebug::target::CallStatus::ok) {
            emit_error(request, outcome.error);
            return;
        }
        const int saved_top = frame_state()->gettop();
        auto read_back      = evaluator.evaluate(expression);
        Json body           = Json::object();
        if (read_back.status == luadebug::target::CallStatus::ok && read_back.nresults > 0) {
            body["result"] = format_value(frame_state()->gettop());
            body["type"]   = short_type(frame_state()->type(-1));
        } else {
            body["result"] = value;
        }
        body["variablesReference"] = 0;
        frame_state()->settop(saved_top);
        emit_response(request, std::move(body));
    }

    void WorkerSession::restart_frame(const Json& request) {
        // Same shape as the Lua worker: drop the references of the old stop and
        // report a restart stop; the client re-requests the frame.
        reset_references();
        if (store_) {
            store_->begin_stop();
        }
        emit_stop("restart", 0, 0);
        Json body = Json::object();
        emit_response(request, std::move(body));
    }

    void WorkerSession::scopes(const Json& request) {
        const int depth = static_cast<int>(get_int(request, "frameId", 0));
        Json scopes     = Json::array();

        Json local                  = Json::object();
        local["name"]               = "Local";
        local["variablesReference"] = allocate_scope(RefKind::local_scope, depth);
        local["expensive"]          = false;
        scopes.push_back(std::move(local));

        Json upvalue                  = Json::object();
        upvalue["name"]               = "Upvalue";
        upvalue["variablesReference"] = allocate_scope(RefKind::upvalue_scope, depth);
        upvalue["expensive"]          = false;
        scopes.push_back(std::move(upvalue));

        Json body      = Json::object();
        body["scopes"] = std::move(scopes);
        emit_response(request, std::move(body));
    }

    void WorkerSession::variables(const Json& request) {
        // The master decodes the client's composite id and forwards the local
        // part as `valueId` (the old worker protocol); tests use the raw field.
        const int ref_id = static_cast<int>(
            get_int(request, "valueId", get_int(request, "variablesReference", 0))
        );
        Json variables = Json::array();

        auto it = refs_.find(ref_id);
        if (it != refs_.end() && target_) {
            const ValueRef ref = it->second;
            // A stop inside a coroutine owns its own stack: reading values off the
            // main state would use the wrong indices (and can run past its top).
            vm::State* stop_thread_for_variables = frame_state();
            if (ref.kind == RefKind::local_scope || ref.kind == RefKind::upvalue_scope) {
                vm::FrameInfo ar;
                const int level = stop_base_level_ + ref.depth;
                if (stop_thread_for_variables->getstack(level, &ar)) {
                    if (ref.kind == RefKind::local_scope) {
                        for (int i = 1;; ++i) {
                            const char* name = stop_thread_for_variables->getlocal(&ar, i);
                            if (!name) {
                                break;
                            }
                            if (name[0] == '(') {
                                stop_thread_for_variables->pop(1);
                                continue;
                            }
                            const int value       = stop_thread_for_variables->gettop();
                            const int type        = stop_thread_for_variables->type(value);
                            Json entry            = Json::object();
                            entry["name"]         = name;
                            entry["value"]        = format_value(value);
                            entry["type"]         = short_type(type);
                            entry["evaluateName"] = name;
                            if (type == vm::type::string || type == vm::type::userdata) {
                                auto handle = store_->add_path({ luadebug::refvalue::FRAME_LOCAL { static_cast<std::uint16_t>(level - stop_base_level_), static_cast<std::int16_t>(i) } });
                                // DAP wants a string; the runtime prefixes the
                                // thread id ("memory_<tid>x<ref>").
                                entry["memoryReference"] =
                                    std::to_string(allocate_memory_ref(handle, type == vm::type::string));
                            }
                            if (type == vm::type::table || type == vm::type::userdata) {
                                auto handle = store_->add_path({ luadebug::refvalue::FRAME_LOCAL { static_cast<std::uint16_t>(level - stop_base_level_), static_cast<std::int16_t>(i) } });
                                entry["variablesReference"] =
                                    allocate_value(handle, std::string(name));
                            } else {
                                entry["variablesReference"] = 0;
                            }
                            variables.push_back(std::move(entry));
                            stop_thread_for_variables->pop(1);
                        }
                    } else {
                        // Upvalues belong to the frame's function.
                        if (stop_thread_for_variables->getinfo("f", &ar)) {
                            const int function = stop_thread_for_variables->gettop();
                            for (int i = 1;; ++i) {
                                const char* name = stop_thread_for_variables->getupvalue(function, i);
                                if (!name) {
                                    break;
                                }
                                const int value       = stop_thread_for_variables->gettop();
                                const int type        = stop_thread_for_variables->type(value);
                                Json entry            = Json::object();
                                entry["name"]         = name[0] ? name : "(no name)";
                                entry["value"]        = format_value(value);
                                entry["type"]         = short_type(type);
                                entry["evaluateName"] = name;
                                if (type == vm::type::string || type == vm::type::userdata) {
                                    auto frame_fn = store_->add_path({ luadebug::refvalue::FRAME_FUNC { static_cast<std::uint16_t>(level - stop_base_level_) } });
                                    auto child    = store_->extend_path(
                                        frame_fn, luadebug::refvalue::UPVALUE { i }
                                    );
                                    entry["memoryReference"] =
                                        std::to_string(allocate_memory_ref(child, type == vm::type::string));
                                }
                                if (type == vm::type::table || type == vm::type::userdata) {
                                    // An upvalue resolves through its parent
                                    // function, so the path starts at the frame.
                                    auto frame_fn = store_->add_path({ luadebug::refvalue::FRAME_FUNC { static_cast<std::uint16_t>(level - stop_base_level_) } });
                                    auto child    = store_->extend_path(
                                        frame_fn, luadebug::refvalue::UPVALUE { i }
                                    );
                                    entry["variablesReference"] =
                                        allocate_value(child, std::string(name));
                                } else {
                                    entry["variablesReference"] = 0;
                                }
                                variables.push_back(std::move(entry));
                                stop_thread_for_variables->pop(1);
                            }
                            stop_thread_for_variables->pop(1);  // function
                        }
                    }
                }
            } else if (ref.kind == RefKind::value) {
                const int saved_top = stop_thread_for_variables->gettop();
                if (push_ref(ref)) {
                    enumerate_container(ref, stop_thread_for_variables->gettop(), variables);
                }
                target_->settop(saved_top);
            }
        }

        Json body         = Json::object();
        body["variables"] = std::move(variables);
        emit_response(request, std::move(body));
    }

    void WorkerSession::evaluate(const Json& request) {
        const int depth = static_cast<int>(get_int(request, "frameId", -1));
        const std::string expression(get_string(request, "expression"));
        if (depth < 0 || !target_) {
            emit_error(request, "no frame to evaluate in");
            return;
        }

        // Nested Lua work must start from a clean stack: a leftover value would
        // sit below the evaluator's protected call and shift its arguments.
        const int eval_top = target_->gettop();
        luadebug::target::Evaluator evaluator(frame_state(), stop_base_level_ + depth);
        // Statement write-back goes through the value store, so the evaluator
        // never has to touch the suspended frame itself.
        evaluator.set_write_back([this](const luadebug::refvalue::value* path, std::size_t size, vm::State* from, int index) {
            if (!store_ || !from || size == 0) {
                return false;
            }
            std::vector<luadebug::refvalue::value> copy(path, path + size);
            auto handle         = store_->add_path(std::move(copy));
            const int saved_top = frame_state()->gettop();
            from->pushvalue(index);
            from->xmove(frame_state(), 1);
            std::string error;
            const auto status = store_->assign(handle, error);
            frame_state()->settop(saved_top);
            return status == luadebug::target::EvalStatus::ok;
        });
        auto outcome = evaluator.evaluate(expression);
        if (outcome.status != luadebug::target::CallStatus::ok) {
            emit_error(request, outcome.error);
            return;
        }

        Json body = Json::object();
        if (outcome.nresults > 0) {
            body["result"] = format_value(frame_state()->gettop());
            body["type"]   = short_type(target_->type(-1));
            const int type = target_->type(-1);
            if (type == vm::type::table || type == vm::type::userdata) {
                // pin_top() takes the value off the stack and keeps it alive.
                auto handle                = store_->pin_top();
                body["variablesReference"] = allocate_value(handle, expression);
            } else {
                body["variablesReference"] = 0;
            }
        } else {
            body["result"]             = "nil";
            body["type"]               = "nil";
            body["variablesReference"] = 0;
        }
        target_->settop(eval_top);
        emit_response(request, std::move(body));
    }

    void WorkerSession::set_variable(const Json& request) {
        // The master decodes the client's composite id and forwards the local
        // part as `valueId` (the old worker protocol); tests use the raw field.
        const int ref_id = static_cast<int>(
            get_int(request, "valueId", get_int(request, "variablesReference", 0))
        );
        const std::string name(get_string(request, "name"));
        const std::string text(get_string(request, "value"));
        auto it = refs_.find(ref_id);
        if (it == refs_.end() || it->second.kind != RefKind::local_scope || !target_) {
            emit_error(request, "Failed set variable");
            return;
        }

        vm::FrameInfo ar;
        const int level = stop_base_level_ + it->second.depth;
        if (!frame_state()->getstack(level, &ar)) {
            emit_error(request, "Failed set variable");
            return;
        }
        int index = 0;
        for (int i = 1;; ++i) {
            const char* local = target_->getlocal(&ar, i);
            if (!local) {
                break;
            }
            target_->pop(1);
            if (name == local) {
                index = i;
                break;
            }
        }
        if (index == 0) {
            emit_error(request, "Failed set variable");
            return;
        }

        // Value text follows the old worker: nil/true/false, quoted strings,
        // numbers, otherwise the raw text.
        const int saved_top = frame_state()->gettop();
        if (text == "nil") {
            target_->pushnil();
        } else if (text == "true") {
            target_->pushboolean(1);
        } else if (text == "false") {
            target_->pushboolean(0);
        } else if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
            target_->pushlstring(text.data() + 1, text.size() - 2);
        } else {
            // `tonumber` semantics: an integer literal stays an integer on the
            // versions that have an integer subtype (an assigned 42 must not
            // become 42.0 in the target).
            bool converted = false;
            if (target_->vm()->api().lua_version_num() >= 503) {
                converted = target_->stringtonumber(text.c_str());
            } else {
                char* end           = nullptr;
                const double number = std::strtod(text.c_str(), &end);
                if (end && *end == '\0' && !text.empty()) {
                    target_->pushnumber(number);
                    converted = true;
                }
            }
            if (!converted) {
                target_->pushlstring(text.data(), text.size());
            }
        }

        auto handle = store_->add_path({ luadebug::refvalue::FRAME_LOCAL { static_cast<std::uint16_t>(it->second.depth), static_cast<std::int16_t>(index) } });
        std::string error;
        const auto status = store_->assign(handle, error);
        frame_state()->settop(saved_top);
        if (status != luadebug::target::EvalStatus::ok) {
            emit_error(request, error.empty() ? "Failed set variable" : error);
            return;
        }

        Json body     = Json::object();
        body["value"] = text;
        emit_response(request, std::move(body));
    }

    void WorkerSession::emit_error(const Json& request, const std::string& message) {
        if (!request.is_object() || !request.contains("seq")) {
            return;
        }
        Json response           = Json::object();
        response["type"]        = "response";
        response["success"]     = false;
        response["command"]     = get_string(request, "command");
        response["request_seq"] = get_int(request, "seq", 0);
        response["message"]     = message;
        emit(response);
    }

    bool WorkerSession::resolve_source_text(const Json& source, const Json& content, std::string& text, std::string& error) const {
        if (content.is_string()) {
            text = content.get<std::string>();
            return true;
        }
        const auto path = std::string(get_string(source, "path"));
        if (path.empty()) {
            error = "no source path";
            return false;
        }
        bool ok = false;
        text    = read_file(path, ok);
        if (!ok) {
            error = "cannot read " + path;
            return false;
        }
        return true;
    }

    bool WorkerSession::verify(Breakpoint& bp, const std::string& source_text, const std::vector<int>& remap) const {
        (void)source_text;
        if (!bp.condition.empty()) {
            const auto result = target::compile_check(target_, "return " + bp.condition, "=(EVAL)");
            if (result.status != target::CompileStatus::ok) {
                bp.message = normalize_message("Condition Error", result.message);
                return false;
            }
        }
        if (!bp.hit_condition.empty()) {
            // worker/eval/verify.lua compiled `return <source>`; a hit condition is
            // an expression compared against the hit count.
            const auto result = target::compile_check(target_, "return 0 " + bp.hit_condition, "=(EVAL)");
            if (result.status != target::CompileStatus::ok) {
                bp.message = normalize_message("HitCondition Error", result.message);
                return false;
            }
        }
        if (bp.line <= 0 || bp.line >= static_cast<int>(remap.size())) {
            bp.message = "The breakpoint didn't hit a valid line.";
            return false;
        }
        const int real = remap[static_cast<std::size_t>(bp.line)];
        if (real <= 0) {
            bp.message = "The breakpoint didn't hit a valid line.";
            return false;
        }
        bp.real_line = real;
        bp.verified  = true;
        return true;
    }

    void WorkerSession::set_breakpoints(const Json& source, const Json& breakpoints, const Json& content) {
        const std::string source_path(get_string(source, "path"));

        std::string text;
        std::string error;
        target::LineInfo line_info;
        bool have_line_info = false;
        if (resolve_source_text(source, content, text, error)) {
            have_line_info = target::build_line_info_from_source(target_, text, line_info, error);
            if (!have_line_info) {
                // A compile error is reported per breakpoint, not swallowed.
                log_info("setBreakpoints: %s", error.c_str());
            }
        }

        // parser.lua's normalize(): per line, where a breakpoint really lands.
        remap_.assign(static_cast<std::size_t>(line_info.max_line()) + 2, 0);
        for (int line = 1; line <= line_info.max_line(); ++line) {
            remap_[static_cast<std::size_t>(line)] = line_info.remap(line);
        }

        // Replace only this source's entries, then rebuild the flattened view.
        std::vector<Breakpoint>& mine = breakpoints_by_source_[source_path];
        mine.clear();
        if (breakpoints.is_array()) {
            for (const auto& item : breakpoints) {
                Breakpoint bp;
                bp.id = static_cast<int>(get_int(item, "id", 0));
                if (bp.id == 0) {
                    bp.id = next_id_++;
                }
                bp.line          = static_cast<int>(get_int(item, "line", 0));
                bp.source_path   = source_path;
                bp.condition     = std::string(get_string(item, "condition"));
                bp.hit_condition = std::string(get_string(item, "hitCondition"));
                bp.log_message   = std::string(get_string(item, "logMessage"));
                mine.push_back(std::move(bp));
            }
        }

        for (auto& bp : mine) {
            if (!have_line_info) {
                bp.verified = false;
                bp.message  = text.empty() ? ("The source file is not loaded.") : error;
            } else {
                verify(bp, text, remap_);
            }
            emit_breakpoint_changed(bp);
        }

        breakpoints_.clear();
        for (const auto& entry : breakpoints_by_source_) {
            breakpoints_.insert(breakpoints_.end(), entry.second.begin(), entry.second.end());
        }

        // A cached per-prototype decision was derived from the old table.
        if (hooks_ && hooks_->core) {
            hooks_->core->break_reset();
        }

        // Arm the call/return hook so prototypes get discovered; the line hook is
        // then enabled per prototype and per state from the decisions the hook
        // produces. The state to arm is the one that will run next: a breakpoint
        // set while stopped inside a coroutine must reach that coroutine's own
        // mask, and no resume event will come to repair it later.
        if (hooks_ && hooks_->core && target_) {
            bool any_verified = false;
            for (const auto& bp : breakpoints_) {
                any_verified = any_verified || bp.verified;
            }
            vm::State* state = frame_state();
            hooks_->core->break_open(state, any_verified);
            if (any_verified) {
                hooks_->core->update_open(state, true);
            }
        }
    }

    LdStatus WorkerSession::dispatch(std::string_view command_json) {
        Json request;
        std::string error;
        if (parse_json(command_json, request, error) != ParseStatus::ok) {
            log_error("worker: invalid command json: %s", error.c_str());
            return LD_STATUS_INVALID_ARGUMENT;
        }
        if (!request.is_object()) {
            return LD_STATUS_INVALID_ARGUMENT;
        }

        const auto cmd = std::string(get_string(request, "cmd"));

        // The debugger's own inspection (compiling conditions, reading frames,
        // running continuation chunks) must not produce hook events: a line hook
        // firing inside a command would nest a stop in the middle of one, which
        // is exactly the recursion the session must never allow. Suppress the
        // target's hooks for the duration and restore the previous state.
        vm::State* hook_state    = stop_thread_ ? stop_thread_ : target_;
        const bool hooks_were_on = hook_state ? hook_state->allowhook() : false;
        struct AllowHookGuard {
            vm::State* state;
            bool previous;
            ~AllowHookGuard() {
                if (state) {
                    state->set_allowhook(previous);
                }
            }
        } allow_hook { hook_state, hooks_were_on };
        if (hook_state && hooks_were_on) {
            hook_state->set_allowhook(false);
        }

        if (cmd == "initializing") {
            if (request.contains("config")) {
                apply_output_capture(request["config"]);
            }
            // Config replay arrives before anything else; the session becomes
            // ready for commands (breakpoint set up, hooks) but stays stopped
            // until the target actually runs.
            state_          = State::initializing;
            Json response   = Json::object();
            response["cmd"] = "eventInitialized";
            emit(response);
            return LD_STATUS_OK;
        }

        if (cmd == "stackTrace") {
            stack_trace(request);
            return LD_STATUS_OK;
        }

        if (cmd == "setExceptionBreakpoints") {
            set_exception_breakpoints(request);
            return LD_STATUS_OK;
        }
        if (cmd == "exceptionInfo") {
            exception_info(request);
            return LD_STATUS_OK;
        }
        if (cmd == "loadedSources") {
            loaded_sources(request);
            return LD_STATUS_OK;
        }
        if (cmd == "readMemory") {
            read_memory(request);
            return LD_STATUS_OK;
        }
        if (cmd == "writeMemory") {
            write_memory(request);
            return LD_STATUS_OK;
        }
        if (cmd == "setExpression") {
            set_expression(request);
            return LD_STATUS_OK;
        }
        if (cmd == "restartFrame") {
            restart_frame(request);
            return LD_STATUS_OK;
        }
        if (cmd == "source") {
            source_content(request);
            return LD_STATUS_OK;
        }
        if (cmd == "threads") {
            threads(request);
            return LD_STATUS_OK;
        }
        if (cmd == "setSearchPath") {
            set_search_path(request);
            return true;
        }
        if (cmd == "customRequestShowIntegerAsDec") {
            show_integers(false);
            return true;
        }
        if (cmd == "customRequestShowIntegerAsHex") {
            show_integers(true);
            return true;
        }
        if (cmd == "initialized") {
            // The master tells a worker the session is configured; the client
            // learns about the VM through `thread`. The bridge waits for this
            // before handing control back to the debuggee.
            ready_          = true;
            Json event      = Json::object();
            event["cmd"]    = "eventThread";
            event["reason"] = "started";
            emit(event);
            return true;
        }
        if (cmd == "terminated") {
            Json event      = Json::object();
            event["cmd"]    = "eventThread";
            event["reason"] = "exited";
            emit(event);
            return true;
        }
        if (cmd == "suspend") {
            // Nothing to do: the target is not resumed by a disconnect here.
            return true;
        }
        if (cmd == "setInstructionBreakpoints") {
            set_instruction_breakpoints(request);
            return true;
        }
        if (cmd == "disassemble") {
            disassemble(request);
            return true;
        }
        if (cmd == "setFunctionBreakpoints") {
            set_function_breakpoints(request);
            return LD_STATUS_OK;
        }
        if (cmd == "scopes") {
            scopes(request);
            return LD_STATUS_OK;
        }
        if (cmd == "variables") {
            variables(request);
            return LD_STATUS_OK;
        }
        if (cmd == "evaluate") {
            evaluate(request);
            return LD_STATUS_OK;
        }
        if (cmd == "setVariable") {
            set_variable(request);
            return LD_STATUS_OK;
        }

        if (cmd == "setBreakpoints") {
            set_breakpoints(request["source"], request["breakpoints"], request["content"]);
            return LD_STATUS_OK;
        }

        if (cmd == "run" || cmd == "continue") {
            // `continue` is broadcast to every worker of the session, including
            // ones that have not stopped. Only a stopped worker has a stop loop to
            // release; a worker that is still configuring just becomes "running"
            // (that is the state its later stops are taken from), and one that is
            // already running or leaving is left alone.
            if (state_ == State::stopped) {
                state_ = State::running;
                resume(luadebug::hook::StepMode::none);
            } else if (state_ == State::initializing) {
                state_ = State::running;
            }
            return LD_STATUS_OK;
        }

        if (cmd == "stepOver" || cmd == "stepIn" || cmd == "stepOut") {
            state_                 = State::running;
            requested_stop_reason_ = "step";
            resume(cmd == "stepOver" ? luadebug::hook::StepMode::over : (cmd == "stepIn" ? luadebug::hook::StepMode::in : luadebug::hook::StepMode::out));
            return LD_STATUS_OK;
        }

        if (cmd == "stop" || cmd == "pause") {
            // Entry/pause stops are requested by the control side; the next hook
            // line produces the stopped event with the requested reason.
            const std::string reason { get_string(request, "reason") };
            requested_stop_reason_ = reason.empty() ? "entry" : reason;
            state_                 = State::running;
            if (hooks_ && hooks_->core) {
                hooks_->core->step(luadebug::hook::StepMode::in);
            }
            return LD_STATUS_OK;
        }

        if (cmd == "disconnect" || cmd == "terminate") {
            // The master answers the client; the worker only reports that this
            // VM leaves the session and drops everything bound to it.
            {
                Json event      = Json::object();
                event["cmd"]    = "eventThread";
                event["reason"] = "exited";
                emit(event);
            }
            state_ = State::terminating;
            breakpoints_.clear();
            remap_.clear();
            state_ = State::closed;
            return LD_STATUS_OK;
        }

        // Everything else is not ported yet: say so instead of pretending.
        emit_error(request, "worker command `" + cmd + "` is not implemented yet");
        return LD_STATUS_FAILED;
    }

}  // namespace luadebug::worker
