#include "frontend/factory.h"

#include <cstdio>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "common/log.h"
#include "common/path_utf8.h"
#include "frontend/inject.h"

namespace luadebug::debugger::frontend {

    namespace {

        const std::map<std::string, std::string> kPlatformMap = {
            { "windows-x86", "win32-ia32" },
            { "windows-x86_64", "win32-x64" },
            { "linux-x86_64", "linux-x64" },
            { "linux-arm64", "linux-arm64" },
            { "android-arm64", "linux-arm64" },
            { "macos-x86_64", "darwin-x64" },
            { "macos-arm64", "darwin-arm64" },
        };

        // Port of `towsl`: drive-relative paths only, WSL only.
        std::string towsl(const PlatformState& state, const std::string& text) {
            if (!state.use_wsl || text.size() < 2 || text[1] != ':') {
                return text;
            }
            std::string out;
            out.reserve(text.size() + 6);
            for (char c : text) {
                out.push_back(c == '\\' ? '/' : c);
            }
            if (!out.empty() && (out[0] >= 'A' && out[0] <= 'Z')) {
                out[0] = static_cast<char>(out[0] - 'A' + 'a');
            }
            return "/mnt/" + out;
        }

        // Version of the *bundled interpreter* to run. This only picks an
        // executable under `runtime/<platform>/<version>/`; it is not a statement
        // about the debuggee's ABI, so a default is fine here.
        std::string bundled_interpreter_version(const Json& arguments) {
            auto value = get_string(arguments, "luaVersion");
            if (value.empty() || value == "auto") {
                return "lua54";
            }
            return std::string(value);
        }

        // The ABI constraint the user actually asked for, empty when unspecified.
        // Never defaulted: an explicit version is enforced against the detected
        // VM, so inventing one would reject every other Lua.
        std::string explicit_lua_version(const Json& arguments) {
            auto value = get_string(arguments, "luaVersion");
            if (value.empty() || value == "auto") {
                return {};
            }
            return std::string(value);
        }

        bool is_arm64_macos() {
#if defined(__APPLE__)
            FILE* pipe = popen("uname -v", "r");
            if (!pipe) {
                return false;
            }
            char buffer[512];
            std::string text;
            while (std::fgets(buffer, sizeof(buffer), pipe)) {
                text += buffer;
            }
            pclose(pipe);
            return text.find("RELEASE_ARM64") != std::string::npos;
#else
            return false;
#endif
        }

        std::string compute_arch(const Json& arguments, HostOs os) {
            auto explicit_arch = get_string(arguments, "luaArch");
            std::string arch(explicit_arch);
            if (os == HostOs::windows) {
                if (arch.empty()) {
                    arch = "x86_64";
                }
                if (arch == "x86_64" && host_arch() != "x86_64") {
                    arch = "x86";
                }
            } else if (os == HostOs::linux_) {
                if (arch.empty()) {
                    arch = "x86_64";
                }
            } else if (os == HostOs::macos) {
                if (is_arm64_macos()) {
                    if (arch.empty()) {
                        arch = "x86_64";
                    }
                    if (arch == "x86") {
                        arch = "x86_64";
                    }
                } else {
                    arch = "x86_64";
                }
            } else if (os == HostOs::other) {
                return {};
            }
            return arch;
        }

        fs::path runtime_platform_dir(const FactoryContext& context, const Json& arguments, std::string& error) {
            auto arch = compute_arch(arguments, context.platform.os);
            if (arch.empty()) {
                error = std::string("No runtime (OS: unknown) is found, you need to compile it yourself.");
                return {};
            }
            std::string key = std::string(host_os_name(host_os())) + "-" + arch;
            auto it         = kPlatformMap.find(key);
            if (it == kPlatformMap.end()) {
                error = "No runtime (OS: " + std::string(host_os_name(context.platform.os)) + ", ARCH: " + arch + ") is found, you need to compile it yourself.";
                return {};
            }
            return path_from_utf8(context.workdir) / "runtime" / it->second;
        }

        void bootstrap_cwd(const Json& arguments, const fs::path& luaexe, SpawnOptions& spawn) {
            auto cwd = get_string(arguments, "cwd");
            if (!cwd.empty()) {
                spawn.cwd = std::string(cwd);
            } else {
                spawn.cwd = path_to_utf8(luaexe.parent_path());
            }
            if (has(arguments, "env") && arguments["env"].is_object()) {
                spawn.env = arguments["env"];
            }
        }

