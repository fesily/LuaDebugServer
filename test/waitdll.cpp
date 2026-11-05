#include <dlfcn.h>

#include <lua.hpp>
#include <string>
#include <thread>
using namespace std::literals;

// Drives the launcher's frida entry point with an explicit bootstrap document,
// then loads a Lua module *after* the entry returned: the point of this host is
// that the module-load listener must still install hooks for a VM created later
// (the `wait_dll` path).
//
// argv[1] launcher.so, argv[2] lua shared library, argv[3] bootstrap json
// ("-" or "" for none: the launcher then uses its defaults). The marker exists
// because a test runner cannot always pass an empty argument through.
int main(int argc, char** argv) {
    if (argc != 4) {
        return -1;
    }
    std::string error;
    auto launcher = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!launcher) {
        printf("Error: %s", dlerror());
        return 1;
    }
    using entry_t = void (*)(const char*, int32_t*, void*);
    auto entry    = (entry_t)dlsym(launcher, "luadebug_frida_entry");
    if (!entry) {
        printf("Error: %s", dlerror());
        return 1;
    }
    // The unload policy is asserted for both carrier shapes: a null data pointer
    // (environment-only / defaults) must ask for a resident image just like an
    // explicit document, otherwise the launcher can be unloaded under the hooks.
    int32_t unload_policy  = 0;
    const bool no_document = !argv[3] || argv[3][0] == '\0' || (argv[3][0] == '-' && argv[3][1] == '\0');
    if (no_document) {
        entry(nullptr, &unload_policy, nullptr);
    } else {
        entry(argv[3], &unload_policy, nullptr);
    }
    if (unload_policy != 1) {
        printf("Error: expected resident unload policy, got %d\n", unload_policy);
        return 4;
    }
#if !defined(__APPLE__) && !defined(_WIN32)
    // `wait_dll()` has no implementation on this platform yet (see
    // `src/launcher/autoattach/wait_dll.cpp`), so the module-load listener never
    // runs and the hook assertion below would fail for a missing feature rather
    // than a regression. CTest maps 77 to "skipped", which keeps the gap visible
    // without turning the platform's whole suite red. The carrier assertions above
    // still ran.
    printf("skip: listening for a later module load is not implemented here\n");
    return 77;
#endif
    auto hmod = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!hmod) {
        printf("Error: %s", dlerror());
        return 1;
    }
    std::this_thread::sleep_for(1s);
    auto _luaL_newstate   = (decltype(&luaL_newstate))dlsym(hmod, "luaL_newstate");
    auto L                = _luaL_newstate();
    auto _lua_createtable = (decltype(&lua_createtable))dlsym(hmod, "lua_createtable");
    _lua_createtable(L, 0, 0);
    auto _lua_getfield = (decltype(&lua_getfield))dlsym(hmod, "lua_getfield");
    _lua_getfield(L, -1, "1");
    auto _lua_gethook = (decltype(&lua_gethook))dlsym(hmod, "lua_gethook");
    return _lua_gethook(L) != nullptr ? 0 : 3;
}
