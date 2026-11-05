// The frida backend: mapping the launcher through frida-core.
//
// macOS keeps frida out of the adapter and runs `bin/lua-debug-injector-<arch>
// frida ...` instead (see injector_helper.h), so an arm64 adapter can drive
// whichever architecture the target runs. Linux and Windows link frida-core into
// the adapter and this plan is empty, which the dispatcher runs in process.

#include "frontend/injector_backend.h"

#include <cstdlib>
#include <system_error>

#include "common/json.h"
#include "common/log.h"
#include "common/path_utf8.h"
#include "frontend/injector_helper.h"

// The per-architecture client reports this exit code only for an explicit
// permission failure (see src/injector/frida_inject.h).
namespace luadebug::debugger::frontend {

    namespace {
        constexpr int kClientPermissionDenied = 77;

        bool read_client_status(const std::string& out, std::string& status, std::string& message) {
            auto pos = out.find('{');
            if (pos == std::string::npos) {
                return false;
            }
            Json document;
            std::string parse_error;
            if (parse_json(std::string_view(out).substr(pos), document, parse_error) != ParseStatus::ok) {
                return false;
            }
            status  = std::string(get_string(document, "status"));
            message = std::string(get_string(document, "message"));
            return true;
        }
    }  // namespace

    bool make_frida_plan(const InjectionRequest& request, const InjectContext& context, InjectionPlan& plan, std::string& error) {
#if defined(__APPLE__)
        InjectorHelper helper;
        if (!resolve_injector_helper(context, request.target_arch, helper, error)) {
            return false;
        }
        plan.label = helper.label;
        plan.argv  = helper_command(helper, {
                                                "frida",
                                                "--pid",
                                                std::to_string(request.pid),
                                                "--library",
                                                request.launcher,
                                                "--data",
                                                request.data,
                                            });
        return true;
#else
        // Empty argv: the dispatcher calls frida-core in process.
        (void)context;
        plan.argv.clear();
        plan.label = "frida-core";
        return true;
#endif
    }

    InjectionOutcome interpret_frida_run(const CapturedRun& run) {
        InjectionOutcome outcome;
        if (!run.spawned) {
            outcome.error = "Spawn frida-inject failed: " + run.err;
            return outcome;
        }
        if (run.exit_code == 0) {
            outcome.ok = true;
            return outcome;
        }
        std::string status;
        std::string message;
        read_client_status(run.out, status, message);
        if (status.empty() && message.empty() && run.out.empty() && run.err.empty()) {
            // A helper that dies without a word crashed. frida-core's own
            // helper-process bootstrap does this once in a while (a SIGSEGV in
            // its DBus proxy setup, before our request is even sent); a fresh
            // helper process performs the same injection normally, so this is
            // retried rather than reported.
            outcome.retryable = run.exit_code == 11 || run.exit_code == 6 || run.exit_code == 134 || run.exit_code == 139;
            outcome.error     = "the injector helper crashed (exit " + std::to_string(run.exit_code) + ", no output)";
            return outcome;
        }
        if (run.exit_code == kClientPermissionDenied || status == "permission_denied") {
            // Only a *reported* permission failure escalates.
            outcome.permission_denied = true;
            outcome.error             = message.empty() ? std::string("Inject failed: ") + run.out + run.err : message;
            return outcome;
        }
        outcome.error = message.empty() ? ("Inject failed: " + run.out + run.err) : ("Inject failed: " + message);
        return outcome;
    }

}  // namespace luadebug::debugger::frontend
