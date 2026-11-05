#pragma once

// Platform view used by the frontend: which OS we are driving (WSL remaps
// Windows to Linux), process lookup, and helper discovery.

#include <string>
#include <vector>

#include "common/json.h"

namespace luadebug::debugger::frontend {

    enum class HostOs {
        windows,
        macos,
        linux_,
        other,
    };

    HostOs host_os();
    const char* host_os_name(HostOs os);

    struct PlatformState {
        HostOs os     = HostOs::other;
        bool use_wsl  = false;
        bool use_utf8 = false;
    };

    // Port of frontend/platform_os.lua::init plus debuger_factory.initialize.
    void init_platform(const Json& arguments, PlatformState& state);

    // Port of frontend/query_process.lua; returns matching pids.
    std::vector<long> query_processes(const std::string& name);

    // Port of frontend/powershell.lua; empty when unavailable (non-Windows).
    std::string find_powershell();

    // Port of bee::platform Arch for the hosts the adapter supports.
    std::string host_arch();

    std::string current_executable_path();
    // WORKDIR of the published tree: <exe dir>/.. — `publish` for bin/lua-debug.
    std::string workdir_from_executable();

}  // namespace luadebug::debugger::frontend
