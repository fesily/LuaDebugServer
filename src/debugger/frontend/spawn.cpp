// Process helpers.
//   * start a debuggee and keep it running (console launch),
//   * spawn through frida when the launch needs a pre-main gate (Windows),
//   * run a short helper (the inject clients, osascript, fuser) and capture its
//     output.
//
// The native owner is Boost.Process v2. Two of its behaviours differ from the
// previous wrapper and are handled explicitly here:
//   * a `basic_process` destructor terminates a child that was not detached, so
//     the owner-state destructor detaches instead (only terminate_child kills),
//   * pipes are real Asio pipes, so a captured run drains them with coroutines on
//     its own io_context instead of reading them one after the other.

#include "frontend/spawn.h"

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/path_utf8.h"
#include "transport/reactor.h"

#if defined(_WIN32)
#    include <windows.h>

#    include <boost/process/v2/windows/creation_flags.hpp>
#else
#    include <unistd.h>

#    include <csignal>

extern "C" char** environ;
#endif

#if defined(_WIN32)
// Windows launches need a pre-main gate, which only frida's local device
// provides (CREATE_SUSPENDED inside its helper). macOS/Linux preload the launcher
// instead, so they never touch a frida device here.
#    include <frida-core.h>

#    include <deque>
#    include <mutex>
#    include <utility>
#endif

namespace luadebug::debugger::frontend {

    namespace {

        namespace asio = boost::asio;
        namespace v2   = boost::process::v2;

        // The process handle runs on the process-wide transport loop, so waiting
        // for it never needs a thread of its own.
        using process_type = v2::basic_process<asio::io_context::executor_type>;

        // Keeps the process object alive as long as someone holds the child.
        struct native_process_holder {
            explicit native_process_holder(process_type&& value)
                : process(std::move(value)) {
            }

            ~native_process_holder() {
                // Boost.Process terminates a running child from its destructor;
                // this owner only ever gives up the handle, exactly like the
                // previous wrapper. Terminating is terminate_child()'s job.
                boost::system::error_code ec;
                if (process.running(ec)) {
                    process.detach();
                }
            }

            process_type process;
        };

        // An absolute/relative path that names a directory component is used as
        // given; a bare name is looked up in PATH (with PATHEXT on Windows), which
        // is what the previous `execvp` based launcher did. Boost.Process's default
        // launcher execs directly, so the lookup has to happen here.
        fs::path resolve_executable(const std::string& text) {
            fs::path given = path_from_utf8(text);
#if defined(_WIN32)
            const bool has_directory = given.has_parent_path();
#else
            const bool has_directory = text.find('/') != std::string::npos;
#endif
            if (has_directory) {
                return given;
            }
            auto found = v2::environment::find_executable(given);
            if (!found.empty()) {
                return found;
            }
            return given;
        }

        std::vector<std::string> arguments_of(const std::vector<std::string>& argv) {
            // argv[0] is the executable; Process v2 builds [exe, args...] itself.
            if (argv.size() <= 1) {
                return {};
            }
            return std::vector<std::string>(argv.begin() + 1, argv.end());
        }

#if defined(_WIN32)
        std::vector<std::string> parent_environment() {
            std::vector<std::string> entries;
            LPWCH block = ::GetEnvironmentStringsW();
            if (!block) {
                return entries;
            }
            for (LPWCH item = block; *item; item += std::wcslen(item) + 1) {
                const std::size_t length = std::wcslen(item);
                entries.push_back(wide_to_utf8(item, length));
            }
            ::FreeEnvironmentStringsW(block);
            return entries;
        }
#else
        std::vector<std::string> parent_environment() {
            std::vector<std::string> entries;
            for (char** item = ::environ; item && *item; ++item) {
                entries.emplace_back(*item);
            }
            return entries;
        }
#endif

