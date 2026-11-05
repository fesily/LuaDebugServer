#include "host/platform_abi.h"

#include "common/path_utf8.h"

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <dlfcn.h>
#    include <unistd.h>
#endif

#if defined(__APPLE__)
#    include <mach-o/dyld.h>

#    include <cstdint>
#endif

namespace luadebug::debugger::runtime {

    void* platform_dlopen(const std::string& path) {
#if defined(_WIN32)
        return ::LoadLibraryA(path.c_str());
#else
        return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    }

    void* platform_dlsym(void* handle, const char* name) {
#if defined(_WIN32)
        return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return ::dlsym(handle, name);
#endif
    }

    void platform_dlclose(void* handle) {
#if defined(_WIN32)
        ::FreeLibrary(static_cast<HMODULE>(handle));
#else
        ::dlclose(handle);
#endif
    }

    std::string platform_dl_error() {
#if defined(_WIN32)
        return "LoadLibrary failed (" + std::to_string(::GetLastError()) + ")";
#else
        const char* message = ::dlerror();
        return message ? message : "unknown";
#endif
    }

    std::string platform_name() {
#if defined(_WIN32)
        return "win32-x64";
#elif defined(__APPLE__)
#    if defined(__aarch64__)
        return "darwin-arm64";
#    else
        return "darwin-x64";
#    endif
#elif defined(__linux__)
#    if defined(__aarch64__)
        return "linux-arm64";
#    else
        return "linux-x64";
#    endif
#else
        return "bsd-x64";
#endif
    }

    std::string platform_workdir() {
        std::string exe;
#if defined(_WIN32)
        char buffer[MAX_PATH * 4];
        DWORD size = ::GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
        if (size == 0) {
            return {};
        }
        exe.assign(buffer, size);
#elif defined(__APPLE__)
        char buffer[4096];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            return {};
        }
        exe.assign(buffer);
#else
        char buffer[4096];
        auto n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            return {};
        }
        buffer[n] = '\0';
        exe.assign(buffer);
#endif
        // <root>/bin/<exe> -> <root>. Plain string handling keeps this free of
        // std::filesystem, which is unavailable on the 10.12 deployment target
        // of the x86_64 runtime build.
        for (int i = 0; i < 2; ++i) {
            auto pos = exe.find_last_of("/\\");
            if (pos == std::string::npos) {
                return {};
            }
            exe.erase(pos);
        }
        return exe;
    }

    std::string platform_module_directory(const void* address_in_module) {
        std::string path;
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address_in_module), &module)) {
            return {};
        }
        // UTF-8, not the process code page: every consumer of this value feeds it
        // to path_from_utf8 or to a protocol field.
        wchar_t buffer[MAX_PATH * 4];
        DWORD size = ::GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
        if (size == 0 || size >= std::size(buffer)) {
            return {};
        }
        path = wide_to_utf8(buffer, size);
#else
        Dl_info info;
        if (::dladdr(address_in_module, &info) == 0 || !info.dli_fname) {
            return {};
        }
        path.assign(info.dli_fname);
#endif
        const auto pos = path.find_last_of("/\\");
        if (pos == std::string::npos) {
            return {};
        }
        return path.substr(0, pos);
    }

}  // namespace luadebug::debugger::runtime
