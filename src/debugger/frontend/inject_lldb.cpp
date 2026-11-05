// The lldb backend: `dlopen` + the launcher entry point evaluated inside the
// target by a batch lldb session.
//
// This is the only way in when the injector and the target run different
// architectures (arm64 adapter, Rosetta x86_64 target): frida's agent is built
// per architecture, while lldb debugs a foreign-architecture process on the same
// OS natively.

#include "frontend/injector_backend.h"

#include <cstdlib>
#include <system_error>

#include "common/log.h"
#include "common/path_utf8.h"
#include "frontend/injector_helper.h"
#include "frontend/platform.h"
#include "frontend/spawn.h"

namespace luadebug::debugger::frontend {

    namespace {

        // lldb ships with the Command Line Tools; LUA_DEBUG_LLDB overrides the
        // location for a build machine that keeps it elsewhere.
        std::string lldb_executable() {
            if (const char* configured = std::getenv("LUA_DEBUG_LLDB")) {
                if (*configured) {
                    return configured;
                }
            }
            std::error_code ec;
            if (fs::exists("/usr/bin/lldb", ec)) {
                return "/usr/bin/lldb";
            }
            return "lldb";
        }

        // Expression text needs the payload as a C string literal. The bootstrap
        // document is single-line JSON, but a path may carry backslashes or
        // quotes on Windows-style input, so escape everything that matters.
        std::string c_string_literal(const std::string& text) {
            std::string out = "\"";
            out.reserve(text.size() + 2);
            for (char c : text) {
                switch (c) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out.push_back(c);
                    break;
                }
            }
            out.push_back('"');
            return out;
        }

        // The same escaping for a value that is already inside an lldb command
        // argument (quoted path).
        std::string c_string_literal_nested(const std::string& text) {
            std::string literal = c_string_literal(text);
            return literal.substr(1, literal.size() - 2);
        }

        // 0 for both the unload policy (the launcher keeps its image mapped and
        // checks the pointer) and the injector state.
        constexpr const char* kNullPointer = "(void*)0";

        // `process load`, not an `expression` that calls dlopen: for a Rosetta
        // target the expression evaluator cannot resolve `_dlopen` from the
        // shared cache ("Couldn't look up symbols"), while `process load` maps the
        // image through lldb's own loader and reports success.
        std::string load_command(const std::string& library) {
            if (library.find(' ') == std::string::npos && library.find('"') == std::string::npos) {
                return "process load " + library;
            }
            return "process load \"" + c_string_literal_nested(library) + "\"";
        }

        std::string entry_command(const std::string& data) {
            return std::string("expression -- ((void(*)(const char*, void*, void*))&") + kLauncherEntrypoint + ")(" + c_string_literal(data) + ", " + kNullPointer + ", " + kNullPointer + ")";
        }

        // task_for_pid refusals. Narrow on purpose: a plain "attach failed" can
        // also be a startup race, and escalating that would ask for a password
        // for nothing.
        bool reported_denied_attach(const std::string& text) {
            return text.find("Not allowed to attach") != std::string::npos || text.find("Permission denied") != std::string::npos || text.find("System Integrity Protection") != std::string::npos || text.find("task_for_pid") != std::string::npos;
        }