        // Inheritance with overrides: a string value replaces the variable, any
        // other JSON value removes it. Windows keys compare case-insensitively,
        // so the deletion matches what the OS would have done.
        std::vector<std::string> build_environment_block(const Json& env) {
            std::vector<std::string> entries = parent_environment();
            if (!env.is_object() || env.empty()) {
                return entries;
            }
            for (auto it = env.begin(); it != env.end(); ++it) {
                const std::string& key = it.key();
                entries.erase(
                    std::remove_if(entries.begin(), entries.end(), [&key](const std::string& entry) {
                        const auto eq = entry.find('=');
                        if (eq == std::string::npos) {
                            return false;
                        }
#if defined(_WIN32)
                        if (entry.size() < key.size() || _strnicmp(entry.c_str(), key.c_str(), key.size()) != 0) {
                            return false;
                        }
                        return eq == key.size();
#else
                        return eq == key.size() && entry.compare(0, key.size(), key) == 0;
#endif
                    }),
                    entries.end()
                );
                if (it.value().is_string()) {
                    entries.push_back(key + "=" + it.value().get<std::string>());
                }
            }
            return entries;
        }

#if defined(_WIN32)
        // The Windows devkit is built against the static release CRT, so the child
        // handle is the only thing that has to be closed by the holder.
        bool join_job(HANDLE process) {
            static HANDLE job = []() -> HANDLE {
                SECURITY_ATTRIBUTES attributes {};
                attributes.bInheritHandle = FALSE;
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
                limits.BasicLimitInformation.LimitFlags =
                    JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
                HANDLE created = ::CreateJobObjectW(&attributes, nullptr);
                if (!created) {
                    return nullptr;
                }
                if (!::SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                    ::CloseHandle(created);
                    return nullptr;
                }
                return created;
            }();
            if (!job) {
                return false;
            }
            if (!::AssignProcessToJobObject(job, process)) {
                // Already in a job that does not allow breakaway: the adapter's own
                // job (if any) keeps its own kill-on-close semantics.
                return ::GetLastError() == ERROR_ACCESS_DENIED;
            }
            return true;
        }
#endif

        // A detached POSIX child starts a new session, so the adapter's terminal
        // signals do not reach it; the Windows side uses DETACHED_PROCESS.
        struct new_session_init {
#if !defined(_WIN32)
            template <typename Launcher>
            boost::system::error_code on_exec_setup(Launcher&, const fs::path&, const char* const*) const {
                ::setsid();
                return {};
            }
#endif
        };

        struct launch_arguments {
            fs::path executable;
            std::vector<std::string> arguments;
#if defined(_WIN32)
            std::vector<std::wstring> wide_arguments;
#endif
            std::vector<std::string> environment;
        };

        // Windows needs the wide forms: the launcher builds a UTF-16 command line,
        // and converting the UTF-8 inputs here (rather than inside the process
        // library) keeps unpaired surrogates intact.
        launch_arguments prepare_arguments(const SpawnOptions& options) {
            launch_arguments prepared;
            prepared.executable  = resolve_executable(options.argv.empty() ? std::string() : options.argv[0]);
            prepared.arguments   = arguments_of(options.argv);
            prepared.environment = build_environment_block(options.env);
#if defined(_WIN32)
            prepared.wide_arguments.reserve(prepared.arguments.size());
            for (const auto& argument : prepared.arguments) {
                prepared.wide_arguments.push_back(utf8_to_wide(argument));
            }
#endif
            return prepared;
        }

        // A start-directory initializer is only added when a working directory was
        // requested: `process_start_dir` with an empty path makes the child call
        // chdir("") and fail with ENOENT before exec.
        process_type launch_detached_aware(const launch_arguments& prepared, v2::process_environment& environment, const std::string& cwd, bool detached, boost::system::error_code& ec) {
            v2::default_process_launcher launcher;
            auto& context            = transport::Reactor::instance().context();
            const fs::path start_dir = cwd.empty() ? fs::path() : path_from_utf8(cwd);
#if defined(_WIN32)
            const auto flags = v2::windows::creation_flags(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP);
            if (start_dir.empty()) {
                if (detached) {
                    return launcher(context, ec, prepared.executable, prepared.wide_arguments, environment, flags);
                }
                return launcher(context, ec, prepared.executable, prepared.wide_arguments, environment);
            }
            if (detached) {
                return launcher(context, ec, prepared.executable, prepared.wide_arguments, environment, v2::process_start_dir(start_dir), flags);
            }
            return launcher(context, ec, prepared.executable, prepared.wide_arguments, environment, v2::process_start_dir(start_dir));
#else
            if (start_dir.empty()) {
                if (detached) {
                    return launcher(context, ec, prepared.executable, prepared.arguments, environment, new_session_init {});
                }
                return launcher(context, ec, prepared.executable, prepared.arguments, environment);
            }
            if (detached) {
                return launcher(context, ec, prepared.executable, prepared.arguments, environment, v2::process_start_dir(start_dir), new_session_init {});
            }
            return launcher(context, ec, prepared.executable, prepared.arguments, environment, v2::process_start_dir(start_dir));
#endif
        }

    }  // namespace

