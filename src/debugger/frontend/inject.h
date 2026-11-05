#pragma once

// Injection frontend: turns DAP launch/attach arguments into a bootstrap
// document and hands the launcher to an injector backend (or, on
// macOS/Linux launches, to the dynamic loader as a preloaded library).
//
// `inject` selects whether we inject at all: `none`, or a backend name
// (`frida`, `lldb`, `auto`). The backend itself is chosen in
// frontend/injector_backend.h: frida when the adapter and the target share an
// architecture, lldb when they do not (an arm64 adapter attaching to a Rosetta
// x86_64 process). The hand-rolled mac helper and `gdb` are gone.

#include <string>

#include "common/bootstrap.h"
#include "common/json.h"
#include "frontend/platform.h"

namespace luadebug::debugger::frontend {

    struct InjectContext {
        std::string workdir;  // published root; launcher lives in <workdir>/bin
        PlatformState platform;
    };

    enum class InjectEntry {
        launch,
        attach,
    };

    // The configured strategy, with the documented defaults applied:
    //   * an explicit `inject` wins when present,
    //   * an explicit `address` means "connect to that session" -> none,
    //   * everything else (attach by pid/name, runtimeExecutable launch) -> frida.
    // Legacy values are rejected with a migration error.
    bool resolve_inject_mode(const Json& arguments, std::string& mode, std::string& error);

    // Absolute path of the launcher image for this target.
    bool launcher_path(const InjectContext& context, const std::string& lua_arch, std::string& path, std::string& error);

    // Builds the bootstrap document for one injection. `lua_arch` is the target
    // process' architecture (see detect_target_arch), never the adapter's; an
    // empty value leaves `platform` to the target's own detection (the plain
    // `luaexe` path, where the interpreter that runs is the one we started).
    //
    // Fails when a supplied VM source field is invalid; a symbol table too large
    // for the carrier is moved into a session temp file instead of failing, so a
    // large table never makes the document undeliverable.
    bool make_bootstrap_config(long pid, InjectEntry entry, const Json& arguments, const InjectContext& context, const std::string& lua_arch, BootstrapConfig& out, std::string& error);

    // The environment carrier a plain `luaexe` launch uses: the bootstrap
    // document in `LUA_DEBUG_CONFIG` plus the `LUA_DEBUG_VM_SOURCE` marker,
    // which makes `debugger.lua` open the bridge through the launcher's own
    // entry instead of the target's exported symbols. A launch that names no VM
    // source leaves `env` untouched. `env` is an overrides object, so `null`
    // deletes a key and a plain assignment replaces the user's value.
    bool apply_vm_source_env(const Json& arguments, const InjectContext& context, Json& env, std::string& error);

    // True when the request carries any part of a VM source.
    bool has_vm_source(const Json& arguments);

    // Rejects a VM source on a request that cannot apply it: joining an existing
    // session by `address` (or an `inject: "none"` launch into one) never runs a
    // launcher, so silently ignoring the table would debug whatever VM happens to
    // be there while the user believes the table was used.
    bool validate_vm_source_usage(const Json& arguments, const char* request, std::string& error);

    // Detects the target process' architecture as a runtime bridge platform
    // suffix: "arm64" or "x64". Returns false when it cannot be determined.
    bool detect_target_arch(long pid, std::string& lua_arch, std::string& error);

    // Injects the launcher and waits until its entry point finished.
    bool inject_process(long pid, InjectEntry entry, const Json& arguments, const InjectContext& context, std::string& error);

    // Launch helper (`runtimeExecutable`): the environment that makes the
    // dynamic loader preload the launcher before the first Lua state exists.
    // Returns false on platforms that inject at launch through frida instead.
    bool make_preload_env(InjectEntry entry, const std::string& launcher, const BootstrapConfig& config, bool preload_supported, Json& env, std::string& error);

}  // namespace luadebug::debugger::frontend
