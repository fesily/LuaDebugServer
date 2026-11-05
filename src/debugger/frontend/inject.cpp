#include "frontend/inject.h"

#include <chrono>

#if defined(__APPLE__)
#    include <libproc.h>
#endif
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/log.h"
#include "common/path_utf8.h"
#include "frontend/injector_helper.h"
#include "frontend/spawn.h"
#include "frontend/temp_symbols.h"

#if defined(__APPLE__)
#    include <libproc.h>
#    include <signal.h>
#    include <sys/proc.h>
#    include <sys/sysctl.h>
#    include <sys/types.h>
#    include <unistd.h>
#elif !defined(_WIN32)
#    include <signal.h>
#    include <sys/types.h>
#    include <sys/utsname.h>
#    include <unistd.h>
#endif

// macOS injects through the per-architecture client (deps are one shim), every
// other platform links frida-core into the adapter.
#if !defined(__APPLE__)
#    include "injector/frida_inject.h"
#endif

#include "frontend/injector_backend.h"

namespace luadebug::debugger::frontend {

    namespace {

#if !defined(__APPLE__)
        // Same value the frida client uses for "escalate and retry" (see
        // src/injector/frida_inject.h); the in-process path reports it the same way.
        constexpr int kClientPermissionDenied = 77;
#endif

        // A failed attach leaves the target stopped and traced (lldb's batch
        // session exits at the first failing command). Resume it, or the next
        // attempt attaches to a stopped process and fails for that reason.
        void resume_after_failed_attach(long pid) {
#if !defined(_WIN32)
            if (pid > 0) {
                ::kill(static_cast<pid_t>(pid), SIGCONT);
            }
#else
            (void)pid;
#endif
        }

        // How long ago the target started, in seconds (negative when unknown).
        //
        // Attaching to a *translated* (Rosetta) process while it is still inside
        // dyld initialization kills it: the kernel raises SIGSYS on dyld's own
        // cache probe (`dyld4::SyscallDelegate::hasExistingDyldCache`). Measured
        // on this machine: attach 50 ms after spawn -> 10/10 targets died with
        // -12, attach after 800 ms -> 0/10. So a fresh target gets a grace period
        // before the first attach instead of burning retries on a process that
        // dies under them.
        double process_age_seconds(long pid) {
#if defined(__APPLE__)
            struct proc_bsdinfo info {};
            const int size = proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
            if (size != static_cast<int>(sizeof(info))) {
                return -1.0;
            }
            // The target's own start time, in the target's clock: both sides of
            // this comparison are on the same machine.
            const double started = static_cast<double>(info.pbi_start_tvsec) + static_cast<double>(info.pbi_start_tvusec) / 1.0e6;
            struct timeval now {};
            ::gettimeofday(&now, nullptr);
            const double current = static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_usec) / 1.0e6;
            return current - started;
#else
            (void)pid;
            return -1.0;
#endif
        }

        // A retry only makes sense while the target is alive; a debuggee that
        // already exited must not turn into a few seconds of pointless attempts.
        //
        // `kill(pid, 0)` is not enough on macOS: a child whose parent never
        // reaped it stays visible as a zombie, and attaching to a zombie fails
        // with lldb's confusing "non-translated process using Rosetta
        // debugserver" - which is exactly what the retry loop then repeats.
        bool process_exists(long pid) {
#if defined(__APPLE__)
            int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid) };
            struct kinfo_proc info {};
            std::size_t size = sizeof(info);
            if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size != sizeof(info)) {
                return false;
            }
            return info.kp_proc.p_stat != SZOMB;
#elif !defined(_WIN32)
            return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0;
#else
            (void)pid;
            return true;
