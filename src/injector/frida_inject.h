#pragma once

// Native library injection through frida-core's devkit.
//
// This layer owns exactly three things: loading a shared library into another
// process, proving this injection's entry point has finished, and turning
// frida's GError into structured data. No DAP, no Lua, no frida JS agent.

#include <string>

namespace luadebug::injector {

    enum class Status {
        ok,
        // frida reported FRIDA_ERROR_PERMISSION_DENIED: the caller may retry once
        // through the OS escalation path.
        permission_denied,
        failed,
        // The entry point never reported completion inside the deadline.
        timed_out,
    };

    struct Result {
        Status status = Status::failed;
        // frida-core injection id matched against `uninjected`.
        unsigned int id = 0;
        // Structured GError (0/0 when the failure did not come from frida).
        unsigned int error_domain = 0;
        int error_code            = 0;
        std::string error_message;
    };

    // Process-wide frida-core initialisation; idempotent and thread-safe.
    void global_init();

    // Injects `library` into `pid` and calls its `entrypoint` with `data`
    // (a UTF-8 string, may be empty). Returns only after frida reported that
    // this very injection's entry point has finished, or after `timeout_ms`.
    //
    // The unload policy belongs to the entry point, not to us: frida keeps the
    // image mapped when the entry asks for RESIDENT.
    Result inject_library(unsigned int pid, const std::string& library, const std::string& entrypoint, const std::string& data, unsigned timeout_ms = 15000);

    // Exit code the macOS client uses to tell "escalate and retry" apart from a
    // plain failure, so no caller has to match on English error text.
    constexpr int kExitOk               = 0;
    constexpr int kExitFailed           = 1;
    constexpr int kExitPermissionDenied = 77;

}  // namespace luadebug::injector
