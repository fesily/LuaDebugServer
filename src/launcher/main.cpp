#include <autoattach/autoattach.h>
#include <autoattach/ctx.h>
#include <bridge/bridge_abi.h>
#include <common/path_utf8.h>
#include <common/vm_version.h>
#include <host/platform_abi.h>
#include <resolver/symbol_source.h>
#include <util/log.h>

#include <gumpp.hpp>
#include <mutex>
#include <vector>
#ifndef _WIN32
#    include <unistd.h>
#    define DLLEXPORT __attribute__((visibility("default")))
#    define DLLEXPORT_DECLARATION
#else
#    include <windows.h>
#    define DLLEXPORT __declspec(dllexport)
#    define DLLEXPORT_DECLARATION __cdecl
#endif
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace luadebug::autoattach {
    namespace fs = debugger::fs;

    static std::string readfile(const fs::path& filename) {
#ifdef _WIN32
        FILE* f = _wfopen(filename.c_str(), L"rb");
#else
        FILE* f = fopen(filename.c_str(), "rb");
#endif
        if (!f) {
            return std::string();
        }
        fseek(f, 0, SEEK_END);
        long length = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string tmp;
        tmp.resize(length);
        fread(tmp.data(), 1, length, f);
        fclose(f);
        return tmp;
    }

    // The bootstrap snapshot holds everything the target side needs; only the
    // mode stays here because it is not a session field.
    static void push_session_config(const lua_api& api, lua::state L, const debugger::BootstrapConfig& config, const std::string& address) {
        api.createtable(L, 0, 5);
        const int table = api.gettop(L);

        api.pushlstring(L, address.data(), address.size());
        api.setfield(L, table, "address");

        api.pushboolean(L, config.client ? 1 : 0);
        api.setfield(L, table, "client");

        api.pushboolean(L, config.ansi ? 1 : 0);
        api.setfield(L, table, "ansi");

        if (!config.lua_version.empty()) {
            api.pushlstring(L, config.lua_version.data(), config.lua_version.size());
            api.setfield(L, table, "luaVersion");
        }
        if (!config.platform.empty()) {
            api.pushlstring(L, config.platform.data(), config.platform.size());
            api.setfield(L, table, "platform");
        }
    }

    // Where `script/` and `runtime/` live. A frida-injected image is mapped by
    // frida's own Mach-O loader, so it has no dyld entry and dladdr() cannot
    // name it: the publish root therefore travels in the bootstrap config. The
    // DSO fallback keeps plain dlopen/LoadLibrary hosts (and the launcher tests)
    // working without any config.
    static std::optional<fs::path> resolve_root(const debugger::BootstrapConfig& config) {
        if (!config.root.empty()) {
            // The contract carries UTF-8; a narrow fs::path would reinterpret it in
            // the process code page on Windows.
            return debugger::path_from_utf8(config.root);
        }
        // `platform_module_directory` already returns the directory of this
        // library (`<root>/bin`), so one parent is the published root.
        auto module_dir = debugger::runtime::platform_module_directory(
            reinterpret_cast<const void*>(&resolve_root)
        );
        if (module_dir.empty()) {
            return std::nullopt;
        }
        return debugger::path_from_utf8(module_dir).parent_path();
    }

    namespace {

        using bridge_open_fn = int (*)(const LdBridgeDescriptor*, const char**);

        // The bridge DSO next to `script/`, selected by the target platform (not
        // by the adapter's own architecture).
        fs::path bridge_path(const debugger::BootstrapConfig& config, const fs::path& root) {
            std::string platform = config.platform;
            if (platform.empty()) {
                platform = debugger::runtime::platform_name();
            }
#ifdef _WIN32
            const char* file = "luadebug.dll";
#else
            const char* file = "luadebug.so";
#endif
            return root / "runtime" / platform / file;
        }

        // Opens the bridge with the table the launcher already resolved, so the
        // target never has to export `luaopen_luadebug_*` and the script path
        // never searches for a caller anchor. Pushes the module table on success.
        bool open_bridge(lua::state L, int profile, const resolved_symbols& symbols, const fs::path& path, std::string& error) {
            void* handle = debugger::runtime::platform_dlopen(debugger::path_to_utf8(path));
            if (!handle) {
                error = "cannot load " + debugger::path_to_utf8(path) + ": " + debugger::runtime::platform_dl_error();
                return false;
            }
            auto open = reinterpret_cast<bridge_open_fn>(
                debugger::runtime::platform_dlsym(handle, "luadebug_bridge_open")
            );
            if (!open) {
                error = "the bridge does not export luadebug_bridge_open";
                return false;
            }
            // The names must outlive the call; the bridge copies what it needs.
            std::vector<std::string> names;
            names.reserve(symbols.entries.size());
            for (const auto& entry : symbols.entries) {
                names.push_back(entry.first);
            }
            std::vector<LdBridgeSymbol> table;
            table.reserve(names.size());
            {
                std::size_t index = 0;
                for (const auto& entry : symbols.entries) {
                    table.push_back(LdBridgeSymbol { names[index].c_str(), reinterpret_cast<void*>(entry.second) });
                    ++index;
                }
            }
            LdBridgeDescriptor descriptor {};
            descriptor.size         = sizeof(descriptor);
            descriptor.abi_version  = LDB_BRIDGE_ABI_VERSION;
            descriptor.profile      = profile;
            descriptor.state        = reinterpret_cast<void*>(L);
            descriptor.symbols      = table.data();
            descriptor.symbol_count = static_cast<uint32_t>(table.size());
            const char* why         = nullptr;
            if (open(&descriptor, &why) != 1) {
                error = why ? why : "the bridge refused the descriptor";
                return false;
            }
            return true;
        }

    }  // namespace

    attach_status attach_lua(lua::state L, const lua_api& api, lua_version version) {
        debugger::BootstrapConfig config;
        {
            auto context = ctx::get();
            std::lock_guard guard(context->mtx);
            if (context->refused) {
                // Discovery already decided this process stays undebugged (or the
                // carrier was rejected): drop the hook instead of re-running the
                // whole attach on every VM call.
                return attach_status::fatal;
            }
            if (!context->bootstrap_valid) {
                // The selected carrier was rejected: report it instead of
                // quietly connecting to the default socket.
                log::error("attach.lua skipped: invalid bootstrap config: {}", context->bootstrap_error);
                return attach_status::fatal;
            }
            config = context->bootstrap;
        }
        auto root = resolve_root(config);
        if (!root) {
            log::fatal("cannot locate the published root (no config root, no dso path)");
            return attach_status::fatal;
        }
        auto buf = readfile(*root / "script" / "attach.lua");
        if (api.loadbuffer(L, buf.data(), buf.size(), "=(attach.lua)")) {
            const char* message = api.tolstring(L, -1, nullptr);
            log::fatal("load attach.lua error: {}", message ? message : "(not a string)");
            api.settop(L, -2);
            return attach_status::fatal;
        }
        const std::string root_utf8 = debugger::path_to_utf8(*root);
        api.pushstring(L, root_utf8.c_str());
#ifdef _WIN32
        auto pid = std::to_string(GetCurrentProcessId());
#else
        auto pid = std::to_string(getpid());
#endif
        api.pushstring(L, pid.c_str());

        auto address = config.address.empty() ? ("@$tmp/luadbg_" + pid) : config.address;
        push_session_config(api, L, config, address);

        // Hand the already-bound module table to the script: the table came from
        // the source this launcher resolved, so the script must not re-enter the
        // bridge through an exported target symbol.
        {
            const int table = api.gettop(L);
            auto context    = ctx::get();
            resolved_symbols symbols;
            bool have_symbols = false;
            {
                std::lock_guard guard(context->mtx);
                if (context->resolved_ready) {
                    symbols      = context->resolved;
                    have_symbols = true;
                }
            }
            if (have_symbols && !symbols.entries.empty()) {
                // The profile must come from the enum->bridge mapping, never a cast:
                // the two enums order their members differently, and the wrong id
                // binds a different backend to a real state.
                const int profile = lua_version_profile(version);
                std::string error;
                if (profile < 0) {
                    error =
                        "the bridge handoff needs an identified ABI; an explicit "
                        "luaVersion is required";
                } else if (open_bridge(L, profile, symbols, bridge_path(config, *root), error)) {
                    // The bridge pushed its module table on top of the stack.
                    api.setfield(L, table, "nativeBridge");
                    error.clear();
                }
                if (!error.empty()) {
                    // A supplied source that cannot be applied is terminal: falling
                    // through would let the script bind whatever the target happens
                    // to export, which is the silent downgrade the contract forbids.
                    api.pushstring(L, error.c_str());
                    api.setfield(L, table, "nativeBridgeError");
                    log::error("bridge handoff failed: {}", error);
                    api.settop(L, -2);
                    return attach_status::fatal;
                }
            }
        }

        if (api.pcall(L, 3, 1, 0)) {
            /*
                这里失败无法调用log::fatal，因为无法知道调试器已经加载到哪一步才失败的。
                所以调试器不应该把错误抛到这里。
            */
            log::error("{}", api.tolstring(L, -1, nullptr));
            api.settop(L, -2);
            return attach_status::wait;
        }
        using namespace std::string_view_literals;
        const char* result = api.tolstring(L, -1, nullptr);
        bool ok            = result != nullptr && std::string_view(result) == "ok"sv;
        api.settop(L, -2);
        return ok ? attach_status::success : attach_status::wait;
    }
}