    // Implemented per platform at the bottom of this file.
    bool frida_child_is_running(ChildProcess& child);
    bool frida_child_resume(ChildProcess& child);

    bool spawn_child(const SpawnOptions& options, ChildProcess& out, std::string& error) {
        if (options.argv.empty()) {
            error = "spawn failed: empty command line";
            return false;
        }
        launch_arguments prepared = prepare_arguments(options);
        v2::process_environment environment(prepared.environment);

        boost::system::error_code ec;
        process_type process =
            launch_detached_aware(prepared, environment, options.cwd, options.detached, ec);

        if (ec) {
            error = std::string("spawn failed: ") + ec.message();
            return false;
        }
#if defined(_WIN32)
        if (!options.detached) {
            // Kill-on-adapter-exit for the children this process owns; a detached
            // child and anything frida spawned are deliberately outside the job.
            join_job(process.native_handle());
        }
#endif
        auto holder = std::make_shared<native_process_holder>(std::move(process));
        out.pid     = static_cast<long>(holder->process.id());
        out.owner   = ChildOwner::native;
        out.handle  = holder;
        if (out.pid <= 0) {
            error = "spawn failed: no pid";
            out.handle.reset();
            return false;
        }
        return true;
    }

    bool wait_child(ChildProcess& child, int& exit_code, std::string& error) {
        exit_code = -1;
        if (child.owner == ChildOwner::frida) {
            error = "a frida-owned child cannot be waited for";
            return false;
        }
        auto holder = std::static_pointer_cast<native_process_holder>(child.handle);
        if (!holder) {
            error = "child handle is gone";
            return false;
        }
        boost::system::error_code ec;
        holder->process.wait(ec);
        if (ec) {
            error = "wait failed: " + ec.message();
            return false;
        }
        exit_code = holder->process.exit_code();
        return true;
    }

    bool is_child_running(ChildProcess& child) {
        if (child.owner == ChildOwner::frida) {
            return frida_child_is_running(child);
        }
        auto holder = std::static_pointer_cast<native_process_holder>(child.handle);
        if (!holder) {
            return false;
        }
        boost::system::error_code ec;
        return holder->process.running(ec);
    }

    bool resume_child(ChildProcess& child) {
        if (child.owner == ChildOwner::frida) {
            return frida_child_resume(child);
        }
        // Only a frida-gated child starts suspended; a native child is running.
        log_debug("frontend: resume requested for a native child");
        return false;
    }

    void detach_child(ChildProcess& child) {
        if (child.owner == ChildOwner::frida) {
            // Releasing a frida-owned child means forgetting it; frida keeps the
            // process alive on its own until it exits.
            child.pid = 0;
            return;
        }
        auto holder = std::static_pointer_cast<native_process_holder>(child.handle);
        if (holder) {
            // Give up the handle without terminating: the returned handle object
            // closes the OS handle when it goes out of scope.
            holder->process.detach();
        }
        child.handle.reset();
        child.pid = 0;
    }

    void close_child(ChildProcess& child) {
        detach_child(child);
    }