#endif
        }

        // A freshly spawned target needs a few hundred milliseconds to get past
        // dyld init, where lldb cannot install its utility function yet. The
        // retries back off (100 ms .. 600 ms, ~14 s in total) so a cold Rosetta
        // lldb start is covered without hammering the target.
        constexpr int kStartupRetries = 40;
        constexpr std::chrono::milliseconds kStartupRetryDelayMin { 100 };
        constexpr std::chrono::milliseconds kStartupRetryDelayMax { 600 };
        constexpr std::chrono::milliseconds kColdStartDelay { 150 };
        // dyld has finished by then even on a busy machine (see
        // process_age_seconds for the measurements).
        constexpr std::chrono::milliseconds kFreshTargetGrace { 1200 };

        // The pre-frida strategies needed a debugger executable or shellcode;
        // `lldb` is back as a *backend* (see injector_backend.h), not as a mode.
        bool is_legacy_inject_mode(const std::string& mode) {
            return mode == "gdb" || mode == "hook";
        }

        // ---- bootstrap document ------------------------------------------------

        // Target-side `client` is the opposite of the adapter-side one: the DAP
        // argument says who connects to whom from the adapter's point of view,
        // and the launcher needs the target's point of view.
        bool target_client_mode(const Json& arguments) {
            return !get_bool(arguments, "client", false);
        }

        // The published runtime directories are `runtime/<platform>/<lua version>`
        // (see factory.cpp's kPlatformMap): x86_64 is published as `x64` and
        // Windows x86 as `ia32`. `debugger.lua` uses this field verbatim to pick a
        // bridge, so an unmapped name there means no bridge at all.
        //
        // An unknown architecture yields no value: the target's own launcher then
        // detects the platform from the image it runs in. Guessing `arm64` would
        // make a Rosetta target load the wrong bridge.
        std::string bridge_platform(HostOs os, const std::string& lua_arch) {
            if (lua_arch.empty()) {
                return {};
            }
            switch (os) {
            case HostOs::macos:
                return std::string("darwin-") + (lua_arch == "x64" ? "x64" : "arm64");
            case HostOs::windows:
                return std::string("win32-") + (lua_arch == "x86" ? "ia32" : "x64");
            case HostOs::linux_:
                return std::string("linux-") + (lua_arch == "arm64" ? "arm64" : "x64");
            default:
                return {};
            }
        }

        // ---- VM source arguments ------------------------------------------------

        // An optional string field of the external VM source. Absent means "not
        // provided"; a present field must be usable, because an empty path or a
        // wrong type would otherwise reach the target as "nothing" and silently
        // fall back to discovery.
        bool read_source_string(const Json& arguments, const char* key, std::string& out, std::string& error) {
            if (!has(arguments, key)) {
                return true;
            }
            const auto& value = arguments[key];
            if (!value.is_string()) {
                error = std::string("`") + key + "` must be a string";
                return false;
            }
            std::string text = value.get<std::string>();
            if (text.empty()) {
                error = std::string("`") + key + "` must not be empty";
                return false;
            }
            if (text.find('\0') != std::string::npos) {
                error = std::string("`") + key + "` must not contain NUL";
                return false;
            }
            out = std::move(text);
            return true;
        }

        // A relative `luaSymbolsFile` is relative to the request, like every other
        // user path in a launch config: the request cwd, then the workspace, then
        // the adapter's own workdir. The document must carry an absolute path
        // because the target opens the file with *its* working directory, which is
        // not ours.
        fs::path resolve_request_path(const std::string& text, const Json& arguments, const std::string& workdir) {
            fs::path path = path_from_utf8(text);
            if (path.is_absolute()) {
                return path.lexically_normal();
            }
            const std::string cwd       = std::string(get_string(arguments, "cwd"));
            const std::string workspace = std::string(get_string(arguments, "workspaceFolder"));
            fs::path base;
            if (!cwd.empty()) {
                base = path_from_utf8(cwd);
            } else if (!workspace.empty()) {
                base = path_from_utf8(workspace);
            } else if (!workdir.empty()) {
                base = path_from_utf8(workdir);
            }
            if (base.empty()) {
                std::error_code ec;
                base = fs::current_path(ec);
            }
            std::error_code ec;
            auto absolute = fs::absolute(base / path, ec);
            return (ec ? (base / path) : absolute).lexically_normal();
        }

        // A loader preload list cannot carry spaces or `:` separators (both
        // `LD_PRELOAD` and `DYLD_INSERT_LIBRARIES` split on them and neither
        // supports escaping). When the published tree lives under such a path,
        // preload a symlink from the (separator-free) temp directory instead; the
        // bootstrap document keeps the real root.
        long adapter_pid() {
#if defined(_WIN32)
            return static_cast<long>(GetCurrentProcessId());
#else
            return static_cast<long>(getpid());
#endif
        }

        bool preload_image_path(const std::string& launcher, std::string& path, std::string& error) {
            const bool needs_alias = launcher.find_first_of(" :") != std::string::npos;
            if (!needs_alias) {
                path = launcher;
                return true;
            }
            std::error_code ec;
            auto temp = fs::temp_directory_path(ec);
            if (ec) {
                error = "cannot locate a temp directory for the preload alias";
                return false;
            }
            const auto temp_text = path_to_utf8(temp);
            if (temp_text.find_first_of(" :") != std::string::npos) {
                error = "the published path contains spaces, and the temp directory `" + temp_text + "` cannot be used as a preload alias either";
                return false;
            }
            auto alias = temp / ("lua-debug-launcher-" + std::to_string(adapter_pid()) + ".so");
            fs::remove(alias, ec);
            fs::create_symlink(path_from_utf8(launcher), alias, ec);
            if (ec) {
                error = "cannot create the preload alias `" + path_to_utf8(alias) + "`: " + ec.message();
                return false;
            }
            path = path_to_utf8(alias);
            return true;
        }

        // ---- macOS client invocation ------------------------------------------

