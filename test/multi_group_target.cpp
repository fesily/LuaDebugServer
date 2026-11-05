// A stand-in application that embeds Lua itself, so the launcher's automatic
// discovery can be exercised against a real process with more than one ABI.
//
// It never links Lua: each VM is a shared library opened with RTLD_LOCAL and
// addressed through its own handle, exactly like an embedder that ships several
// interpreter versions. The program keeps both VMs busy with plain C API calls
// (the calls a launcher can observe) until a control file says to stop, and
// records a heartbeat per VM so the test can prove a VM kept running after the
// debugger refused to hook it.
//
// Usage: multi_group_target LIB_A [LIB_B] --dir DIR [--load-second]
//   * LIB_B omitted: the second VM is opened only when DIR/load-second appears,
//     which covers "another group loads after the debugger attached".
//   * --load-second: both VMs exist before the debugger is injected.

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

    using newstate_fn = void* (*)(void*);
    using close_fn = void (*)(void*);
    using openlibs_fn = void (*)(void*);
    using openselectedlibs_fn = void (*)(void*, int, int);
    using top_fn = int (*)(void*);
    using settop_fn = void (*)(void*, int);
    using type_fn = int (*)(void*, int);
    using pushlstring_fn = void (*)(void*, const char*, size_t);
    using loadstring_fn = int (*)(void*, const char*);
    using pcall_fn = int (*)(void*, int, int, int);

    struct Vm {
        void* handle        = nullptr;
        void* state         = nullptr;
        settop_fn settop    = nullptr;
        type_fn type        = nullptr;
        pushlstring_fn push = nullptr;
        loadstring_fn load  = nullptr;
        pcall_fn pcall      = nullptr;
        std::string name;

        bool open(const char* path, const char* label) {
            name   = label;
            handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                std::fprintf(stderr, "dlopen %s: %s\n", path, ::dlerror());
                return false;
            }
            auto newstate = reinterpret_cast<newstate_fn>(::dlsym(handle, "luaL_newstate"));
            settop        = reinterpret_cast<settop_fn>(::dlsym(handle, "lua_settop"));
            type          = reinterpret_cast<type_fn>(::dlsym(handle, "lua_type"));
            push          = reinterpret_cast<pushlstring_fn>(::dlsym(handle, "lua_pushlstring"));
            load          = reinterpret_cast<loadstring_fn>(::dlsym(handle, "luaL_loadstring"));
            pcall         = reinterpret_cast<pcall_fn>(::dlsym(handle, "lua_pcall"));
            auto top      = reinterpret_cast<top_fn>(::dlsym(handle, "lua_gettop"));
            if (!newstate || !settop || !type || !push || !top) {
                std::fprintf(stderr, "%s does not export the C API\n", label);
                return false;
            }
            auto openlibs = reinterpret_cast<openlibs_fn>(::dlsym(handle, "luaL_openlibs"));
            state         = newstate(nullptr);
            if (!state) {
                return false;
            }
            if (openlibs) {
                openlibs(state);
            } else if (auto selected =
                           reinterpret_cast<openselectedlibs_fn>(
                               ::dlsym(handle, "luaL_openselectedlibs"))) {
                selected(state, ~0, 0);
            }
            return true;
        }

        // One iteration of ordinary work: run a small script (a real embedder
        // runs the host program's Lua code - without bytecode executing, no hook
        // event exists for a debugger to interrupt), plus a few exported C API
        // calls, which is what a launcher can hook in an already running VM.
        void tick() const {
            push(state, name.c_str(), name.size());
            (void)type(state, -1);
            if (load && pcall
                && load(state, "local s = 0 for i = 1, 200 do s = s + i end return s") == 0) {
                (void)pcall(state, 0, 1, 0);
            }
            // Back to an empty stack: `-1` would keep the pushed string forever.
            settop(state, 0);
        }

        void close() {
            if (handle && state) {
                if (auto fn = reinterpret_cast<close_fn>(::dlsym(handle, "lua_close"))) {
                    fn(state);
                }
            }
            state = nullptr;
        }
    };

    bool exists(const std::string& path) {
        std::ifstream in(path);
        return in.good();
    }

    void beat(const std::string& dir, const char* name, unsigned long n) {
        std::ofstream out(dir + "/alive_" + name, std::ios::trunc);
        out << n;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s LIB_A [LIB_B] --dir DIR [--load-second]\n", argv[0]);
        return 2;
    }
    const char* lib_a    = argv[1];
    const char* lib_b    = nullptr;
    std::string dir;
    bool load_second_now = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (std::strcmp(argv[i], "--load-second") == 0) {
            load_second_now = true;
        } else if (!lib_b && argv[i][0] != '-') {
            lib_b = argv[i];
        }
    }
    if (dir.empty()) {
        std::fprintf(stderr, "--dir is required\n");
        return 2;
    }

    Vm a;
    Vm b;
    if (!a.open(lib_a, "a")) {
        return 1;
    }
    if (load_second_now) {
        if (!lib_b || !b.open(lib_b, "b")) {
            return 1;
        }
    }

    // Each VM runs on its own thread, like a real embedder that serves several
    // interpreters: a debugger stopping one VM must leave the other executing.
    std::atomic<bool> quit { false };

    auto run_vm = [&quit, &dir](Vm* vm) {
        unsigned long ticks = 0;
        while (!quit.load(std::memory_order_relaxed)) {
            vm->tick();
            beat(dir, vm->name.c_str(), ticks);
            ++ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    // The test removes `stop` before starting; its presence ends the program.
    std::vector<std::thread> workers;
    Vm* second = nullptr;
    if (b.state) {
        second = &b;
    }
    workers.emplace_back(run_vm, &a);
    if (second) {
        workers.emplace_back(run_vm, second);
    }

    while (!exists(dir + "/stop")) {
        if (!second && lib_b && exists(dir + "/load-second")) {
            // A second ABI shows up while the debugger is already attached.
            if (!b.open(lib_b, "b")) {
                quit = true;
                break;
            }
            second = &b;
            workers.emplace_back(run_vm, second);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    quit = true;
    for (auto& worker : workers) {
        worker.join();
    }
    b.close();
    a.close();
    return 0;
}
