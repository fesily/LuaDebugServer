#pragma once

// The bootstrap contract handed to the launcher inside the target process.
//
// Both carriers use the very same UTF-8 JSON document:
//   * frida-core's injector `data` argument (normal frontend path), and
//   * the target's LUA_DEBUG_CONFIG environment variable (environment-only
//     injection, e.g. a host process that injects on its own).
//
// The frontend and the launcher share this file so validation can never drift
// apart: whatever the frontend accepts, the launcher accepts, and vice versa.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common/vm_source.h"

namespace luadebug::debugger {

    enum class BootstrapMode {
        launch,
        attach,
    };

    struct BootstrapConfig {
        // Controls the launcher's hook mode. It is not the same thing as
        // `dbg:attach` (which only joins an existing session): even mode=attach
        // enters the target through `dbg:start`.
        BootstrapMode mode = BootstrapMode::attach;
        // Session address. Empty means "@$tmp/luadbg_<target pid>".
        std::string address;
        // Interpreted from the target's point of view: false = the target
        // listens and the adapter connects, true = the target connects.
        bool client = false;
        // ABI constraint for the target VM ("lua54", "lua-latest", ...; normalized
        // "lua-latest" -> lua55). An explicit value is a constraint the binding
        // must satisfy, not a hint. Empty means unspecified: the VM is detected
        // from evidence, and "auto" is accepted as another spelling of empty.
        std::string lua_version;
        bool ansi = false;
        // Bridge runtime platform (e.g. darwin-x64). Empty means "detect from
        // the target"; the adapter's own architecture must never override it.
        std::string platform;
        // Directory that holds `script/` and `runtime/`. Frontends set it
        // because frida maps the launcher with its own Mach-O loader, so the
        // image has no dyld path and dladdr() cannot report where it came from.
        // When absent the launcher falls back to its own DSO path (a plain
        // dlopen/LoadLibrary, e.g. the dlopen-based hosts and tests).
        std::string root;

        // --- optional VM source ------------------------------------------
        // Absent when the target VM must be discovered from its module. These
        // three are the only way to name a VM that exports nothing; every field
        // is independent, and none of them implies another.
        //
        // Loaded image path or file name of the debuggee's Lua implementation.
        std::string lua_binary;
        // Inline table. `dump_bootstrap` refuses a document over
        // kMaxBootstrapBytes, so a caller with a large table writes it to a file
        // and sets `lua_symbols_file` instead.
        std::vector<SymbolEntry> lua_symbols;
        // A UTF-8 JSON file the *target* can read, holding the same object.
        std::string lua_symbols_file;

        // True when any part of the VM source was provided. An external source
        // never silently degrades into discovery.
        bool has_vm_source() const {
            return !lua_binary.empty() || !lua_symbols.empty() || !lua_symbols_file.empty();
        }
    };

    // Upper bound for the serialized document. frida-core stores entrypoint data
    // in a fixed buffer on Darwin (entrypoint_data_storage, 4096 bytes, asserted
    // before the copy), so a larger document can never be delivered.
    constexpr std::size_t kMaxBootstrapBytes = 4095;

    // Parses and validates one carrier document. Absent fields keep the defaults
    // above; a present field with the wrong type, an unknown mode, an empty
    // string, or an embedded NUL is an error. `{}` is valid.
    bool parse_bootstrap(std::string_view text, BootstrapConfig& out, std::string& error);

    // Serializes the contract for delivery through frida-core's `data` argument.
    // Returns an empty string when the result would exceed kMaxBootstrapBytes.
    std::string dump_bootstrap(const BootstrapConfig& config);

}  // namespace luadebug::debugger