    CapturedRun run_captured(const std::vector<std::string>& argv, const std::string& cwd, const Json& env) {
        CapturedRun result;
        if (argv.empty()) {
            result.err = "spawn failed: empty command line";
            return result;
        }

        // A local context: the captured run must not re-enter the adapter's loop.
        asio::io_context context;
        asio::readable_pipe out_pipe(context);
        asio::readable_pipe err_pipe(context);

        SpawnOptions options;
        options.argv              = argv;
        options.cwd               = cwd;
        options.env               = env;
        launch_arguments prepared = prepare_arguments(options);
        v2::process_environment environment(prepared.environment);
        const fs::path start_dir = cwd.empty() ? fs::path() : path_from_utf8(cwd);

        v2::default_process_launcher launcher;
        boost::system::error_code ec;
        std::optional<process_type> launched;

        // The stdio bindings own the parent's copy of the child's descriptors, so
        // they are destroyed as soon as the child exists: keeping them alive until
        // the end of this function would keep the write ends open and the drains
        // would never see EOF.
        {
            v2::process_stdio stdio {
                v2::detail::process_input_binding {},
                v2::detail::process_output_binding { out_pipe },
                v2::detail::process_error_binding { err_pipe }
            };
#if defined(_WIN32)
            if (start_dir.empty()) {
                launched.emplace(launcher(context, ec, prepared.executable, prepared.wide_arguments, environment, stdio));
            } else {
                launched.emplace(launcher(context, ec, prepared.executable, prepared.wide_arguments, environment, v2::process_start_dir(start_dir), stdio));
            }
#else
            if (start_dir.empty()) {
                launched.emplace(launcher(context, ec, prepared.executable, prepared.arguments, environment, stdio));
            } else {
                launched.emplace(launcher(context, ec, prepared.executable, prepared.arguments, environment, v2::process_start_dir(start_dir), stdio));
            }
#endif
        }
        process_type process = launched ? std::move(*launched) : process_type { context.get_executor() };
        if (ec) {
            // 77 is the "permission denied" code the callers already translate.
            result.err = std::string("spawn failed: ") + ec.message();
            return result;
        }
        result.spawned = true;

        int remaining = 3;
        auto done     = [&](bool stop) {
            --remaining;
            if (stop || remaining == 0) {
                context.stop();
            }
        };

        asio::co_spawn(
            context,
            [&]() -> asio::awaitable<void> {
                std::array<char, 4096> buffer {};
                for (;;) {
                    boost::system::error_code read_error;
                    const std::size_t read = co_await out_pipe.async_read_some(
                        asio::buffer(buffer.data(), buffer.size()),
                        asio::redirect_error(asio::use_awaitable, read_error)
                    );
                    if (read > 0) {
                        result.out.append(buffer.data(), read);
                    }
                    if (read_error) {
                        // EOF ends this stream normally; anything else is reported
                        // through the captured bytes and the exit code.
                        break;
                    }
                }
                out_pipe.close();
                done(false);
            },
            [&](std::exception_ptr failure) {
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        log_debug("frontend: stdout drain failed: %s", error.what());
                    } catch (...) {
                        log_debug("frontend: stdout drain failed");
                    }
                    done(true);
                }
            }
        );

        asio::co_spawn(
            context,
            [&]() -> asio::awaitable<void> {
                std::array<char, 4096> buffer {};
                for (;;) {
                    boost::system::error_code read_error;
                    const std::size_t read = co_await err_pipe.async_read_some(
                        asio::buffer(buffer.data(), buffer.size()),
                        asio::redirect_error(asio::use_awaitable, read_error)
                    );
                    if (read > 0) {
                        result.err.append(buffer.data(), read);
                    }
                    if (read_error) {
                        break;
                    }
                }
                err_pipe.close();
                done(false);
            },
            [&](std::exception_ptr failure) {
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        log_debug("frontend: stderr drain failed: %s", error.what());
                    } catch (...) {
                        log_debug("frontend: stderr drain failed");
                    }
                    done(true);
                }
            }
        );

        asio::co_spawn(
            context,
            [&]() -> asio::awaitable<void> {
                boost::system::error_code wait_error;
                co_await process.async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
                result.exit_code = wait_error ? -1 : process.exit_code();
                done(false);
            },
            [&](std::exception_ptr failure) {
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        log_debug("frontend: wait failed: %s", error.what());
                    } catch (...) {
                        log_debug("frontend: wait failed");
                    }
                    done(true);
                }
            }
        );

        context.run();
        return result;
    }

