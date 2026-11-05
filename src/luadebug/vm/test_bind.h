#pragma once

// Test hook: binds the module that contains this executable's own Lua symbols.
//
// The native tests embed a Lua interpreter directly (no DSO), so "the module" is
// the test binary itself. Everything else uses the same code path as a host that
// names its Lua module explicitly.

#include <string>

#include "vm/boot.h"

namespace luadebug::vm {

    struct TestBinding {
        Vm* vm       = nullptr;
        State* state = nullptr;
        std::string error;
    };

    // `symbol` must be an address inside the module that implements the C API
    // (pass the address of a `lua_*` function).
    inline TestBinding bind_test_module(lua_State* L, Profile profile, const void* symbol) {
        TestBinding binding;
        const Module module = module_by_address(symbol);
        if (!module.valid()) {
            binding.error = "cannot resolve the test module";
            return binding;
        }
        binding.vm = bind_module(profile, module, binding.error);
        if (binding.vm) {
            binding.state = binding.vm->state(L);
        }
        return binding;
    }

}  // namespace luadebug::vm
