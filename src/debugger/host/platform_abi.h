#pragma once

// Minimal platform shims used by the runtime loader (and later by the bridge
// side of the ABI): dynamic-library access and published-root discovery.
// Kept separate so both the adapter and the target-side code can share it
// without depending on the frontend.

#include <string>

namespace luadebug::debugger::runtime {

    void* platform_dlopen(const std::string& path);
    void* platform_dlsym(void* handle, const char* name);
    void platform_dlclose(void* handle);
    std::string platform_dl_error();

    // Platform directory name used under `<root>/runtime`, e.g. darwin-arm64.
    std::string platform_name();

    // `<exe dir>/..` — the published root of the adapter binary.
    std::string platform_workdir();

    // Directory of the dynamic library that contains `address_in_module`
    // (pass the address of a function defined in that library). Empty when the
    // platform cannot answer.
    std::string platform_module_directory(const void* address_in_module);

}  // namespace luadebug::debugger::runtime