namespace luadebug::autoattach {

    namespace {

        // Entry used when the interpreter launches our script itself (no
        // launcher injection): `debugger.lua` asks this DSO for the profile it
        // detected, the DSO reads the bootstrap document from the environment,
        // resolves the source, and opens the bridge. No watchdog is installed and
        // no attach.lua runs, so nothing can recurse into another launch.
        int launcher_open(lua::state L, int profile) {
            // A release build compiles `log::info` out and only sends `fatal` to
            // the frontend socket, which does not exist in this launch shape. The
            // reason therefore goes to stderr as well: without it the script can
            // only say "the launcher refused", which is useless to a user.
            std::string reason;
            auto fail = [&reason](const std::string& why) {
                reason = why;
                log::error("{}", why);
                log::notify_frontend(why);
                return 0;
            };

            // This can be the first launcher code to run in the process.
            static std::once_flag once;
            std::call_once(once, [] { Gum::runtime_init(); });

            debugger::BootstrapConfig config;
            std::string error;
            const char* text = std::getenv("LUA_DEBUG_CONFIG");
            if (!text || !*text) {
                return fail("vm source: LUA_DEBUG_CONFIG is not set");
            }
            if (!debugger::parse_bootstrap(text, config, error)) {
                return fail("vm source: " + error);
            }
            resolved_symbols symbols;
            symbol_source_result result;
            if (!resolve_vm_source(config, symbols, error, &result)) {
                return fail("vm source: " + error);
            }
            auto root = resolve_root(config);
            if (!root) {
                return fail("vm source: cannot locate the published root");
            }
            // Only this profile's entry can be reached, so a conflicting
            // `luaVersion` is a caller error rather than something to guess about.
            if (!config.lua_version.empty()) {
                const int configured = debugger::profile_from_name(config.lua_version);
                if (configured >= 0 && configured != profile) {
                    error = "luaVersion " + config.lua_version + " does not match the running VM's entry point (" + debugger::profile_id_name(profile) + ")";
                }
            }
            if (error.empty() && open_bridge(L, profile, symbols, bridge_path(config, *root), error)) {
                return 1;
            }
            if (reason.empty()) {
                reason = error;
            }
            log::error("{}", reason);
            log::notify_frontend(reason);
            // One value on the stack so the script can raise the reason: the
            // table's own `lua_pushstring` needs no exported target symbol, and
            // without it the message still reached stderr above.
            auto pushstring = reinterpret_cast<void (*)(lua::state, const char*)>(
                symbols.find("lua_pushstring")
            );
            if (pushstring) {
                pushstring(L, reason.c_str());
                return 1;
            }
            return 0;
        }

    }  // namespace

}  // namespace luadebug::autoattach