#if defined(_WIN32)

    namespace {

        // frida delivers signals on its own main loop thread, so the sink is never
        // invoked from the handler: bytes are queued here and drained by
        // pump_frida_events() on the adapter thread.
        std::mutex g_output_mtx;
        std::deque<std::pair<int, std::string>> g_output_queue;
        constexpr std::size_t kMaxQueue = 4096;
        ChildOutputSink g_output_sink;
        FridaDeviceManager* g_manager = nullptr;
        FridaDevice* g_device         = nullptr;
        std::once_flag g_device_once;

        void on_device_output(FridaDevice* /*device*/, guint /*pid*/, gint fd, GBytes* data, gpointer /*user_data*/) {
            gsize size        = 0;
            const char* bytes = static_cast<const char*>(g_bytes_get_data(data, &size));
            // An empty buffer marks EOF for that descriptor; nothing to forward.
            if (size == 0) {
                return;
            }
            std::lock_guard lock(g_output_mtx);
            if (g_output_queue.size() >= kMaxQueue) {
                // Never let a chatty target grow without bound if the adapter loop
                // is stuck; dropped bytes are reported once when it resumes.
                g_output_queue.pop_front();
            }
            g_output_queue.emplace_back(fd, std::string(bytes, size));
        }

        bool ensure_device(std::string& error) {
            std::call_once(g_device_once, []() {
                frida_init();
                g_manager = frida_device_manager_new();
            });
            if (g_device) {
                return true;
            }
            GError* frida_error   = nullptr;
            FridaDeviceList* list = frida_device_manager_enumerate_devices_sync(
                g_manager, nullptr, &frida_error
            );
            if (frida_error) {
                error = frida_error->message ? frida_error->message : "cannot enumerate devices";
                g_error_free(frida_error);
                return false;
            }
            for (gint i = 0; i < frida_device_list_size(list); ++i) {
                FridaDevice* device = frida_device_list_get(list, i);
                if (frida_device_get_dtype(device) == FRIDA_DEVICE_TYPE_LOCAL) {
                    g_device = g_object_ref(device);
                }
                g_object_unref(device);
            }
            g_object_unref(list);
            if (!g_device) {
                error = "no local frida device";
                return false;
            }
            g_signal_connect(g_device, "output", G_CALLBACK(on_device_output), nullptr);
            return true;
        }

        // Inherited environment with the overrides/deletions applied, as the
        // "KEY=VALUE" block frida expects.
        std::vector<std::string> build_environment_block(const Json& overrides) {
            std::vector<std::string> entries = parent_environment();
            if (!overrides.is_object()) {
                return entries;
            }
            for (auto it = overrides.begin(); it != overrides.end(); ++it) {
                const std::string& key = it.key();
                entries.erase(
                    std::remove_if(entries.begin(), entries.end(), [&key](const std::string& entry) {
                        const auto eq = entry.find('=');
                        if (eq == std::string::npos) {
                            return false;
                        }
                        return eq == key.size() && _stricmp(entry.substr(0, eq).c_str(), key.c_str()) == 0;
                    }),
                    entries.end()
                );
                if (it.value().is_string()) {
                    entries.push_back(key + "=" + it.value().get<std::string>());
                }
            }
            return entries;
        }

        // frida delivers signals on the creating thread's GLib main context; the
        // adapter loop is the transport reactor, so it has to pump that context.
        FridaDevice* frida_local_device(std::string& error) {
            if (!ensure_device(error)) {
                return nullptr;
            }
            return g_device;
        }

    }  // namespace

    void set_child_output_sink(ChildOutputSink sink) {
        std::lock_guard lock(g_output_mtx);
        g_output_sink = std::move(sink);
    }

    void pump_frida_events() {
        if (!g_manager) {
            return;
        }
        auto context = g_main_context_default();
        while (g_main_context_pending(context)) {
            g_main_context_iteration(context, FALSE);
        }
        // Deliver whatever the handler queued, on this (the adapter's) thread.
        for (;;) {
            std::pair<int, std::string> item;
            ChildOutputSink sink;
            {
                std::lock_guard lock(g_output_mtx);
                if (g_output_queue.empty()) {
                    break;
                }
                item = std::move(g_output_queue.front());
                g_output_queue.pop_front();
                sink = g_output_sink;
            }
            if (sink) {
                sink(item.first, item.second);
            }
        }
    }

    bool spawn_child_gated(const SpawnOptions& options, ChildProcess& out, std::string& error) {
        auto device = frida_local_device(error);
        if (!device) {
            return false;
        }
        GError* frida_error   = nullptr;
        FridaSpawnOptions* so = frida_spawn_options_new();
        std::vector<gchar*> argv;
        argv.reserve(options.argv.size() + 1);
        for (const auto& item : options.argv) {
            argv.push_back(const_cast<gchar*>(item.c_str()));
        }
        if (!argv.empty()) {
            frida_spawn_options_set_argv(so, argv.data(), static_cast<gint>(argv.size()));
        }
        if (!options.cwd.empty()) {
            frida_spawn_options_set_cwd(so, options.cwd.c_str());
        }
        // String values override, any other value *deletes* the variable. Frida's
        // `env` can only add or override, so the complete block is built here and
        // passed as `envp`.
        auto env_values = build_environment_block(options.env);
        std::vector<gchar*> envp;
        envp.reserve(env_values.size());
        for (auto& item : env_values) {
            envp.push_back(const_cast<gchar*>(item.c_str()));
        }
        if (!envp.empty()) {
            frida_spawn_options_set_envp(so, envp.data(), static_cast<gint>(envp.size()));
        }
        frida_spawn_options_set_stdio(so, FRIDA_STDIO_PIPE);

        guint pid = frida_device_spawn_sync(device, options.argv.empty() ? "" : options.argv[0].c_str(), so, nullptr, &frida_error);
        g_object_unref(so);
        if (frida_error) {
            error = std::string("spawn failed: ") + (frida_error->message ? frida_error->message : "");
            g_error_free(frida_error);
            return false;
        }
        out.pid   = static_cast<long>(pid);
        out.owner = ChildOwner::frida;
        return true;
    }

    bool frida_child_is_running(ChildProcess& child) {
        std::string error;
        auto device = frida_local_device(error);
        if (!device) {
            return false;
        }
        GError* frida_error   = nullptr;
        FridaProcess* process = frida_device_get_process_by_pid_sync(
            device, static_cast<guint>(child.pid), nullptr, nullptr, &frida_error
        );
        if (frida_error) {
            g_error_free(frida_error);
            return false;
        }
        bool running = process && frida_process_get_pid(process) != 0;
        if (process) {
            g_object_unref(process);
        }
        return running;
    }

    bool frida_child_resume(ChildProcess& child) {
        std::string error;
        auto device = frida_local_device(error);
        if (!device) {
            return false;
        }
        GError* frida_error = nullptr;
        frida_device_resume_sync(device, static_cast<guint>(child.pid), nullptr, &frida_error);
        if (frida_error) {
            log_error("frontend: resume failed: %s", frida_error->message);
            g_error_free(frida_error);
            return false;
        }
        return true;
    }

    void terminate_child(ChildProcess& child) {
        if (!child.valid()) {
            return;
        }
        if (child.owner == ChildOwner::frida) {
            std::string error;
            auto device = frida_local_device(error);
            if (device) {
                GError* frida_error = nullptr;
                frida_device_kill_sync(device, static_cast<guint>(child.pid), nullptr, &frida_error);
                if (frida_error) {
                    g_error_free(frida_error);
                }
            }
            child.pid = 0;
            return;
        }
        auto holder = std::static_pointer_cast<native_process_holder>(child.handle);
        if (holder) {
            // Launch-failure cleanup: kill and reap, explicitly.
            boost::system::error_code ec;
            holder->process.terminate(ec);
            holder->process.wait(ec);
        }
        child.handle.reset();
        child.pid = 0;
    }

#else

    void set_child_output_sink(ChildOutputSink sink) {
        (void)sink;
    }

    void pump_frida_events() {
    }

    bool spawn_child_gated(const SpawnOptions& options, ChildProcess& out, std::string& error) {
        (void)options;
        (void)out;
        error = "gated spawn is only used on windows";
        return false;
    }

    bool frida_child_is_running(ChildProcess& child) {
        (void)child;
        return false;
    }

    bool frida_child_resume(ChildProcess& child) {
        (void)child;
        return false;
    }

    void terminate_child(ChildProcess& child) {
        if (!child.valid()) {
            return;
        }
        auto holder = std::static_pointer_cast<native_process_holder>(child.handle);
        if (holder) {
            // Kill and reap explicitly: releasing the handle would only detach,
            // which leaves a zombie for as long as the adapter keeps running.
            boost::system::error_code ec;
            holder->process.terminate(ec);
            holder->process.wait(ec);
        }
        child.handle.reset();
        child.pid = 0;
    }

#endif

}  // namespace luadebug::debugger::frontend
