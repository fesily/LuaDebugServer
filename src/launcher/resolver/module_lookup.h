#pragma once

// Module lookup on top of frida-gum 17.
//
// Up to 16 the C API accepted a module *name* wherever it wanted a module
// (`gum_module_find_export_by_name`, `gum_module_enumerate_symbols`); 17 requires
// a `GumModule *`, obtained from `gum_process_find_module_by_name`, which matches
// the file name only. Our callers hold a full path, so this resolves a module by
// path first and falls back to its file name, then hands the module to `fn`.

#include <gumpp.hpp>

#include <string_view>

namespace luadebug {

    // True when a module with that path or file name exists; `fn` is called with
    // it while the enumeration is still valid.
    template <typename Fn>
    bool with_module(std::string_view name_or_path, Fn&& fn) {
        if (name_or_path.empty()) {
            return false;
        }
        bool found = false;
        Gum::Process::enumerate_modules([&](const Gum::Module& module) -> bool {
            const char* path = module.path();
            const char* name = module.name();
            const bool match = (path && name_or_path == path) || (name && name_or_path == name);
            if (!match) {
                return true;
            }
            found = true;
            fn(module);
            return false;
        });
        return found;
    }

}  // namespace luadebug