        // lldb installs a "utility function" in the target before it can call
        // anything; a process that has not finished dyld startup has no working
        // allocator yet, so the install fails and succeeds a moment later.
        bool reported_early_startup(const std::string& text) {
            // The Rosetta case: right after exec the kernel has not marked the
            // process translated yet, so the x86_64 debugserver refuses it
            // ("tried to attach to non-translated process"). A moment later the
            // flag is set and the same attach works.
            return text.find("utility function") != std::string::npos || text.find("non-translated process") != std::string::npos;
        }

    }  // namespace

    bool make_lldb_plan(const InjectionRequest& request, const InjectContext& context, InjectionPlan& plan, std::string& error) {
        (void)context;
#if !defined(__APPLE__)
        (void)request;
        error = "the lldb injector backend is only available on macOS";
        return false;
#else
        plan.label = "lldb";
        // lldb must run with the target's architecture, exactly like the helper:
        // an arm64 lldb cannot resolve libSystem symbols inside a Rosetta x86_64
        // process (`Couldn't look up symbols: _dlopen`), while an x86_64 lldb under
        // Rosetta debugs it like a native one. /usr/bin/lldb is universal.
        plan.argv.push_back(lldb_executable());
        // `--batch` runs the commands and exits; every step is checked by lldb,
        // so a failed `dlopen` stops the chain and the exit status is non-zero.
        plan.argv.push_back("--batch");
        plan.argv.push_back("-p");
        plan.argv.push_back(std::to_string(request.pid));
        // A failing command must not stop the session: the `detach` below has to
        // run either way, or a failed attempt leaves the target stopped and
        // traced (which makes every following attempt fail for that reason).
        plan.argv.push_back("-o");
        plan.argv.push_back("settings set interpreter.stop-command-source-on-error false");
        plan.argv.push_back("-o");
        plan.argv.push_back(load_command(request.launcher));
        plan.argv.push_back("-o");
        plan.argv.push_back(entry_command(request.data));
        plan.argv.push_back("-o");
        plan.argv.push_back("detach");
        plan.argv = run_as_target_arch(std::move(plan.argv), request.target_arch);
        return true;
#endif
    }

    bool launch_with_lldb(const LaunchRequest& request, std::string& error) {
#if !defined(__APPLE__)
        (void)request;
        error = "the lldb injector backend is only available on macOS";
        return false;
#else
        // `process launch --stop-at-entry` stops after dyld relocated the image;
        // that is the same point the preload constructor would run at, so the
        // launcher installs its hooks before the interpreter creates a state.
        std::vector<std::string> command { lldb_executable() };
        command.push_back("--batch");
        command.push_back("-o");
        command.push_back("settings set interpreter.stop-command-source-on-error false");
        command.push_back("-o");
        command.push_back("process launch --stop-at-entry");
        command.push_back("-o");
        command.push_back("process load " + request.launcher);
        command.push_back("-o");
        command.push_back(entry_command(request.data));
        command.push_back("-o");
        command.push_back("detach");
        command.push_back("--");
        command.insert(command.end(), request.argv.begin(), request.argv.end());

        SpawnOptions options;
        options.argv = run_as_target_arch(std::move(command), request.target_arch);
        options.cwd  = request.cwd;
        options.env  = request.env;

        ChildProcess child;
        if (!spawn_child(options, child, error)) {
            error = "Spawn lldb failed: " + error;
            return false;
        }
        // lldb runs the whole chain (launch �� load → entry → detach) and exits;
        // the target keeps running as lldb's reparented child. Its stdio was
        // inherited, so the debuggee's console output stayed visible and the
        // debugger's own capture takes over once the target runs.
        int exit_code = -1;
        if (!wait_child(child, exit_code, error)) {
            return false;
        }
        detach_child(child);
        if (exit_code == 0) {
            return true;
        }
        error = "lldb failed to launch and inject the target (exit " + std::to_string(exit_code) + "; see the messages above)";
        return false;
#endif
    }

    InjectionOutcome interpret_lldb_run(const CapturedRun& run) {
        InjectionOutcome outcome;
        if (!run.spawned) {
            outcome.error = "Spawn lldb failed: " + run.err;
            return outcome;
        }
        if (run.exit_code == 0) {
            outcome.ok = true;
            return outcome;
        }
        const std::string output = run.out + run.err;
        if (reported_denied_attach(output)) {
            outcome.permission_denied = true;
            outcome.error             = "Inject failed: lldb could not attach (" + output + ")";
            return outcome;
        }
        if (reported_early_startup(output)) {
            outcome.retryable = true;
            outcome.error     = "Inject failed: the target was still starting up (" + output + ")";
            return outcome;
        }
        outcome.error = "Inject failed: " + output;
        return outcome;
    }

}  // namespace luadebug::debugger::frontend
