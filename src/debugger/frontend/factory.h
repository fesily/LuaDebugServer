#pragma once

// Port of extension/script/frontend/debuger_factory.lua: turns launch/attach
// arguments into either a concrete argv for a console spawn, or a
// `runInTerminal` request body for integrated/external terminals.

#include <string>
#include <vector>

#include "common/bootstrap.h"
#include "common/json.h"
#include "frontend/platform.h"
#include "frontend/spawn.h"

namespace luadebug::debugger::frontend {

    struct FactoryContext {
        std::string workdir;  // published root (`publish`)
        PlatformState platform;
    };

    struct ConsolePlan {
        std::string luaexe;
        SpawnOptions spawn;
        bool need_resume = false;  // Windows launches suspended
    };

    struct TerminalPlan {
        Json arguments = Json::object();
    };

    // Shared initialization: pulls useWSL/sourceCoding out of the arguments.
    void factory_initialize(const Json& arguments, PlatformState& state);

    // Resolves the interpreter: explicit `luaexe`, else runtime/<platform>/<ver>/lua.
    bool resolve_lua_exe(const Json& arguments, const FactoryContext& context, std::string& path, std::string& error);

    // `console: internalConsole` with a plain Lua interpreter.
    bool create_luaexe_in_console(const Json& arguments, const FactoryContext& context, const std::string& address, ConsolePlan& plan, std::string& error);

    // `console: integratedTerminal | externalTerminal` with a plain interpreter.
    bool create_luaexe_in_terminal(const Json& init_request, const Json& arguments, const FactoryContext& context, const std::string& address, TerminalPlan& plan, std::string& error);

    // How a `runtimeExecutable` launch reaches the debugger.
    struct LaunchInjection {
        bool enabled = false;
        // Deliver the bootstrap document through the dynamic loader
        // (DYLD_INSERT_LIBRARIES / LD_PRELOAD). macOS and Linux use this: it is
        // the only way to hook before the interpreter creates its first Lua
        // state without either root or a pre-dyld injection that frida cannot
        // perform safely.
        bool preload = false;
        std::string launcher;
        BootstrapConfig config;
    };

    // `runtimeExecutable` launch in an internal console: builds argv, cwd and the
    // environment the target must see (including the inherited/overridden env and
    // the preload carrier when injection is enabled).
    bool create_process_in_console(const Json& arguments, const FactoryContext& context, const LaunchInjection& injection, SpawnOptions& spawn, std::string& error);

    // `runtimeExecutable` launch in a terminal.
    bool create_process_in_terminal(const Json& init_request, const Json& arguments, const FactoryContext& context, TerminalPlan& plan, std::string& error);

}  // namespace luadebug::debugger::frontend
