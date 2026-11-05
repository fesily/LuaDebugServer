// Proves the runtime-sharing contract every bridge loaded from one path relies on:
//   * two bridges loaded from the same runtime path join one session hub and get
//     distinct worker ids (ids start at 1, then 2),
//   * unregistering one bridge leaves the other registered,
//   * a second, differently-pathed copy of the runtime is a *different* hub —
//     the hazard the published layout must never create.
//
// The bridges are stand-ins (test/bridge_shim.cpp) with no Lua inside; the point
// is the ABI and hub ownership, not the debuggee's interpreter.

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "runtime/abi.h"

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    using shim_start_fn    = uint64_t (*)();
    using shim_worker_fn   = uint64_t (*)();
    using shim_unreg_fn    = int (*)();
    using shim_register_fn = int (*)(uint64_t*);

    struct Shim {
        void* handle = nullptr;
        shim_start_fn start = nullptr;
        shim_worker_fn worker = nullptr;
        shim_unreg_fn unregister = nullptr;
        shim_register_fn register_again = nullptr;
    };

    bool load_shim(const std::string& path, Shim& out) {
        out.handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!out.handle) {
            std::fprintf(stderr, "dlopen(%s): %s\n", path.c_str(), ::dlerror());
            return false;
        }
        out.start    = reinterpret_cast<shim_start_fn>(::dlsym(out.handle, "ld_shim_start"));
        out.worker   = reinterpret_cast<shim_worker_fn>(::dlsym(out.handle, "ld_shim_worker"));
        out.unregister = reinterpret_cast<shim_unreg_fn>(::dlsym(out.handle, "ld_shim_unregister"));
        out.register_again = reinterpret_cast<shim_register_fn>(::dlsym(out.handle, "ld_shim_register_again"));
        return out.start && out.worker && out.unregister;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s RUNTIME SHIM_A SHIM_B\n", argv[0]);
        return 2;
    }
    const std::string runtime = argv[1];
    const std::string shim_a  = argv[2];
    const std::string shim_b  = argv[3];

    setenv("LUA_DEBUG_RUNTIME", runtime.c_str(), 1);

    Shim a;
    Shim b;
    if (!load_shim(shim_a, a) || !load_shim(shim_b, b)) {
        return 1;
    }

    uint64_t worker_a = a.start();
    uint64_t worker_b = b.start();
    expect(worker_a != 0, "bridge A registered");
    expect(worker_b != 0, "bridge B registered");
    expect(worker_a != worker_b, "both bridges are distinct workers");
    expect(worker_a == 1 && worker_b == 2,
           "worker ids are allocated in one hub (1 then 2)");

    // One hub also means one id space: a third registration from bridge A must
    // continue the sequence, not restart it.
    uint64_t third = 0;
    expect(a.register_again(&third) == LD_STATUS_OK && third == 3,
           "a third registration continues the same id space");

    expect(a.unregister() == LD_STATUS_OK, "bridge A unregisters");
    expect(b.worker() != 0, "bridge B is still registered");

    // A different path for the same library is a different hub: ids restart and
    // the session is not shared. The published layout must therefore contain
    // exactly one copy of the runtime per platform/arch.
    const char* tmpdir = std::getenv("TMPDIR");
    std::string dir    = tmpdir && *tmpdir ? tmpdir : "/tmp";
    if (dir.back() != '/') {
        dir.push_back('/');
    }
    auto basename = runtime.substr(runtime.find_last_of('/') + 1);
    std::string second = dir + "lua-debug-second-runtime-" + basename;
    {
        std::ifstream in(runtime, std::ios::binary);
        std::ofstream out(second, std::ios::binary | std::ios::trunc);
        if (!in || !out) {
            std::fprintf(stderr, "FAIL: cannot copy runtime to %s\n", second.c_str());
            return 1;
        }
        out << in.rdbuf();
    }
    setenv("LUA_DEBUG_RUNTIME", second.c_str(), 1);
    Shim c;
    if (!load_shim(shim_b, c)) {
        return 1;
    }
    uint64_t worker_c = c.start();
    expect(worker_c == 1, "a second runtime copy starts its own id space");

    setenv("LUA_DEBUG_RUNTIME", runtime.c_str(), 1);

    if (failures == 0) {
        std::printf("two_bridge_host: OK\n");
        return 0;
    }
    std::printf("two_bridge_host: %d failure(s)\n", failures);
    return 1;
}
