#include "frontend/injector_backend.h"

#include <cstdlib>

#include "common/log.h"
#include "frontend/platform.h"

namespace luadebug::debugger::frontend {

    const char* injector_backend_name(InjectorBackend backend) {
        switch (backend) {
        case InjectorBackend::lldb:
            return "lldb";
        case InjectorBackend::frida:
        default:
            return "frida";
        }
    }

    namespace {

        bool lldb_supported() {
            return host_os() == HostOs::macos;
        }

        bool parse_backend(const std::string& text, InjectorBackend& backend) {
            if (text.empty() || text == "auto") {
                return false;
            }
            if (text == "frida") {
                backend = InjectorBackend::frida;
                return true;
            }
            if (text == "lldb") {
                backend = InjectorBackend::lldb;
                return true;
            }
            return false;
        }

        // The adapter's own architecture, as a bridge platform suffix. Never the
        // target's: a fat adapter holds both slices while the target runs one.
        bool own_arch_is_x64() {
            return host_arch() == "x86_64";
        }

    }  // namespace

    bool resolve_injector_backend(const Json& arguments, const std::string& target_arch, InjectorBackend& backend, bool& automatic, std::string& error) {
        automatic = false;

        const std::string configured(get_string(arguments, "inject"));
        if (!configured.empty() && configured != "auto") {
            if (!parse_backend(configured, backend)) {
                error = "unknown `inject` value `" + configured + "`; use `frida`, `lldb`, `auto` or `none`.";
                return false;
            }
            if (backend == InjectorBackend::lldb && !lldb_supported()) {
                error = "`inject: \"lldb\"` is only available on macOS";
                return false;
            }
            return true;
        }

        if (const char* from_env = std::getenv("LUA_DEBUG_INJECTOR")) {
            if (*from_env && !parse_backend(from_env, backend)) {
                error = std::string("unknown LUA_DEBUG_INJECTOR value `") + from_env + "`; use `frida`, `lldb` or `auto`.";
                return false;
            }
            if (*from_env) {
                if (backend == InjectorBackend::lldb && !lldb_supported()) {
                    error = "LUA_DEBUG_INJECTOR=lldb is only available on macOS";
                    return false;
                }
                log_info("frontend: injector backend %s (LUA_DEBUG_INJECTOR)", injector_backend_name(backend));
                return true;
            }
        }

        // Default: frida when the two architectures match, lldb when they do
        // not. An unknown target architecture keeps the default rather than
        // guessing: frida reports the mismatch itself when there is one.
        automatic = true;
        if (lldb_supported() && !target_arch.empty()) {
            const bool target_is_x64 = target_arch == "x64";
            if (target_is_x64 != own_arch_is_x64()) {
                backend = InjectorBackend::lldb;
                return true;
            }
        }
        backend = InjectorBackend::frida;
        return true;
    }

}  // namespace luadebug::debugger::frontend
