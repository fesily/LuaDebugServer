#pragma once

// Bridge-side worker session: the C++ replacement for the state machine in
// extension/script/backend/worker.lua.
//
// Scope of the current slice: the session skeleton (lifecycle, command
// dispatch, event emission) plus `setBreakpoints`, which is verified from the
// target's own Protos via target::build_line_info. Commands that are not
// implemented yet answer with an explicit error response — never with a
// fabricated success — and the bridge only takes over from the Lua worker once
// the whole command table is covered.

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

#include "common/json.h"
#include "hook/sink.h"
#include "rdebug_redirect.h"
#include "target/value_store.h"
#include "runtime/abi.h"
#include "vm/vm.h"

namespace luadebug::worker {

    using Json = luadebug::debugger::Json;

    enum class State {
        initializing,
        running,
        stopped,
        terminating,
        closed,
    };

    const char* state_name(State state);

    struct Breakpoint {
        int id        = 0;
        int line      = 0;  // line the client asked for
        int real_line = 0;  // line it will actually stop on (0 = unverified)
        bool verified = false;
        std::string message;
        std::string condition;
        std::string hit_condition;
        std::string log_message;
        std::string source_path;
    };

    class WorkerSession final : public luadebug::hook::Sink
        , public luadebug::stdio::output_sink {
    public:
        explicit WorkerSession(vm::State* target);
        ~WorkerSession() override;

        void set_sink(LdSink sink, void* sink_ctx) {
            sink_     = sink;
            sink_ctx_ = sink_ctx;
        }

        // Entry point for runtime enqueue/pump. Returns LD_STATUS_OK when the
        // command was handled (a response/event may have been emitted), a
        // negative status otherwise.
        LdStatus dispatch(std::string_view command_json);

        State state() const {
            return state_;
        }

        // Verification entry point, exposed for tests and for the hook side.
        // Emits one `eventBreakpoint` per breakpoint.
        void set_breakpoints(const Json& source, const Json& breakpoints, const Json& content);

        // Installs the target hooks and starts receiving hook events.
        void attach_hooks();
        bool hooks_attached() const;

        // True once the master finished configuring this worker (`initialized`).
        // The target must not run before that, otherwise breakpoints set by the
        // client would miss the code that runs at startup.
        bool ready() const {
            return ready_;
        }

        // Commands are drained by this callback while the target is stopped. It
        // returns false when the worker is gone (the stop loop then unwinds).
        void set_pump(std::function<bool()> pump);
        // Non-blocking drain used by the periodic hook tick while the target
        // runs, so commands (breakpoints, run) reach the session mid-execution.
        void set_idle_pump(std::function<void()> pump);

        // --- hook::Sink -------------------------------------------------------
        bool new_proto(vm::State* L, void* proto, int level) override;
        void function_break(vm::State* L) override;
        void instruction_break(vm::State* L, void* proto) override;
        void exception(vm::State* L, int errcode) override;
        void line_stop(vm::State* L, bool stepping, int line, void* proto) override;
        void update(vm::State* L) override;

        // --- luadebug::stdio::output_sink ------------------------------------
        // Captured `print` / `io.write` from the target (`outputCapture`).
        bool output(vm::State* hL, const char* name, int first) override;

        // DAP-shaped frames for the stopped thread (local ids; the runtime adds
        // the thread id). Emits the `stackTrace` response.
        void stack_trace(const Json& request);

        // Function breakpoints, threads and logpoint output.
        void set_function_breakpoints(const Json& request);
        void threads(const Json& request);
        void set_exception_breakpoints(const Json& request);
        void exception_info(const Json& request);
        void loaded_sources(const Json& request);
        void source_content(const Json& request);
        void set_expression(const Json& request);
        void restart_frame(const Json& request);
        void read_memory(const Json& request);
        void write_memory(const Json& request);
        // Instruction-level breakpoints and the bytecode listing they address.
        void set_instruction_breakpoints(const Json& request);
        void disassemble(const Json& request);
        // Editor conveniences forwarded by the master.
        void set_search_path(const Json& request);
        void show_integers(bool hex);

        // DAP-shaped scopes / variables / evaluation for the stopped frame.
        // References are local ids; the runtime ORs in the thread id.
        void scopes(const Json& request);
        void variables(const Json& request);
        void evaluate(const Json& request);
        void set_variable(const Json& request);

        // Stop bookkeeping, exposed for tests.
        int last_stop_line() const {
            return last_stop_line_;
        }
        std::string last_stop_reason() const {
            return last_stop_reason_;
        }

        const std::vector<Breakpoint>& breakpoints() const {
            return breakpoints_;
        }

    private:
        struct ExceptionFilter {
            int id = 0;
            std::string condition;
        };
        std::map<std::string, ExceptionFilter> exception_filters_;
        std::string exception_message_;
        std::string exception_trace_;
        // Sources seen so far, keyed by path.
        struct SourceEntry {
            std::string name;
            std::string path;
            int reference = 0;
        };
        struct MemoryRef {
            luadebug::target::HandleId handle = 0;
            bool is_string                    = false;
        };
        std::map<int, MemoryRef> memory_refs_;
        int next_memory_ref_ = 1;
        int allocate_memory_ref(luadebug::target::HandleId handle, bool is_string);
        bool show_integer_as_hex_ = false;
        bool ready_               = false;
        // Reason reported for the next step stop: `CMD.stop` sets entry/pause,
        // the step commands set "step" (worker.lua's stopReason).
        std::string requested_stop_reason_;

        // Output capture (config.outputCapture). The redirects are owned here so
        // a session that ends cannot leave the process writing into a pipe
        // nobody reads.
        void apply_output_capture(const Json& config);
        void drain_redirects();
        std::unique_ptr<luadebug::stdio::std_redirect> stdout_redirect_;
        std::unique_ptr<luadebug::stdio::std_redirect> stderr_redirect_;
        bool capture_print_   = false;
        bool capture_iowrite_ = false;

        std::map<std::string, SourceEntry> sources_;
        int next_source_reference_ = 1;
        enum class RefKind {
            local_scope,
            upvalue_scope,
            value,
        };

        struct ProtoKey {
            int linedefined     = 0;
            int lastlinedefined = 0;
            std::string source;
        };
        struct InstructionBreakpoint {
            int id = 0;
            int pc = 0;
            std::string reference;
            std::string condition;
            std::string hit_condition;
            std::string log_message;
            int hits = 0;
        };
        // Remembers a prototype's identity and retries pending instruction
        // breakpoints.
        void note_proto(void* proto);
        void* find_proto(const ProtoKey& key) const;
        int current_pc(vm::State* L) const;
        void emit_instruction_breakpoint(const InstructionBreakpoint& bp, bool verified, const std::string& message);

        struct FunctionBreakpoint {
            std::string name;
            int id        = 0;
            bool verified = false;
        };

        struct ValueRef {
            RefKind kind                      = RefKind::value;
            int depth                         = 0;  // frame depth for scopes
            luadebug::target::HandleId handle = 0;  // value refs
            std::string evaluate_name;
        };

        int allocate_scope(RefKind kind, int depth);
        int allocate_value(luadebug::target::HandleId handle, std::string evaluate_name);
        int allocate_child(const ValueRef& parent, luadebug::refvalue::value member);
        void reset_references();

        // Pushes the value behind `ref` onto the target stack; false on failure.
        bool push_ref(const ValueRef& ref);
        std::string format_value(int stack_index);
        // Same, on an arbitrary state (the stop thread, a hook's thread).
        std::string format_value_on(vm::State* L, int stack_index) const;
        int child_reference(const ValueRef& parent, int key_stack_index);
        // Enumerates a container at `stack_index` into `variables`.
        void enumerate_container(const ValueRef& ref, int stack_index, Json& variables);

        struct StopRequest {
            bool pending = false;
            std::string reason;
            int line          = 0;
            int breakpoint_id = 0;
        };

        void emit(const Json& message);
        // Emits the stop event and services commands until the target resumes.
        void enter_stop(const std::string& reason, int line, int breakpoint_id, vm::State* hL);
        void emit_stop(const std::string& reason, int line, int breakpoint_id);
        std::string frame_name(vm::State* L, int depth, const vm::FrameInfo& ar) const;
        void emit_response(const Json& request, Json body);
        void resume(luadebug::hook::StepMode mode);
        // Breakpoints whose real line falls inside [start, end].
        bool wants_line_hooks(int start, int end, int& breakpoint_id) const;
        // Returns the breakpoint to act on (0 = none) and fills `log_output`
        // when the breakpoint is a logpoint. Hit conditions are honoured here.
        int breakpoint_at_line(vm::State* L, const std::string& source, int line, std::string& log_output);
        // Evaluates the hit condition against the current hit count.
        bool hit_condition_met(const Breakpoint& bp);
        bool hit_condition_met(int id, const std::string& hit_condition);
        // Exception classification, mirroring worker.lua's getExceptionType /
        // getExceptionCaught.
        std::string exception_type(vm::State* L, int errcode);
        std::string exception_caught(vm::State* L, int errcode);
        bool exception_filter_matches(vm::State* L, const std::string& type, const std::string& caught, const ExceptionFilter& filter);
        std::string exception_trace(vm::State* L, int skip);
        void note_source(vm::State* L, const std::string& path);
        // Expands `{expr}` placeholders in a logpoint message.
        std::string expand_log_message(const Breakpoint& bp);
        std::string expand_log_message(const std::string& log_message);
        std::string expand_log_message_at(vm::State* L, const std::string& log_message);

        // Evaluates a breakpoint condition at the stop frame.
        bool eval_condition(const std::string& expression);
        // Same, evaluated against `L`'s innermost Lua frame (a hook context),
        // which is what a breakpoint condition refers to.
        bool eval_condition_at(vm::State* L, const std::string& expression);
        std::string current_source(vm::State* L) const;

        void emit_breakpoint_changed(const Breakpoint& bp);
        void emit_error(const Json& request, const std::string& message);

        bool resolve_source_text(const Json& source, const Json& content, std::string& text, std::string& error) const;
        // Returns false and fills `bp.message` when the breakpoint cannot be
        // honoured.
        bool verify(Breakpoint& bp, const std::string& source_text, const std::vector<int>& remap) const;

        vm::State* target_ = nullptr;
        LdSink sink_       = nullptr;
        void* sink_ctx_    = nullptr;
        State state_       = State::initializing;
        // Breakpoints per source: a DAP client sets one source at a time, and a
        // later source must not drop an earlier one's entries (that would make a
        // two-VM session stop in only one of its scripts).
        std::map<std::string, std::vector<Breakpoint>> breakpoints_by_source_;
        // Flattened view of the same entries, used by the hot paths.
        std::vector<Breakpoint> breakpoints_;
        int next_id_ = 1;
        // line -> line the breakpoint really lands on, for the source in play.
        std::vector<int> remap_;
        std::vector<FunctionBreakpoint> function_breakpoints_;
        std::map<void*, ProtoKey> proto_keys_;
        // (linedefined, lastlinedefined) -> {source, prototype}
        std::map<std::pair<int, int>, std::vector<std::pair<std::string, void*>>> proto_ranges_;
        std::map<void*, std::map<int, InstructionBreakpoint>> inst_breakpoints_;
        std::map<std::pair<int, int>, std::vector<InstructionBreakpoint>>
            pending_inst_breakpoints_;
        std::map<std::string, int> hit_counts_;
        std::map<int, int> breakpoint_hits_;

        std::map<int, ValueRef> refs_;
        int next_ref_ = 1;
        std::vector<int> key_refs_;  // target registry refs of table keys
        luadebug::target::ValueStore* store_ = nullptr;

        struct HookState;
        std::unique_ptr<HookState> hooks_;
        std::function<bool()> pump_;
        std::function<void()> idle_pump_;
        bool stopped_                          = false;
        bool resume_requested_                 = false;
        luadebug::hook::StepMode pending_step_ = luadebug::hook::StepMode::none;
        int last_stop_line_                    = 0;
        // Absolute lua_getstack level of the innermost Lua frame at the stop;
        // DAP frame 0 maps to this level.
        int stop_base_level_ = 0;
        // Thread the stop happened on (a coroutine, when the target runs one).
        vm::State* stop_thread_ = nullptr;
        // State frame operations must use: the stop thread, else the target.
        vm::State* frame_state() const {
            return stop_thread_ ? stop_thread_ : target_;
        }
        std::string last_stop_reason_;
        int stop_depth_ = 0;
    };

}  // namespace luadebug::worker
