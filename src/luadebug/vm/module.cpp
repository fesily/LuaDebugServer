#include "vm/module.h"

#include <cstdlib>

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <dlfcn.h>
#    include <limits.h>
#endif

namespace luadebug::vm {

    namespace {

#if defined(_WIN32)
        std::string module_path(HMODULE module) {
            char buffer[MAX_PATH * 4];
            const DWORD size = ::GetModuleFileNameA(module, buffer, static_cast<DWORD>(sizeof(buffer)));
            if (size == 0) {
                return {};
            }
            return std::string(buffer, size);
        }

        void* load_exact(const std::string& path) {
            // RTLD_NOLOAD equivalent: an already mapped image is reused, never
            // a second one. GetModuleHandle on the full path does that.
            HMODULE module = ::GetModuleHandleA(path.c_str());
            if (module) {
                return module;
            }
            return ::LoadLibraryA(path.c_str());
        }
#else
        std::string canonical(const char* path) {
            if (!path || !*path) {
                return {};
            }
            char buffer[PATH_MAX];
            if (::realpath(path, buffer)) {
                return std::string(buffer);
            }
            return std::string(path);
        }

        void* load_exact(const std::string& path) {
            // RTLD_NOLOAD: reuse the mapping when the image is already loaded,
            // otherwise load exactly this file.
            if (void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
                return handle;
            }
            ::dlerror();
            return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        }
#endif

    }  // namespace

    Module module_by_address(const void* address) {
        Module module;
        if (!address) {
            return module;
        }
#if defined(_WIN32)
        HMODULE handle = nullptr;
        if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &handle)) {
            return module;
        }
        module.handle = handle;
        module.path   = module_path(handle);
#else
        Dl_info info;
        if (::dladdr(address, &info) == 0 || !info.dli_fname) {
            return module;
        }
        module.path = canonical(info.dli_fname);
        if (module.path.empty()) {
            return module;
        }
        // The image is mapped by definition (we are executing in it); NOLOAD
        // must succeed. A failure means we cannot address its symbols without
        // a process-wide search, which this contract forbids.
        module.handle = ::dlopen(module.path.c_str(), RTLD_NOW | RTLD_NOLOAD);
#    if defined(__linux__)
        if (!module.handle) {
            // glibc refuses to `dlopen` an ET_DYN executable ("cannot dynamically
            // load position-independent executable"), so the main program - the
            // shape every embedded-Lua host has - would have no handle at all.
            // `dlopen(NULL)` is its handle; `module_symbol` keeps the search inside
            // this image by checking containment on every hit.
            const std::string self = canonical("/proc/self/exe");
            if (!self.empty() && self == module.path) {
                module.handle = ::dlopen(nullptr, RTLD_NOW);
            }
        }
#    endif
        if (!module.handle) {
            module.handle = nullptr;
            module.path.clear();
        }
#endif
        return module;
    }

    Module module_by_path(const std::string& path) {
        Module module;
        if (path.empty()) {
            return module;
        }
        module.handle = load_exact(path);
        if (!module.handle) {
            return module;
        }
#if defined(_WIN32)
        module.path = module_path(static_cast<HMODULE>(module.handle));
        if (module.path.empty()) {
            module.path = path;
        }
#else
        Dl_info info;
        if (::dladdr(module.handle ? ::dlsym(module.handle, "lua_newstate") : nullptr, &info) != 0 && info.dli_fname) {
            module.path = canonical(info.dli_fname);
        }
        if (module.path.empty()) {
            module.path = path;
        }
#endif
        return module;
    }

    void* module_symbol(const Module& module, const char* name) {
        if (!module.valid() || !name) {
            return nullptr;
        }
#if defined(_WIN32)
        return reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(module.handle), name)
        );
#elif defined(__linux__)
        void* symbol = ::dlsym(module.handle, name);
        if (!symbol) {
            return nullptr;
        }
        // `dlopen(NULL)` (the main program's handle) searches the whole global
        // scope, dependencies included. Returning a dependency's `lua_*` would bind
        // one image's state to another image's API, so a hit outside this module is
        // rejected instead of being handed on.
        if (!module_contains(module, symbol)) {
            return nullptr;
        }
        return symbol;
#else
        return ::dlsym(module.handle, name);
#endif
    }

    bool module_contains(const Module& module, const void* address) {
        if (!module.valid() || !address) {
            return false;
        }
#if defined(_WIN32)
        HMODULE handle = nullptr;
        if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &handle)) {
            return false;
        }
        return handle == static_cast<HMODULE>(module.handle);
#else
        Dl_info info;
        if (::dladdr(address, &info) == 0 || !info.dli_fname) {
            return false;
        }
        return canonical(info.dli_fname) == module.path;
#endif
    }

}  // namespace luadebug::vm