        // Port of bootstrapMakeExe: `-e 'dofile"<script>" DBG"<address>/<...>"'`.
        void bootstrap_make_exe(std::vector<std::string>& argv, const std::string& luaexe, const Json& arguments, const std::string& address, const FactoryContext& context, const std::string& script_root) {
            argv.push_back(towsl(context.platform, luaexe));
            argv.push_back("-e");

            std::string params = address;
            if (!context.platform.use_utf8) {
                params += "/ansi";
            }
            // Every explicit version travels, not only the `lua-*` spellings: the
            // script entry validates it against the running VM, so a mismatch is
            // reported instead of silently ignored.
            const auto lua_version = explicit_lua_version(arguments);
            if (!lua_version.empty()) {
                params += "/" + lua_version;
            }

            std::string script = "dofile[[";
            script += path_to_utf8(path_from_utf8(script_root) / "script" / "launch.lua");
            script += "]] DBG[[";
            script += params;
            script += "]]";

#if !defined(_WIN32)
            // Non-Windows shells get plain quotes, exactly like the Lua version.
            for (std::size_t pos = 0; (pos = script.find("[[", pos)) != std::string::npos;) {
                script.replace(pos, 2, "\"");
            }
            for (std::size_t pos = 0; (pos = script.find("]]", pos)) != std::string::npos;) {
                script.replace(pos, 2, "\"");
            }
#endif
            argv.push_back(script);
        }

        void bootstrap_make_args(std::vector<std::string>& argv, const Json& arguments, const FactoryContext& context) {
            if (has(arguments, "arg0")) {
                const auto& arg0 = arguments["arg0"];
                if (arg0.is_string()) {
                    argv.push_back(arg0.get<std::string>());
                } else if (arg0.is_array()) {
                    for (const auto& item : arg0) {
                        if (item.is_string()) {
                            argv.push_back(item.get<std::string>());
                        }
                    }
                }
            }
            auto program = get_string(arguments, "program");
            argv.push_back(program.empty() ? std::string(".lua") : towsl(context.platform, std::string(program)));
            if (has(arguments, "arg")) {
                const auto& extra = arguments["arg"];
                if (extra.is_string()) {
                    argv.push_back(extra.get<std::string>());
                } else if (extra.is_array()) {
                    for (const auto& item : extra) {
                        if (item.is_string()) {
                            argv.push_back(item.get<std::string>());
                        }
                    }
                }
            }
        }

        Json terminal_kind(const Json& arguments) {
            auto console = get_string(arguments, "console");
            return Json(console == "integratedTerminal" ? "integrated" : "external");
        }

        void copy_terminal_common(Json& body, const Json& arguments, const std::string& cwd) {
            body["kind"]  = terminal_kind(arguments);
            body["title"] = std::string(get_string(arguments, "name"));
            body["cwd"]   = cwd;
            if (has(arguments, "env") && arguments["env"].is_object()) {
                body["env"] = arguments["env"];
            }
        }

    }  // namespace

    void factory_initialize(const Json& arguments, PlatformState& state) {
        init_platform(arguments, state);
    }

    bool resolve_lua_exe(const Json& arguments, const FactoryContext& context, std::string& path, std::string& error) {
        auto explicit_exe = get_string(arguments, "luaexe");
        if (!explicit_exe.empty()) {
            std::error_code ec;
            auto candidate = path_from_utf8(explicit_exe);
            if (fs::exists(candidate, ec)) {
                path = path_to_utf8(candidate);
                return true;
            }
#if defined(_WIN32)
            if (!candidate.has_extension()) {
                auto with_ext = candidate;
                with_ext.replace_extension("exe");
                if (fs::exists(with_ext, ec)) {
                    path = path_to_utf8(with_ext);
                    return true;
                }
            }
#endif
            error = "No file `" + std::string(explicit_exe) + "`.";
            return false;
        }

        auto dir = runtime_platform_dir(context, arguments, error);
        if (dir.empty()) {
            return false;
        }
        auto version = bundled_interpreter_version(arguments);
        auto luaexe  = dir / version / (host_os() == HostOs::windows ? "lua.exe" : "lua");
        std::error_code ec;
        if (fs::exists(luaexe, ec)) {
            path = path_to_utf8(luaexe);
            return true;
        }
        error = "No runtime (" + path_to_utf8(luaexe) + ") is found, you need to compile it yourself.";
        return false;
    }

    bool create_luaexe_in_console(const Json& arguments, const FactoryContext& context, const std::string& address, ConsolePlan& plan, std::string& error) {
        std::string luaexe;
        if (!resolve_lua_exe(arguments, context, luaexe, error)) {
            return false;
        }
        plan.luaexe = luaexe;
        plan.spawn.argv.clear();

        if (context.platform.use_wsl) {
            plan.spawn.argv.push_back("wsl");
        }
        bootstrap_cwd(arguments, path_from_utf8(luaexe), plan.spawn);
        // A plain interpreter launch cannot be injected, so the VM source travels
        // in the environment and `debugger.lua` opens the bridge through the
        // launcher's own entry. Nothing is added for a launch without a source.
        if (!apply_vm_source_env(arguments, InjectContext { context.workdir, context.platform }, plan.spawn.env, error)) {
            return false;
        }
        bootstrap_make_exe(plan.spawn.argv, luaexe, arguments, address, context, context.workdir);
        bootstrap_make_args(plan.spawn.argv, arguments, context);
        plan.need_resume = false;
        return true;
    }

