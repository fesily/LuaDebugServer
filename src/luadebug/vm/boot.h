#pragma once

// Binding a target VM to the bridge.
//
// The bridge is one shared library for every supported Lua, so the target is
// identified by what its C API *is*, not by a name it happens to export. Three
// sources can describe that API, and a caller may use any of them:
//
//   * an explicit symbol table (works for an image that exports nothing),
//   * a module handle to search,
//   * a resolver callback with the caller's own context.
//
// Binding identity is the resolved profile plus the full set of resolved
// function addresses: two modules of the same version keep separate bindings,
// and two descriptions that resolve to the same addresses share one.

#include <cstddef>
#include <string>

#include "vm/module.h"
#include "vm/vm.h"

namespace luadebug::vm {

    // One externally supplied symbol.
    struct SymbolDef {
        const char* name;
        void* address;
    };

    // Everything that identifies the target VM. At least one of `symbols`,
    // `module` and `resolver` must be present.
    struct VmSpec {
        // ABI/layout profile, or -1 to detect it from the evidence.
        int profile = -1;
        Module module;
        const SymbolDef* symbols = nullptr;
        std::size_t symbol_count = 0;
        // Called as `resolver(resolver_user, name)`.
        Resolver resolver   = nullptr;
        void* resolver_user = nullptr;
    };

    // Binds `spec` and adopts `state` into the resulting VM. Returns null and
    // fills `error` when the spec cannot describe that profile: a missing API
    // entry, evidence that contradicts the requested profile, or a response so
    // incomplete that the version cannot be decided. Nothing is installed on the
    // target in that case.
    Vm* bind_spec(const VmSpec& spec, lua_State* state, std::string& error);

    // Binds (or returns the existing binding for) `module` at `profile`.
    Vm* bind_module(Profile profile, const Module& module, std::string& error);

    // Convenience: bind the module that contains `address`.
    Vm* bind_module_at(Profile profile, const void* address, std::string& error);

    // Drops the binding for `vm` (session teardown). `vm` must not be in use.
    void release_module(Vm* vm);

}  // namespace luadebug::vm
