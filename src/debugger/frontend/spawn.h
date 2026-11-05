#pragma once

// Process helpers.
//   * start a debuggee and keep it running (console launch),
//   * spawn through frida when the launch needs a pre-main gate (Windows),
//   * run a short helper (the inject clients, osascript, fuser) and capture its
//     output.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/json.h"

namespace luadebug::debugger::frontend {

    struct SpawnOptions {
        std::vector<std::string> argv;
        std::string cwd;
        // Only the keys present here are overridden; the rest are inherited.
        Json env      = Json::object();
        bool detached = false;
    };

    // Who owns the child: this process (a Boost.Process v2 handle), or frida's
    // local device. The distinction matters for resume/kill/close: a frida-owned
    // child must never be touched through the local process handle, and vice
    // versa.
    enum class ChildOwner {
        native,
        frida,
    };

    struct ChildProcess {
        long pid         = 0;
        ChildOwner owner = ChildOwner::native;
        // Opaque owner-specific state (the native process handle holder).
        std::shared_ptr<void> handle;
        bool valid() const {
            return pid != 0;
        }
    };

    // Receives the bytes a spawned child writes to fd 1/2 (frida-owned children
    // only: the debugger's own capture takes over those descriptors once the
    // target-side redirect is installed, so nothing is delivered twice).
    using ChildOutputSink = std::function<void(int fd, const std::string& bytes)>;
    void set_child_output_sink(ChildOutputSink sink);

    // Starts a process and leaves it attached to the adapter's stdio (the
    // debuggee's own stdout is captured inside the target by the debugger).
    bool spawn_child(const SpawnOptions& options, ChildProcess& out, std::string& error);

    // Starts a process through frida's local device, which spawns it gated (the
    // target has not run any user code yet). Its stdio is a pipe delivered to the
    // output sink, never to the adapter's protocol stdout.
    bool spawn_child_gated(const SpawnOptions& options, ChildProcess& out, std::string& error);

    // Pumps the GLib main context frida delivers its signals on. Call from the
    // adapter's own loop iteration; only meaningful after a gated spawn.
    void pump_frida_events();

    // Waits for a native child to exit and reports its status. A frida-owned
    // child is not waited for: frida owns its lifetime.
    bool wait_child(ChildProcess& child, int& exit_code, std::string& error);

    bool is_child_running(ChildProcess& child);
    bool resume_child(ChildProcess& child);
    void detach_child(ChildProcess& child);
    void close_child(ChildProcess& child);
    // Terminates the child (launch failure cleanup). Never used for attach, whose
    // target belongs to the user.
    void terminate_child(ChildProcess& child);

    struct CapturedRun {
        bool spawned  = false;
        int exit_code = -1;
        std::string out;
        std::string err;
    };

    // Runs `argv` to completion, capturing stdout/stderr.
    CapturedRun run_captured(const std::vector<std::string>& argv, const std::string& cwd = std::string(), const Json& env = Json::object());

}  // namespace luadebug::debugger::frontend