    bool create_luaexe_in_terminal(const Json& /*init_request*/, const Json& arguments, const FactoryContext& context, const std::string& address, TerminalPlan& plan, std::string& error) {
        std::string luaexe;
        if (!resolve_lua_exe(arguments, context, luaexe, error)) {
            return false;
        }
        std::vector<std::string> argv;
        if (context.platform.use_wsl) {
            argv.push_back("wsl");
        }
        bootstrap_make_exe(argv, luaexe, arguments, address, context, context.workdir);
        bootstrap_make_args(argv, arguments, context);

        Json body = Json::object();
        copy_terminal_common(body, arguments, get_string(arguments, "cwd").empty() ? path_to_utf8(path_from_utf8(luaexe).parent_path()) : std::string(get_string(arguments, "cwd")));
        {
            // `runInTerminal` carries the same environment overrides as the
            // console spawn, so the source reaches the target there too.
            Json env = body.contains("env") ? body["env"] : Json::object();
            if (!apply_vm_source_env(arguments, InjectContext { context.workdir, context.platform }, env, error)) {
                return false;
            }
            if (body.contains("env") || !env.empty()) {
                body["env"] = std::move(env);
            }
        }
        Json args = Json::array();
        for (const auto& item : argv) {
            args.push_back(item);
        }
        body["args"]   = std::move(args);
        plan.arguments = std::move(body);
        return true;
    }

    bool create_process_in_console(const Json& arguments, const FactoryContext& context, const LaunchInjection& injection, SpawnOptions& spawn, std::string& error) {
        auto runtime_executable = get_string(arguments, "runtimeExecutable");
        if (runtime_executable.empty()) {
            error = "`runtimeExecutable` is required.";
            return false;
        }
        spawn.argv.push_back(std::string(runtime_executable));
        if (has(arguments, "runtimeArgs")) {
            const auto& extra = arguments["runtimeArgs"];
            if (extra.is_string()) {
                spawn.argv.push_back(extra.get<std::string>());
            } else if (extra.is_array()) {
                for (const auto& item : extra) {
                    if (item.is_string()) {
                        spawn.argv.push_back(item.get<std::string>());
                    }
                }
            }
        }
        if (has(arguments, "env") && arguments["env"].is_object()) {
            spawn.env = arguments["env"];
        }
        auto cwd  = get_string(arguments, "cwd");
        spawn.cwd = cwd.empty()
                        ? path_to_utf8(path_from_utf8(runtime_executable).parent_path())
                        : std::string(cwd);
        if (injection.enabled && injection.preload) {
            if (!make_preload_env(InjectEntry::launch, injection.launcher, injection.config, true, spawn.env, error)) {
                return false;
            }
        }
        (void)context;
        return true;
    }

    bool create_process_in_terminal(const Json& init_request, const Json& arguments, const FactoryContext& context, TerminalPlan& plan, std::string& error) {
        auto runtime_executable = get_string(arguments, "runtimeExecutable");
        if (runtime_executable.empty()) {
            error = "`runtimeExecutable` is required.";
            return false;
        }
        Json body = Json::object();
        auto cwd  = get_string(arguments, "cwd");
        copy_terminal_common(body, arguments, cwd.empty() ? path_to_utf8(path_from_utf8(runtime_executable).parent_path()) : std::string(cwd));

        Json args = Json::array();
        if (context.platform.use_wsl) {
            args.push_back("wsl");
        }
        args.push_back(std::string(runtime_executable));
        bool runtime_args_is_string = false;
        if (has(arguments, "runtimeArgs")) {
            const auto& extra = arguments["runtimeArgs"];
            if (extra.is_string()) {
                runtime_args_is_string = true;
                args.push_back(extra.get<std::string>());
            } else if (extra.is_array()) {
                for (const auto& item : extra) {
                    if (item.is_string()) {
                        args.push_back(item.get<std::string>());
                    }
                }
            }
        }
        body["args"] = std::move(args);
        // `supportsArgsCanBeInterpretedByShell` only matters when runtimeArgs is a
        // single string; the client capability travels in the cached initialize
        // request, so report it the same way the Lua frontend did.
        bool supports_args_can_be_interpreted_by_shell = false;
        if (init_request.is_object() && init_request.contains("arguments")) {
            const auto& init_args = init_request["arguments"];
            supports_args_can_be_interpreted_by_shell =
                get_bool(init_args, "supportsArgsCanBeInterpretedByShell", false);
        }
        body["argsCanBeInterpretedByShell"] =
            supports_args_can_be_interpreted_by_shell && runtime_args_is_string;
        plan.arguments = std::move(body);
        return true;
    }

}  // namespace luadebug::debugger::frontend
