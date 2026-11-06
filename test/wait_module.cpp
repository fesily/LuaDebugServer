#include <dlfcn.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <lua.hpp>
#include <thread>
using namespace std::literals;

// Drives the launcher's frida entry point, then loads a Lua module *after* the
// entry returned: the point of this host is that the module-load listener must
// still install hooks for a VM created later (the `wait_module` path).
//
// argv[1] launcher.so, argv[2] lua shared library, argv[3] bootstrap json
// ("-" or "" for none: the launcher then has no document of its own and falls
// back to LUA_DEBUG_CONFIG, which this host clears). The marker exists because a
// test runner cannot always pass an empty argument through.
int main(int argc, char** argv) {
    if (argc != 4) {
        return -1;
    }
    auto launcher = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!launcher) {
        printf("Error: %s\n", dlerror());
        return 1;
    }
    using entry_t = void (*)(const char*, int32_t*, void*);
    auto entry    = (entry_t)dlsym(launcher, "luadebug_frida_entry");
    if (!entry) {
        printf("Error: %s\n", dlerror());
        return 1;
    }
    // The unload policy is asserted for both carrier shapes: a null data pointer
    // (environment-only / defaults) must ask for a resident image just like an
    // explicit document, otherwise the launcher can be unloaded under the hooks.
    int32_t unload_policy  = 0;
    const bool no_document = !argv[3] || argv[3][0] == '\0' || (argv[3][0] == '-' && argv[3][1] == '\0');
    if (no_document) {
        // Passing no document means "whatever the environment says" to the
        // launcher, and a leftover LUA_DEBUG_CONFIG from a manual preload run
        // would silently pick a different VM than the one loaded below.
        unsetenv("LUA_DEBUG_CONFIG");
        entry(nullptr, &unload_policy, nullptr);
    } else {
        entry(argv[3], &unload_policy, nullptr);
    }
    if (unload_policy != 1) {
        printf("Error: expected resident unload policy, got %d\n", unload_policy);
        return 4;
    }
    auto hmod = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!hmod) {
        printf("Error: %s\n", dlerror());
        return 1;
    }
    auto _luaL_newstate = (decltype(&luaL_newstate))dlsym(hmod, "luaL_newstate");
    auto _lua_gethook   = (decltype(&lua_gethook))dlsym(hmod, "lua_gethook");
    auto _lua_pushnil   = (decltype(&lua_pushnil))dlsym(hmod, "lua_pushnil");
    auto _lua_settop    = (decltype(&lua_settop))dlsym(hmod, "lua_settop");
    if (!_luaL_newstate || !_lua_gethook || !_lua_pushnil || !_lua_settop) {
        printf("Error: %s does not export the Lua 5.1 C API\n", argv[2]);
        return 1;
    }
    // Discovery and hook installation run on the watcher's worker thread after the
    // dlopen, so waiting is unavoidable; waiting with a fixed sleep is not, because
    // a slow runner then fails as if the watcher were broken. Poll a watch point
    // instead: the first watched call on a state is what installs the hook.
    auto L               = _luaL_newstate();
    const auto deadline  = std::chrono::steady_clock::now() + 30s;
    while (_lua_gethook(L) == nullptr) {
        if (std::chrono::steady_clock::now() > deadline) {
            printf("Error: no hook was installed for the late-loaded VM\n");
            return 3;
        }
        _lua_pushnil(L);
        _lua_settop(L, 0);
        std::this_thread::sleep_for(20ms);
    }
    return 0;
}