extern "C" {
// Pre-main entry for the launch path: the target is started with this library
// in DYLD_INSERT_LIBRARIES / LD_PRELOAD, so dyld (or ld.so) runs our
// initializers before the interpreter creates its first Lua state. The
// bootstrap document arrives in LUA_DEBUG_CONFIG, exactly like the
// environment-only injection contract; nothing is read from the target after
// the hook is installed.
//
// (frida's Darwin injector cannot be used here: it maps the image without
// dyld, so neither our C++ initializers nor libc++'s have run - a gated
// pre-libc++ target aborts on the first operator new, and resuming a pre-dyld
// START_SUSPENDED process traps in dyld. See README for the measurements.)
#if !defined(_WIN32)
__attribute__((constructor)) static void luadebug_preload_entry() {
    // Only a launch that the adapter started explicitly preloads the launcher.
    // Without this marker a process that merely happens to carry
    // LUA_DEBUG_CONFIG in its environment (and dlopens the launcher for another
    // reason) would be injected silently.
    const char* preload = getenv("LUA_DEBUG_PRELOAD");
    const char* config  = getenv("LUA_DEBUG_CONFIG");
    if (!preload || !*preload || !config || !*config) {
        return;
    }
    // frida also runs an injected image's initializers, so the environment marker
    // alone cannot tell a preload from a frida-mapped image. A genuinely preloaded
    // image is in dyld's image list, which is exactly what dladdr proves; skipping
    // here keeps `data` the winner for injected attaches even when the target's
    // environment carries a (possibly invalid) bootstrap document.
    if (luadebug::debugger::runtime::platform_module_directory(
            reinterpret_cast<const void*>(&luadebug_preload_entry)
        )
            .empty()) {
        return;
    }
    if (luadebug::autoattach::already_bootstrapped()) {
        return;
    }
    luadebug::autoattach::bootstrap(nullptr);
}
#endif

// frida-core's library-injection entry point. The injector calls
//   void entry(const char* data, int32_t* unload_policy, void* injector_state)
// from a thread it owns (see frida-core 17.25.2
// src/*/frida-helper-backend-glue.*, which passes `data`, the address of the
// policy out-parameter, and the injector state block).
//
// `unload_policy` is frida-core's UnloadPolicy enum, declared in
// lib/base/session.vala as { IMMEDIATE = 0, RESIDENT = 1, DEFERRED = 2 }. The
// launcher must stay mapped for the whole session (hook trampolines and the
// Frida interceptor live in this image), so it always asks for RESIDENT.
DLLEXPORT void DLLEXPORT_DECLARATION luadebug_frida_entry(const char* data, int32_t* unload_policy, void* injector_state) {
    constexpr int32_t kUnloadPolicyResident = 1;
    (void)injector_state;
    if (unload_policy) {
        *unload_policy = kUnloadPolicyResident;
    }
    luadebug::autoattach::bootstrap(data);
}

// --- script entry points ----------------------------------------------------
//
// One per profile, selected by `debugger.lua` from the VM it just detected. They
// exist for the launch shape where the adapter cannot inject (a plain `luaexe`
// in a console or a terminal): the environment carries the bootstrap document
// and this DSO does the binding, so the target needs no exported Lua entry of
// its own.
#define LDB_LAUNCHER_ENTRY(profile_name_literal, profile_index)                           \
    DLLEXPORT int DLLEXPORT_DECLARATION luaopen_luadebug_launcher_##profile_name_literal( \
        std::uintptr_t L                                                                  \
    ) {                                                                                   \
        return luadebug::autoattach::launcher_open(L, profile_index);                     \
    }

LDB_LAUNCHER_ENTRY(lua51, 0)
LDB_LAUNCHER_ENTRY(lua52, 1)
LDB_LAUNCHER_ENTRY(lua53, 2)
LDB_LAUNCHER_ENTRY(lua54, 3)
LDB_LAUNCHER_ENTRY(lua55, 4)
LDB_LAUNCHER_ENTRY(luajit, 5)

#undef LDB_LAUNCHER_ENTRY
}