#if defined(__APPLE__)

        // Rosetta processes carry P_TRANSLATED. The adapter's own architecture
        // must never decide this: a fat executable may hold both slices while
        // the target runs only one of them.
        bool target_is_translated(long pid) {
            int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid) };
            struct kinfo_proc info;
            std::memset(&info, 0, sizeof(info));
            std::size_t size = sizeof(info);
            if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size != sizeof(info)) {
                return false;
            }
            constexpr int kPTranslated = 0x00020000;
            return (info.kp_proc.p_flag & kPTranslated) != 0;
        }

#endif

    }  // namespace

    bool resolve_inject_mode(const Json& arguments, std::string& mode, std::string& error) {
        auto configured = std::string(get_string(arguments, "inject"));
        if (!configured.empty()) {
            if (is_legacy_inject_mode(configured)) {
                error = "`inject` value `" + configured + "` is no longer supported; use `frida` (or `none`).";
                return false;
            }
            if (configured != "frida" && configured != "lldb" && configured != "auto" && configured != "none") {
                error = "unknown `inject` value `" + configured + "`; use `frida`, `lldb`, `auto` or `none`.";
                return false;
            }
            mode = configured;
            return true;
        }
        // Defaults: an address-only session is not ours to inject into; every
        // other case (attach by pid/name, runtimeExecutable launch) injects.
        mode = has(arguments, "address") ? "none" : "frida";
        return true;
    }

    bool launcher_path(const InjectContext& context, const std::string& lua_arch, std::string& path, std::string& error) {
        auto bin = path_from_utf8(context.workdir) / "bin";
        fs::path candidate;
        if (host_os() == HostOs::windows) {
            // A 32-bit adapter drives 32-bit and 64-bit targets with the matching
            // launcher, so the target architecture selects the file.
            candidate = bin / (lua_arch == "x86_64" ? "launcher.x64.dll" : "launcher.x86.dll");
        } else {
            candidate = bin / "launcher.so";
        }
        std::error_code ec;
        if (!fs::exists(candidate, ec)) {
            error = "Not found " + path_to_utf8(candidate.filename()) + ".";
            return false;
        }
        path = path_to_utf8(candidate);
        return true;
    }

    bool make_bootstrap_config(long pid, InjectEntry entry, const Json& arguments, const InjectContext& context, const std::string& lua_arch, BootstrapConfig& out, std::string& error) {
        (void)pid;
        BootstrapConfig config;
        config.mode = entry == InjectEntry::launch ? BootstrapMode::launch : BootstrapMode::attach;
        // The launcher derives "@$tmp/luadbg_<pid>" when this is empty.
        config.address = std::string(get_string(arguments, "address"));
        if (config.address.empty()) {
            // The adapter always connects to the pid socket
            // (<workdir>/tmp defaults), so the target has to listen there
            // regardless of what the client prefers for an explicit address.
            config.client = false;
        } else {
            config.client = target_client_mode(arguments);
        }
        // Only an explicit version travels: the launcher must let the target VM
        // decide otherwise. A present-but-unusable value is an error rather than
        // "absent" - `get_string` answers "" for a non-string, which would silently
        // downgrade a constraint the user asked for into auto-detection.
        if (has(arguments, "luaVersion")) {
            const auto& version = arguments["luaVersion"];
            if (!version.is_string()) {
                error = "`luaVersion` must be a string";
                return false;
            }
            const std::string text = version.get<std::string>();
            if (text.empty()) {
                error = "`luaVersion` must not be empty (omit it to detect the ABI)";
                return false;
            }
            if (text.find('\0') != std::string::npos) {
                error = "`luaVersion` must not contain NUL";
                return false;
            }
            config.lua_version = text;
        }
        config.ansi     = !context.platform.use_utf8;
        config.platform = bridge_platform(context.platform.os, lua_arch);
        // A frida-injected image has no dyld path, so dladdr() cannot locate the
        // published tree; the root travels with the config.
        config.root = context.workdir;

        // ---- optional VM source ----------------------------------------------
        // The source is resolved inside the *target*, and every carrier for it is a
        // target-side path or an environment variable. Under WSL neither travels (the
        // host path is meaningless to the Linux interpreter and `wsl.exe` does not
        // forward our variables to it), so the request is refused instead of being
        // half-applied.
        if (context.platform.use_wsl && has_vm_source(arguments)) {
            error =
                "`luaBinary`/`luaSymbols` are not supported with `useWSL`: the symbol "
                "source is resolved inside the target, whose paths and environment are "
                "not the ones this adapter can set";
            return false;
        }
        // The three fields are independent, but none of them is guessed at: a
        // value that is present and unusable fails here rather than degrading to
        // discovery inside the target.
        if (!read_source_string(arguments, "luaBinary", config.lua_binary, error)) {
            return false;
        }
        if (has(arguments, "luaSymbols") && has(arguments, "luaSymbolsFile")) {
            error = "`luaSymbols` and `luaSymbolsFile` are mutually exclusive; name the table once";
            return false;
        }
        if (has(arguments, "luaSymbols")) {
            const auto& table = arguments["luaSymbols"];
            if (!table.is_object()) {
                error = "`luaSymbols` must be an object";
                return false;
            }
            // An empty object means "not provided" (see parse_bootstrap): it is
            // the same contract both carriers read, so the frontend must not
            // disagree with the launcher about it.
            if (!parse_symbols(table, config.lua_symbols, error)) {
                return false;
            }
        }
        if (!read_source_string(arguments, "luaSymbolsFile", config.lua_symbols_file, error)) {
            return false;
        }
        if (!config.lua_symbols_file.empty()) {
            config.lua_symbols_file = path_to_utf8(
                resolve_request_path(config.lua_symbols_file, arguments, context.workdir)
            );
        }

        // Both carriers are capped at kMaxBootstrapBytes (see bootstrap.h). A
        // table that does not fit moves to a session temp file the target reads,
        // so size never turns a valid source into an undeliverable config. When
        // the document without the table is already undeliverable, the table is
        // not the reason and moving it would only hide the real failure.
        if (!config.lua_symbols.empty() && dump_bootstrap(config).empty()) {
            BootstrapConfig without_table = config;
            without_table.lua_symbols.clear();
            if (!dump_bootstrap(without_table).empty()) {
                std::string path;
                if (!temp_symbols_file(config.lua_symbols, path, error)) {
                    return false;
                }
                config.lua_symbols.clear();
                config.lua_symbols_file = std::move(path);
            }
        }

        out = std::move(config);
        error.clear();
        return true;
    }

    bool has_vm_source(const Json& arguments) {
        if (has(arguments, "luaBinary") || has(arguments, "luaSymbolsFile")) {
            return true;
        }
        // An empty table carries nothing and the bootstrap parser treats it as "not
        // provided"; counting the key here would make the two disagree.
        return has(arguments, "luaSymbols") && arguments["luaSymbols"].is_object() && !arguments["luaSymbols"].empty();
    }

    bool validate_vm_source_usage(const Json& arguments, const char* request, std::string& error) {
        if (!has_vm_source(arguments)) {
            return true;
        }
        const bool attach = request && std::string_view(request) == "attach";
        // An attach by address joins a session that already exists: nothing is
        // injected, so a source in this request would be silently ignored while the
        // user believes the table was used.
        if (attach && has(arguments, "address")) {
            error =
                "`luaBinary`/`luaSymbols` do not apply to an attach by address: the "
                "session joins a VM that is already running and cannot rebind it. "
                "Use `processId` to inject, or drop the source";
            return false;
        }
        std::string mode;
        if (!resolve_inject_mode(arguments, mode, error)) {
            return false;
        }
        // Two shapes can deliver the document without an injector: a `luaexe` launch
        // (the adapter spawns the interpreter and adds the environment carrier) and
        // any request that injects (the document rides in the payload). Everything
        // else would drop it - e.g. a `runtimeExecutable` launch with
        // `inject: "none"`, whose spawn gets neither the payload nor the env.
        const bool injects                     = mode != "none";
        const bool adapter_spawned_interpreter = !attach && !has(arguments, "runtimeExecutable");
        if (!injects && !adapter_spawned_interpreter) {
            error =
                "`luaBinary`/`luaSymbols` cannot reach the target with `inject: \"none\"` "
                "for this request: nothing is injected and the adapter does not start the "
                "interpreter, so there is no carrier for the source. Use `inject: "
                "\"frida\"`, a `luaexe` launch, or drop the source";
            return false;
        }
        return true;
    }

    bool apply_vm_source_env(const Json& arguments, const InjectContext& context, Json& env, std::string& error) {
        BootstrapConfig config;
        // No target process exists yet, and a plain `luaexe` launch is the very
        // interpreter we start: its own detection picks the published runtime, so
        // no architecture is claimed here.
        if (!make_bootstrap_config(0, InjectEntry::launch, arguments, context, std::string(), config, error)) {
            return false;
        }
        if (!config.has_vm_source()) {
            // A launch without a source keeps the plain `debugger.lua` path; the
            // document (and its marker) must not be set for it.
            return true;
        }
        const auto document = dump_bootstrap(config);
        if (document.empty()) {
            error =
                "the VM source cannot be delivered to the target: the bootstrap document "
                "does not fit in " +
                std::to_string(kMaxBootstrapBytes) + " bytes";
            return false;
        }
        if (!env.is_object()) {
            env = Json::object();
        }
        // The adapter owns `LUA_DEBUG_CONFIG`: a user value would name a session
        // this launch is not, so it is replaced. The marker is what makes
        // `debugger.lua` enter through the launcher instead of the target's
        // exported symbols.
        env["LUA_DEBUG_CONFIG"]    = document;
        env["LUA_DEBUG_VM_SOURCE"] = "1";
        return true;
    }

    bool detect_target_arch(long pid, std::string& lua_arch, std::string& error) {
#if defined(__APPLE__)
        // The target's own Mach-O header is authoritative and always readable: it
        // answers for a process this adapter does not own, which `P_TRANSLATED`
        // does not (and a target started by the client, or by a shell, is exactly
        // that). Without it an arm64 host inspecting an x86_64 target assumed
        // "native arm64", looked for the arm64 injector helper and failed on a
        // distribution that ships the x64 one.
        if (pid > 0) {
            char image[4096] {};
            if (::proc_pidpath(static_cast<int>(pid), image, sizeof(image)) > 0) {
                const std::string arch = executable_arch(image);
                if (!arch.empty()) {
                    lua_arch = arch;
                    return true;
                }
            }
        }
        // Fallback: P_TRANSLATED marks a Rosetta process, and its absence means
        // "not translated", which is arm64 only on an arm64 host.
        if (host_arch() == "x86_64") {
            lua_arch = "x64";
        } else {
            lua_arch = target_is_translated(pid) ? "x64" : "arm64";
        }
        return true;
#elif defined(_WIN32)
        // A 32-bit adapter drives 32- and 64-bit targets, with a launcher per
        // width, so the target's own bitness decides. IsWow64Process2 reports the
        // *process* machine even from a WOW64 caller; the older API is the
        // fallback (from a 32-bit caller it reports whether the target runs under
        // WOW64, i.e. is 32-bit).
        if (pid > 0) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
            if (process) {
                USHORT process_machine = 0;
                USHORT native_machine  = 0;
                auto is_wow64_process2 = reinterpret_cast<BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*)>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2")
                );
                bool resolved = false;
                if (is_wow64_process2 && is_wow64_process2(process, &process_machine, &native_machine)) {
                    if (process_machine != IMAGE_FILE_MACHINE_UNKNOWN) {
                        lua_arch = process_machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "x86_64";
                        resolved = true;
                    } else {
                        lua_arch = native_machine == IMAGE_FILE_MACHINE_I386 ? "x86" : "x86_64";
                        resolved = true;
                    }
                } else {
                    BOOL wow64 = FALSE;
                    if (IsWow64Process(process, &wow64)) {
                        lua_arch = wow64 ? "x86" : "x86_64";
                        resolved = true;
                    }
                }
                CloseHandle(process);
                if (resolved) {
                    return true;
                }
            }
        }
        error = "cannot detect the architecture of process " + std::to_string(pid);
        return false;
