#pragma once

// Locates and binds `luadebug_runtime` for a process that must share the
// session hub: the adapter executable and the target-side bridge use this
// loader (or the equivalent on the target side) instead of linking a second
// copy of the hub.
//
// The library is addressed by canonical absolute path (resolved from the
// executable's published tree), never by a bare name and never through
// process-global Lua symbols.

#include <string>

#include "runtime/abi.h"

namespace luadebug::debugger::runtime {

    // Published file name per platform, e.g. `libluadebug_runtime.dylib`.
    const char* runtime_library_name();

    // Directory that holds the runtime for this platform/arch, derived from the
    // executable's `<root>/bin/<exe>` layout: `<root>/runtime/<platform>`.
    std::string default_runtime_directory();

    struct BoundRuntime {
        LdRuntimeApi api {};
        void* handle = nullptr;
        std::string path;

        bool valid() const {
            return handle != nullptr && api.start_session != nullptr;
        }
    };

    // Loads and handshakes the runtime. On failure `error` names the path and
    // the reason, and no fallback (Lua backend, static hub) is attempted.
    bool bind_runtime(const std::string& path, BoundRuntime& out, std::string& error);

    // Convenience: `default_runtime_directory()/runtime_library_name()`.
    bool bind_default_runtime(BoundRuntime& out, std::string& error);

    void unbind(BoundRuntime& runtime);

}  // namespace luadebug::debugger::runtime
