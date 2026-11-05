#pragma once

// Injection backends: how the launcher image gets into a live process.
//
// The mechanism depends on the platform and on the architecture relation
// between the adapter and its target:
//
//   * `frida` speaks frida-core. The macOS build spawns the per-architecture
//     client (`bin/frida-inject-<arch>`), every other platform links frida-core
//     into the adapter. It is the default when the two architectures match.
//   * `lldb` drives `lldb --batch -p <pid>` and evaluates `dlopen` plus the entry
//     call inside the target. It is the only option when the architectures
//     differ - an arm64 adapter attaching to a Rosetta x86_64 process - because
//     frida's agent has to match the target's architecture.
//
// A backend only has to answer two questions: which command maps the image and
// calls the entry point, and what a given run's status means. Running that
// command - including the one escalation retry through the OS - belongs to the
// dispatcher, so both backends share the same permission path.

#include <string>
#include <vector>

#include "common/json.h"
#include "frontend/inject.h"
#include "frontend/spawn.h"

namespace luadebug::debugger::frontend {

    // Exported by the launcher and called after the image is mapped:
    // `void(const char* data, int32_t* unload_policy, void* injector_state)`.
    inline constexpr const char kLauncherEntrypoint[] = "luadebug_frida_entry";

    // What one injection needs, independent of the backend.
    struct InjectionRequest {
        long pid = 0;
        std::string launcher;     // absolute path of the launcher image
        std::string data;         // bootstrap document (may be empty)
        std::string target_arch;  // "arm64" | "x64" | "" when unknown
        InjectEntry entry = InjectEntry::attach;
    };

    // The command that performs the injection. `argv` empty means "in process":
    // frida-core is linked into the adapter on Linux and Windows.
    struct InjectionPlan {
        std::vector<std::string> argv;
        std::string label;  // executable name, for logs and error text
    };

    struct InjectionOutcome {
        bool ok = false;
        // The backend could not get the permission it needs: the dispatcher may
        // retry the very same command through the OS escalation path.
        bool permission_denied = false;
        // The failure is the target's state, not the request (a process that is
        // still inside dyld startup cannot host lldb's injected utility
        // function). The dispatcher retries the same command a few times.
        bool retryable = false;
        std::string error;
    };

    enum class InjectorBackend {
        frida,
        lldb,
    };

    const char* injector_backend_name(InjectorBackend backend);

    // Reads `inject` from the DAP arguments, then `LUA_DEBUG_INJECTOR`, then
    // applies the architecture rule above. `automatic` reports whether the
    // decision was the default (the caller logs it, it is not an error either
    // way). `inject: "none"` is resolved earlier, by resolve_inject_mode().
    bool resolve_injector_backend(const Json& arguments, const std::string& target_arch, InjectorBackend& backend, bool& automatic, std::string& error);

    // Backends. Each returns false with `error` set when it cannot serve this
    // request at all (wrong platform, missing client binary).
    bool make_frida_plan(const InjectionRequest& request, const InjectContext& context, InjectionPlan& plan, std::string& error);
    bool make_lldb_plan(const InjectionRequest& request, const InjectContext& context, InjectionPlan& plan, std::string& error);

    // A launch the backend performs itself. The target is created by the backend
    // (lldb's `process launch --stop-at-entry`) because a Rosetta target ignores
    // DYLD_INSERT_LIBRARIES, so the preload carrier cannot reach it.
    struct LaunchRequest {
        std::vector<std::string> argv;  // the target's own command line
        std::string cwd;                // empty = inherit the adapter's
        Json env = Json::object();      // overrides applied to the inherited env
        std::string launcher;
        std::string data;  // bootstrap document
        std::string target_arch;
    };

    // Starts the target, loads the launcher into it at the entry point, calls the
    // launcher entry and detaches so the target runs on its own. Uses the
    // adapter's own stdio for the debuggee, so console output stays visible.
    bool launch_with_lldb(const LaunchRequest& request, std::string& error);

    // Maps one captured run (unprivileged or escalated) onto the outcome.
    InjectionOutcome interpret_frida_run(const CapturedRun& run);
    InjectionOutcome interpret_lldb_run(const CapturedRun& run);

    // ---- shared command plumbing -------------------------------------------

    // POSIX shell quoting for the escalation path.
    std::string shell_quote(const std::string& text);
    // AppleScript string escaping (backslash and double quote).
    std::string applescript_escape(const std::string& text);

    // Runs `plan.argv` with administrator privileges (macOS asks for the
    // password). Only called after the unprivileged attempt reported a
    // permission failure.
    CapturedRun run_escalated(const std::vector<std::string>& argv);

}  // namespace luadebug::debugger::frontend
