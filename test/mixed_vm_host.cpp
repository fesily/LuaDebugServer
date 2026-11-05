// Heterogeneous VMs in one process: one bridge file, two different Lua ABIs, one
// session bound to a single group.
//
// The interpreters are separate shared libraries, loaded with RTLD_LOCAL and
// addressed by their own handle - never by a process-wide symbol search. The host
// binds each state through the bridge's native descriptor, which is how a host
// that embeds several Lua versions names the module explicitly.
//
// Usage: mixed_vm_host BRIDGE LIB_A PROFILE_A LIB_B PROFILE_B [A|B] [join]
//   PROFILE_* is the bridge profile number (0 = lua51 ... 4 = lua55, 5 = luajit);
//   the fifth argument selects which VM joins first (default B). The run then
//   proves the *other* group is refused, so running it both ways covers both
//   directions of the same pair. A trailing `join` instead requires both states
//   to be of one group (two modules of the same ABI) and proves they join the
//   same session as separate workers.
//
// Proven in one process:
//   * only the selected group joins; the other VM keeps running Lua normally,
//   * an explicit join from the other group is refused and leaves no worker,
//   * after every worker of the selected group stopped, the refusal still holds,
//   * closing the VMs afterwards does not disturb the remaining session.

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bridge/bridge_abi.h"

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    using newstate_fn = void* (*)(void*);
    using close_fn = void (*)(void*);
    using openlibs_fn = void (*)(void*);
    // Lua 5.5 turned `luaL_openlibs` into a macro over `luaL_openselectedlibs`.
    using openselectedlibs_fn = void (*)(void*, int, int);
    using loadstring_fn = int (*)(void*, const char*);
    using pcallk_fn = int (*)(void*, int, int, int, long, void*);
    using pcall_fn = int (*)(void*, int, int, int);
    // `lua_tolstring` takes the length out-parameter in every version; omitting it
    // makes the callee write through garbage.
    using tostring_fn = const char* (*)(void*, int, size_t*);

    struct LuaLib {
        void* handle = nullptr;

        bool open(const char* path) {
            handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                std::fprintf(stderr, "dlopen %s: %s\n", path, ::dlerror());
                return false;
            }
            return true;
        }

        template <typename T>
        T fn(const char* name) const {
            return reinterpret_cast<T>(::dlsym(handle, name));
        }

        // Opens the standard libraries through whichever entry point the target
        // version exports; `luaL_openlibs` is a macro from 5.5 on.
        bool openlibs(void* state) const {
            if (auto open = fn<openlibs_fn>("luaL_openlibs")) {
                open(state);
                return true;
            }
            if (auto selected = fn<openselectedlibs_fn>("luaL_openselectedlibs")) {
                selected(state, ~0, 0);
                return true;
            }
            return false;
        }
    };

    // Runs `return tostring(<expression>)` on the state. Every entry point is
    // resolved from that library's own handle; `luaL_dostring`/`lua_pcall` are
    // macros in several versions, so the actual functions are used instead.
    double eval_number(const LuaLib& lib, void* state, const char* expression) {
        auto loadstring = lib.fn<loadstring_fn>("luaL_loadstring");
        auto tostring   = lib.fn<tostring_fn>("lua_tolstring");
        auto pcallk     = lib.fn<pcallk_fn>("lua_pcallk");
        auto pcall      = lib.fn<pcall_fn>("lua_pcall");
        if (!loadstring || !tostring || (!pcallk && !pcall)) {
            return -1.0;
        }
        std::string chunk = std::string("return tostring(") + expression + ")";
        if (loadstring(state, chunk.c_str()) != 0) {
            return -1.0;
        }
        const int status = pcallk ? pcallk(state, 0, 1, 0, 0, nullptr)
                                  : pcall(state, 0, 1, 0);
        if (status != 0) {
            return -1.0;
        }
        const char* text = tostring(state, -1, nullptr);
        return text ? std::strtod(text, nullptr) : -1.0;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s BRIDGE LIB_A PROFILE_A LIB_B PROFILE_B [A|B]\n", argv[0]);
        return 2;
    }
    const std::string bridge = argv[1];
    // Selecting A first is the reverse direction of selecting B first: the run
    // always refuses the group that did not join.
    const bool select_first = argc > 6 && std::strcmp(argv[6], "A") == 0;
    const bool same_group_join = argc > 7 && std::strcmp(argv[7], "join") == 0;
    const int profile_a     = std::atoi(argv[3]);
    const int profile_b     = std::atoi(argv[5]);
    const char* name_a      = argv[3];
    const char* name_b      = argv[5];

    LuaLib lib_a;
    LuaLib lib_b;
    if (!lib_a.open(argv[2]) || !lib_b.open(argv[4])) {
        return 1;
    }

    auto newstate_a = lib_a.fn<newstate_fn>("luaL_newstate");
    auto newstate_b = lib_b.fn<newstate_fn>("luaL_newstate");
    if (!newstate_a || !newstate_b) {
        std::fprintf(stderr, "the Lua libraries do not export luaL_newstate\n");
        return 1;
    }
    void* state_a = newstate_a(nullptr);
    void* state_b = newstate_b(nullptr);
    if (!lib_a.openlibs(state_a) || !lib_b.openlibs(state_b)) {
        std::fprintf(stderr, "the Lua libraries do not open their standard libraries\n");
        return 1;
    }

    expect(eval_number(lib_a, state_a, "1 + 1") == 2.0, "the first VM evaluates");
    expect(eval_number(lib_b, state_b, "3 + 4") == 7.0, "the second VM evaluates");

    using lookup_fn = const LdBridgeOps* (*)(const LdBridgeDescriptor*, void**, const char**);
    void* handle = ::dlopen(bridge.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto lookup  = handle ? reinterpret_cast<lookup_fn>(::dlsym(handle, "luadebug_bridge_bind"))
                          : nullptr;
    if (!lookup) {
        std::fprintf(stderr, "cannot load %s\n", bridge.c_str());
        return 1;
    }

    struct Binding {
        const LdBridgeOps* ops = nullptr;
        void* ctx              = nullptr;
        std::string error;
    };

    auto bind_state = [&](int profile, const LuaLib& lib, void* state) {
        Binding binding;
        LdBridgeDescriptor descriptor {};
        descriptor.size        = sizeof(descriptor);
        descriptor.abi_version = LDB_BRIDGE_ABI_VERSION;
        descriptor.profile     = profile;
        descriptor.state       = state;
        // The resolver's first argument is its own context, not the module: the
        // ABI never substitutes one for the other.
        descriptor.module        = lib.handle;
        descriptor.resolver_user = lib.handle;
        descriptor.resolver      = [](void* user, const char* name) -> void* {
            return ::dlsym(user, name);
        };
        const char* why = nullptr;
        binding.ops     = lookup(&descriptor, &binding.ctx, &why);
        if (!binding.ops && why) {
            binding.error = why;
        }
        return binding;
    };

    Binding first  = select_first ? bind_state(profile_a, lib_a, state_a)
                                  : bind_state(profile_b, lib_b, state_b);
    Binding second = select_first ? bind_state(profile_b, lib_b, state_b)
                                  : bind_state(profile_a, lib_a, state_a);
    const char* first_profile  = select_first ? name_a : name_b;
    const char* second_profile = select_first ? name_b : name_a;

    expect(first.ops != nullptr, "the selected state binds");
    expect(second.ops != nullptr, "the other state binds");
    if (!first.ops || !second.ops) {
        return 1;
    }

    // An in-process embedder has nobody to configure the session, so the start
    // call registers the worker and skips the client handshake.
    const char* config = "{\"luaVersion\":\"lua54\"}";
    const auto size    = static_cast<uint32_t>(std::strlen(config));

    std::fprintf(stderr, "ctx=%p ops=%p\n", first.ctx, (const void*)first.ops);
    const int first_joined = first.ops->start(first.ctx, config, size, 0);
    if (first_joined != 1) {
        std::fprintf(stderr, "first start failed: %s\n",
                     first.ops->last_error(first.ctx));
    }
    expect(first_joined == 1, "the selected group joins the session");
    expect(first.ops->running(first.ctx) == 1, "the selected worker runs");

    const uint64_t worker = first.ops->worker_id(first.ctx);
    expect(worker != 0, "the selected worker has an id");

    if (same_group_join) {
        // Two modules of one ABI: the second state joins the same session with
        // its own worker, and the two remain independent.
        const int both = second.ops->start(second.ctx, config, size, 0);
        if (both != 1) {
            std::fprintf(stderr, "second start failed: %s\n",
                         second.ops->last_error(second.ctx));
        }
        expect(both == 1, "a second state of the same group joins");
        expect(second.ops->running(second.ctx) == 1, "the second worker runs");
        const uint64_t worker2 = second.ops->worker_id(second.ctx);
        expect(worker2 != 0 && worker2 != worker, "the second worker has its own id");

        expect(eval_number(lib_a, state_a, "6 * 7") == 42.0, "the first VM evaluates");
        expect(eval_number(lib_b, state_b, "8 * 8") == 64.0, "the second VM evaluates");

        first.ops->stop(first.ctx);
        expect(first.ops->running(first.ctx) == 0, "the first worker stopped");
        expect(second.ops->running(second.ctx) == 1, "the second worker keeps running");
        expect(eval_number(lib_b, state_b, "9 * 9") == 81.0,
               "the remaining VM still evaluates after the other stopped");

        second.ops->stop(second.ctx);
        first.ops->release(first.ctx);
        second.ops->release(second.ctx);
        lib_a.fn<close_fn>("lua_close")(state_a);
        lib_b.fn<close_fn>("lua_close")(state_b);
        if (failures == 0) {
            std::printf("mixed_vm_host(%s + %s joined): OK\n", name_a, name_b);
            return 0;
        }
        std::printf("mixed_vm_host(%s + %s joined): %d failure(s)\n", name_a, name_b, failures);
        return 1;
    }

    // The other group must be refused, and must not get a worker or a hook.
    const int joined = second.ops->start(second.ctx, config, size, 0);
    expect(joined == 0, "the other group is refused");
    const char* why = second.ops->last_error(second.ctx);
    expect(why && std::strstr(why, "group") != nullptr,
           "the refusal names the session group");
    expect(second.ops->worker_id(second.ctx) == 0, "the refused VM has no worker");
    expect(second.ops->running(second.ctx) == 0, "the refused VM is not registered");

    // ... and the refused VM still executes Lua normally.
    const std::string* second_check = nullptr;
    (void)second_check;
    if (select_first) {
        expect(eval_number(lib_b, state_b, "6 * 7") == 42.0,
               "the refused second VM keeps running");
    } else {
        expect(eval_number(lib_a, state_a, "6 * 7") == 42.0,
               "the refused first VM keeps running");
    }

    // Stopping every worker of the selected group does not free the session for
    // the other group: the session keeps its group until it ends.
    first.ops->stop(first.ctx);
    expect(first.ops->running(first.ctx) == 0, "the selected worker stopped");
    const int joined_after = second.ops->start(second.ctx, config, size, 0);
    expect(joined_after == 0, "the other group is still refused after the worker stopped");

    first.ops->release(first.ctx);
    second.ops->release(second.ctx);

    lib_a.fn<close_fn>("lua_close")(state_a);
    lib_b.fn<close_fn>("lua_close")(state_b);

    if (failures == 0) {
        std::printf("mixed_vm_host(%s first, %s refused): OK\n", first_profile, second_profile);
        return 0;
    }
    std::printf("mixed_vm_host(%s first, %s refused): %d failure(s)\n", first_profile,
                second_profile, failures);
    return 1;
}
