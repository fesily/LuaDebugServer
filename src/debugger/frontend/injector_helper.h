#pragma once

// The injector helper: a per-architecture process the adapter spawns to perform
// one injection.
//
// The chain the adapter drives is
//
//     adapter ── helper (optional) ── launcher (optional) ── debugger
//
// and only the *helper* has to match the target's architecture:
//
//   * `bin/lua-debug-injector-arm64` / `-x64` - this build's own helper (frida
//     backend). It has to run as the target's architecture because frida-core
//     resolves libSystem inside the target, so an x64 target on an arm64 host
//     needs the x64 helper started through `/usr/bin/arch -x86_64`.
//   * `lldb` - the system's helper for the lldb backend, with the same rule
//     (it reads the target's shared cache, which is per architecture).
//   * none at all - Linux and Windows link frida-core into the adapter.
//
// The launcher is one file per platform (`bin/launcher.so`, a fat binary on
// macOS), and the debugger is one bridge per platform/arch, so neither needs a
// per-architecture copy here.

#include <string>
#include <vector>

#include "frontend/inject.h"

namespace luadebug::debugger::frontend {

    struct InjectorHelper {
        std::string path;                 // absolute path of the helper
        std::string label;                // basename, for logs and errors
        std::vector<std::string> prefix;  // e.g. {"/usr/bin/arch", "-x86_64"}
    };

    // Resolves `<workdir>/bin/lua-debug-injector-<target arch>` and the runner
    // prefix needed to execute it as that architecture.
    bool resolve_injector_helper(const InjectContext& context, const std::string& target_arch, InjectorHelper& helper, std::string& error);

    // The full command line: prefix + helper + arguments.
    std::vector<std::string> helper_command(const InjectorHelper& helper, const std::vector<std::string>& arguments);

    // For a helper that is an OS tool (lldb): prepend the architecture runner
    // when the command has to run as x86_64 on an arm64 host.
    std::vector<std::string> run_as_target_arch(std::vector<std::string> command, const std::string& target_arch);

    // Architecture of an executable on disk, as a runtime platform suffix
    // ("arm64" / "x64"): a thin Mach-O reports its own slice, a fat one the
    // slice the OS would pick on this host. Empty when it cannot be read.
    std::string executable_arch(const std::string& path);

}  // namespace luadebug::debugger::frontend
