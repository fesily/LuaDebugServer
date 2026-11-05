#pragma once

// Module identity for the VM binding.
//
// The bridge never binds through `RTLD_DEFAULT`, a process-wide symbol search,
// or "the first library that exports lua_gettop": the module comes from the
// address of the caller that entered the bridge (a Lua C function the target
// itself invoked), and every symbol is then resolved inside that one module.

#include <string>

namespace luadebug::vm {

    struct Module {
        void* handle = nullptr;  // borrowed; the bridge never closes it
        std::string path;        // canonical path, as the platform reports it

        bool valid() const {
            return handle != nullptr;
        }
    };

    // Module that contains `address` (pass a function pointer defined in it).
    Module module_by_address(const void* address);

    // Exact published path. Reuses an already-loaded image when there is one,
    // so the same file is never mapped twice.
    Module module_by_path(const std::string& path);

    // Symbol inside this module. Returns null when the module does not export
    // it - never a process-wide fallback.
    void* module_symbol(const Module& module, const char* name);

    // True when `address` lies inside `module`. Guards against ELF symbol
    // preemption: a resolved address that belongs to another image is not the
    // module's Lua implementation.
    bool module_contains(const Module& module, const void* address);

}  // namespace luadebug::vm
