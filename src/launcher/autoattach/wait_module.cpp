#include <autoattach/wait_module.h>

#include <frida-gum.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace luadebug::autoattach {
    namespace {
        // frida-gum's module registry owns the loader-side notifications: dyld's
        // image callbacks on Darwin, `r_debug`'s notifier (or a scan of the dynamic
        // linker's call sites) on the ELF platforms, and the loader's DLL
        // notification on Windows. `module-added` is therefore the portable
        // surface, and this file has no per-platform code at all.
        //
        // The signal arrives on the thread that maps the image, inside the dynamic
        // loader, so the callback never runs there: it is queued for the worker.
        //
        // Never destroyed: the worker may still be waiting on the condition
        // variable at process exit, and static destruction order is not something a
        // loader hook can respect.
        struct state {
            std::mutex mtx;
            std::condition_variable cv;
            std::deque<std::string> pending;
            loaded_t loaded = nullptr;
            // Bumped by every install. A worker belongs to the generation it was
            // started for: a superseded worker must not drain the new watch's queue,
            // and must not latch its own stop into the new watch.
            std::uint64_t generation = 0;
            bool stopped             = true;
        };

        state& shared() {
            static state* instance = new state();
            return *instance;
        }

        std::string path_of(GumModule* module) {
            const char* path = gum_module_get_path(module);
            if (path == nullptr) {
                path = gum_module_get_name(module);
            }
            return path != nullptr ? std::string(path) : std::string();
        }

        void on_module_added(GumModuleRegistry* /*registry*/, GumModule* module, gpointer /*data*/) {
            if (module == nullptr) {
                return;
            }
            // cloaked modules (frida's own mappings) are filtered by the registry
            // before it emits, so anything arriving here is a real candidate.
            std::string path = path_of(module);
            if (path.empty()) {
                return;
            }
            auto& st = shared();
            {
                std::lock_guard lock(st.mtx);
                if (st.stopped) {
                    return;
                }
                st.pending.push_back(std::move(path));
            }
            st.cv.notify_one();
        }

        void worker(std::uint64_t generation) {
            auto& st = shared();
            for (;;) {
                std::string path;
                loaded_t loaded = nullptr;
                {
                    std::unique_lock lock(st.mtx);
                    st.cv.wait(lock, [&st, generation]() {
                        return st.generation != generation || !st.pending.empty();
                    });
                    if (st.generation != generation) {
                        // A later install owns the watch now.
                        return;
                    }
                    path = std::move(st.pending.front());
                    st.pending.pop_front();
                    loaded = st.loaded;
                }
                if (loaded == nullptr) {
                    continue;
                }
                // A consumed image ends this watch: the caller owns the VM from here
                // on. The stop is committed only while this worker still owns the
                // generation, so a re-armed watch is not killed by its predecessor.
                if (loaded(path)) {
                    std::lock_guard lock(st.mtx);
                    if (st.generation == generation) {
                        st.stopped = true;
                        st.pending.clear();
                    }
                    st.cv.notify_all();
                    return;
                }
            }
        }

        // The registry is a process-wide singleton. Activation hooks the dynamic
        // loader through the interceptor, so gum must already be initialized; the
        // launcher's entry does that (`Gum::runtime_init`) before any discovery, and
        // every path into `wait_module` runs after it. The subscription is permanent
        // because dropping it would leave the loader hooked with no consumer.
        GumModuleRegistry* registry() {
            static std::once_flag once;
            static GumModuleRegistry* instance = nullptr;
            std::call_once(once, []() {
                GumModuleRegistry* reg = gum_module_registry_obtain();
                if (g_signal_connect(reg, "module-added", G_CALLBACK(on_module_added), nullptr) != 0) {
                    instance = reg;
                }
            });
            return instance;
        }
    }  // namespace

    bool wait_module(loaded_t loaded) {
        if (loaded == nullptr) {
            return false;
        }
        std::uint64_t generation = 0;
        {
            auto& st = shared();
            std::lock_guard lock(st.mtx);
            st.loaded  = loaded;
            st.stopped = false;
            generation = ++st.generation;
        }
        // Images mapped from here on are queued, even before the worker exists: the
        // queue is the hand-off between the loader thread and the worker.
        GumModuleRegistry* reg = registry();
        if (reg == nullptr) {
            auto& st = shared();
            std::lock_guard lock(st.mtx);
            st.stopped = true;
            st.loaded  = nullptr;
            return false;
        }
        // Outside both locks: the worker contends for `st.mtx` with the loader
        // thread, and for nothing else.
        std::thread(worker, generation).detach();
        return true;
    }
}