#else
        (void)pid;
        struct utsname info;
        if (uname(&info) != 0) {
            error = "cannot detect the target architecture";
            return false;
        }
        lua_arch = std::string_view(info.machine) == "aarch64" ? "arm64" : "x86_64";
        return true;
#endif
    }

    bool make_preload_env(InjectEntry entry, const std::string& launcher, const BootstrapConfig& config, bool preload_supported, Json& env, std::string& error) {
        if (!preload_supported) {
            error = "preload is not used on this platform";
            return false;
        }
        auto json = dump_bootstrap(config);
        if (json.empty()) {
            error = "bootstrap config cannot be delivered (invalid field or too large)";
            return false;
        }
        std::string image;
        if (!preload_image_path(launcher, image, error)) {
            return false;
        }
        if (!env.is_object()) {
            env = Json::object();
        }
#if defined(_WIN32)
        error = "preload is not used on this platform";
        return false;
#else
        const char* key =
#    if defined(__APPLE__)
            "DYLD_INSERT_LIBRARIES";
#    else
            "LD_PRELOAD";
#    endif
        // A user-provided (or inherited) preload list is kept: replacing it
        // silently changes what the target loads.
        std::string existing;
        if (env.contains(key) && env[key].is_string()) {
            existing = env[key].get<std::string>();
        } else if (!env.contains(key)) {
            if (const char* inherited = std::getenv(key)) {
                existing = inherited;
            }
        }
        env[key] = existing.empty() ? image : (existing + ":" + image);
#endif
        // The preload constructor reads exactly this carrier, and only acts when
        // the adapter asked for a preload launch.
        env["LUA_DEBUG_PRELOAD"] = "1";
        env["LUA_DEBUG_CONFIG"]  = json;
        (void)entry;
        return true;
    }

    bool inject_process(long pid, InjectEntry entry, const Json& arguments, const InjectContext& context, std::string& error) {
        std::string lua_arch;
        if (!detect_target_arch(pid, lua_arch, error)) {
            return false;
        }
        std::string launcher;
        if (!launcher_path(context, lua_arch, launcher, error)) {
            return false;
        }

        BootstrapConfig config;
        if (!make_bootstrap_config(pid, entry, arguments, context, lua_arch, config, error)) {
            return false;
        }
        const auto data = dump_bootstrap(config);
        if (data.empty()) {
            error = "bootstrap config is too large to deliver through the injector";
            return false;
        }

        log_info("frontend: inject pid %ld with %s (%s)", pid, launcher.c_str(), data.c_str());

        InjectionRequest request;
        request.pid         = pid;
        request.launcher    = launcher;
        request.data        = data;
        request.target_arch = lua_arch;
        request.entry       = entry;

        InjectorBackend backend = InjectorBackend::frida;
        bool automatic          = false;
        if (!resolve_injector_backend(arguments, lua_arch, backend, automatic, error)) {
            return false;
        }
        if (automatic) {
            log_info("frontend: injector backend %s (default for a %s target)", injector_backend_name(backend), lua_arch.empty() ? "unknown-arch" : lua_arch.c_str());
        }

        InjectionPlan plan;
        switch (backend) {
        case InjectorBackend::lldb:
            if (!make_lldb_plan(request, context, plan, error)) {
                return false;
            }
            break;
        case InjectorBackend::frida:
        default:
            if (!make_frida_plan(request, context, plan, error)) {
                return false;
            }
            break;
        }

        {
            std::string command;
            for (const auto& item : plan.argv) {
                if (!command.empty()) {
                    command.push_back(' ');
                }
                command += item;
            }
            log_info("frontend: %s injector command: %s", plan.label.c_str(), command.empty() ? "(in process)" : command.c_str());
        }

        auto run = [&](bool escalate) {
            if (escalate) {
                return run_escalated(plan.argv);
            }
#if !defined(__APPLE__)
            if (plan.argv.empty()) {
                // In-process frida-core (Linux/Windows): the same request, one call.
                auto result = injector::inject_library(static_cast<unsigned int>(pid), launcher, kLauncherEntrypoint, data);
                CapturedRun captured;
                captured.spawned   = true;
                captured.exit_code = result.status == injector::Status::ok                  ? 0
                                     : result.status == injector::Status::permission_denied ? kClientPermissionDenied
                                                                                            : 1;
                if (result.status == injector::Status::permission_denied) {
                    captured.out = "{\"status\":\"permission_denied\"}";
                } else if (captured.exit_code != 0) {
                    captured.out = result.error_message;
                }
                return captured;
            }
#endif
            return run_captured(plan.argv);
        };

        auto outcome = [&](const CapturedRun& captured) {
            return backend == InjectorBackend::lldb ? interpret_lldb_run(captured)
                                                    : interpret_frida_run(captured);
        };

        // A target that is still inside dyld startup cannot host lldb's utility
        // function yet: retry the same command briefly instead of failing an
        // attach that would succeed a moment later.
        if (backend == InjectorBackend::lldb && request.entry == InjectEntry::attach) {
            // A process that has not left dyld initialization yet dies if lldb
            // attaches to it (see process_age_seconds). Only a *fresh* target
            // waits; attaching to a long-running one starts immediately.
            const double age   = process_age_seconds(request.pid);
            const double grace = kFreshTargetGrace.count() / 1000.0;
            if (age >= 0.0 && age < grace) {
                log_info("frontend: target started %.0f ms ago, waiting %.0f ms before attaching", age * 1000.0, (grace - age) * 1000.0);
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>((grace - age) * 1000.0)));
            } else {
                std::this_thread::sleep_for(kColdStartDelay);
            }
        }

        InjectionOutcome first;
        for (int attempt = 0;; ++attempt) {
            auto attempt_run = run(false);
            first            = outcome(attempt_run);
            if (!first.ok && first.retryable) {
                const bool last = attempt >= kStartupRetries;
                log_info("frontend: injector attempt %d%s: %s", attempt + 1, last ? " (last)" : "", first.error.substr(0, last ? 2000 : 300).c_str());
            }
            if (first.ok || !first.retryable || attempt >= kStartupRetries) {
                break;
            }
            // A failed attach leaves the target stopped and traced: lldb's batch
            // session exits at the first failing command, so its `detach` never
            // ran. Without this the next attempt attaches to a stopped process
            // and fails for a different reason, forever.
            if (!process_exists(request.pid)) {
                error = std::string("the target exited before the ") + injector_backend_name(backend) + " injector could load the launcher";
                return false;
            }
            resume_after_failed_attach(request.pid);
            const auto step = kStartupRetryDelayMin * (1 + attempt / 8);
            std::this_thread::sleep_for(step > kStartupRetryDelayMax ? kStartupRetryDelayMax : step);
        }
        if (first.ok) {
            return true;
        }
        if (!first.permission_denied) {
            error = first.error;
            return false;
        }

        // Only a *reported* permission failure escalates, and only this one
        // injection is retried: the same document travels again, so the
        // escalated run cannot rebuild a different configuration.
        log_info("frontend: injection needs administrator privileges, retrying");
        auto escalated = outcome(run(true));
        if (escalated.ok) {
            return true;
        }
        error = escalated.error;
        return false;
    }

    std::string shell_quote(const std::string& text) {
        std::string out = "'";
        for (char c : text) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    }

    std::string applescript_escape(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c == '\\' || c == '"') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out;
    }

    CapturedRun run_escalated(const std::vector<std::string>& argv) {
#if defined(__APPLE__)
        std::string line;
        for (const auto& item : argv) {
            if (!line.empty()) {
                line.push_back(' ');
            }
            line += shell_quote(item);
        }
        const std::string script = "do shell script \"" + applescript_escape(line) + "\" with administrator privileges with prompt \"lua-debug\"";
        return run_captured({ "/usr/bin/osascript", "-e", script });
#else
        (void)argv;
        CapturedRun run;
        run.err = "escalation is only implemented for macOS";
        return run;
#endif
    }

}  // namespace luadebug::debugger::frontend
