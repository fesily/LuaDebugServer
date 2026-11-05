#include "host/loader.h"

#include <string>

#include "host/platform_abi.h"

namespace luadebug::debugger::runtime {

    const char* runtime_library_name() {
#if defined(_WIN32)
        return "luadebug_runtime.dll";
#elif defined(__APPLE__)
        return "libluadebug_runtime.dylib";
#else
        return "libluadebug_runtime.so";
#endif
    }

    std::string default_runtime_directory() {
        auto root = platform_workdir();
        if (root.empty()) {
            return {};
        }
        std::string dir = root;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
            dir.push_back('/');
        }
        dir += "runtime/";
        dir += platform_name();
        return dir;
    }

    bool bind_runtime(const std::string& path, BoundRuntime& out, std::string& error) {
        if (!out.handle) {
            out.handle = platform_dlopen(path);
        }
        if (!out.handle) {
            error = "cannot load " + path + ": " + platform_dl_error();
            return false;
        }
        auto get_api = reinterpret_cast<LdStatus (*)(uint32_t, LdRuntimeApi*)>(
            platform_dlsym(out.handle, "luadebug_runtime_get_api")
        );
        if (!get_api) {
            error = "cannot find " + path + ": luadebug_runtime_get_api";
            platform_dlclose(out.handle);
            out.handle = nullptr;
            return false;
        }
        LdRuntimeApi api {};
        api.size    = sizeof(LdRuntimeApi);
        api.version = LD_RUNTIME_ABI_VERSION;
        auto status = get_api(LD_RUNTIME_ABI_VERSION, &api);
        if (status != LD_STATUS_OK) {
            error = "luadebug runtime ABI mismatch";
            platform_dlclose(out.handle);
            out.handle = nullptr;
            return false;
        }
        out.api  = api;
        out.path = path;
        return true;
    }

    bool bind_default_runtime(BoundRuntime& out, std::string& error) {
        auto dir = default_runtime_directory();
        if (dir.empty()) {
            error = "cannot resolve the published root";
            return false;
        }
        std::string path = dir;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path.push_back('/');
        }
        path += runtime_library_name();
        return bind_runtime(path, out, error);
    }

    void unbind(BoundRuntime& runtime) {
        if (runtime.handle) {
            platform_dlclose(runtime.handle);
        }
        runtime = BoundRuntime {};
    }

}  // namespace luadebug::debugger::runtime
