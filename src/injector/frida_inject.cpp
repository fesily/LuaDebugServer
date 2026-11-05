#include "frida_inject.h"

#include <frida-core.h>

#include <mutex>
#include <vector>

#include <cstdio>

namespace luadebug::injector {

    namespace {

        // `uninjected` is the only proof that the entry point of *our* injection
        // finished: the synchronous inject call returns as soon as the library is
        // loaded (frida sends READY before the entry point runs, and BYE after it
        // returns). The signal may also arrive before the sync call hands us the
        // id, so every id is buffered and compared afterwards.
        struct WaitState {
            std::mutex mtx;
            std::vector<unsigned int> received;
            unsigned int wanted = 0;
            bool done           = false;
            // Set while the bounded wait runs, so a signal arriving on frida's own
            // context can wake it instead of letting it idle until the deadline.
            GMainLoop* loop = nullptr;
        };

        // Runs on whatever thread frida delivers the signal from; `g_main_loop_quit`
        // is safe to call from another thread.
        void on_uninjected(FridaInjector* /*injector*/, guint id, gpointer user_data) {
            auto* state        = static_cast<WaitState*>(user_data);
            GMainLoop* to_quit = nullptr;
            {
                std::lock_guard guard(state->mtx);
                state->received.push_back(id);
                if (id == state->wanted && !state->done) {
                    state->done = true;
                    to_quit     = state->loop;
                }
            }
            if (to_quit) {
                g_main_loop_quit(to_quit);
            }
        }

        // Timeout source for the bounded wait. It must not be removed after it
        // already removed itself, so the caller checks `fired`.
        struct TimeoutState {
            GMainLoop* loop = nullptr;
            WaitState* wait = nullptr;
            bool fired      = false;
        };

        gboolean quit_loop(gpointer user_data) {
            auto* state = static_cast<TimeoutState*>(user_data);
            {
                // Completion wins if it happened first; the deadline must not
                // relabel a finished injection as a timeout (or the other way
                // round) depending on scheduling.
                std::lock_guard guard(state->wait->mtx);
                if (state->wait->done) {
                    return G_SOURCE_REMOVE;
                }
                state->fired = true;
            }
            g_main_loop_quit(state->loop);
            return G_SOURCE_REMOVE;
        }

        Result from_gerror(GError* error) {
            Result result;
            if (!error) {
                result.status        = Status::failed;
                result.error_message = "unknown frida-core failure";
                return result;
            }
            result.error_domain  = error->domain;
            result.error_code    = error->code;
            result.error_message = error->message ? error->message : "";
            if (error->domain == FRIDA_ERROR && error->code == FRIDA_ERROR_PERMISSION_DENIED) {
                result.status = Status::permission_denied;
            } else if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_PERMISSION_DENIED) {
                result.status = Status::permission_denied;
            } else {
                result.status = Status::failed;
            }
            return result;
        }

    }  // namespace

    void global_init() {
        static std::once_flag once;
        std::call_once(once, []() { frida_init(); });
    }

    Result inject_library(unsigned int pid, const std::string& library, const std::string& entrypoint, const std::string& data, unsigned timeout_ms) {
        global_init();

        // Created on this thread, so its `uninjected` signal is delivered on this
        // thread's default main context, which is what we iterate below.
        FridaInjector* injector = frida_injector_new();
        if (!injector) {
            Result result;
            result.status        = Status::failed;
            result.error_message = "frida_injector_new failed";
            return result;
        }

        WaitState state;
        g_signal_connect(injector, "uninjected", G_CALLBACK(on_uninjected), &state);

        GError* error = nullptr;
        guint id      = frida_injector_inject_library_file_sync(
            injector, pid, library.c_str(), entrypoint.c_str(), data.c_str(), nullptr, &error
        );
        if (error) {
            auto result = from_gerror(error);
            g_error_free(error);
            g_signal_handlers_disconnect_by_func(injector, (gpointer)on_uninjected, &state);
            return result;
        }

        bool completed = false;
        {
            std::lock_guard guard(state.mtx);
            state.wanted = id;
            for (auto seen : state.received) {
                if (seen == id) {
                    state.done = true;
                    break;
                }
            }
            completed = state.done;
        }

        TimeoutState timeout_state { nullptr, &state, false };
        if (!completed) {
            GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
            {
                std::lock_guard guard(state.mtx);
                state.loop = loop;
            }
            timeout_state.loop = loop;
            guint timer        = g_timeout_add(timeout_ms, quit_loop, &timeout_state);
            g_main_loop_run(loop);
            if (!timeout_state.fired) {
                g_source_remove(timer);
            }
            {
                std::lock_guard guard(state.mtx);
                state.loop = nullptr;
                completed  = state.done;
            }
            g_main_loop_unref(loop);
        }
        // A late completion (after the deadline) is not a success: the entry point
        // ran, but the caller's bounded contract was not met.
        const bool timed_out = !completed || timeout_state.fired;

        g_signal_handlers_disconnect_by_func(injector, (gpointer)on_uninjected, &state);
        // A successful injection can release the helper: the target keeps the
        // resident image, frida only drops its bookkeeping. The failure path
        // deliberately skips this: tearing down a helper whose injection was
        // refused crashed inside frida's dbus teardown.
        frida_injector_close_sync(injector, nullptr, nullptr);
        g_object_unref(injector);

        Result result;
        result.id = id;
        if (timed_out) {
            result.status        = Status::timed_out;
            result.error_message = "injection " + std::to_string(id) + " did not finish within " + std::to_string(timeout_ms) + "ms";
            return result;
        }
        result.status = Status::ok;
        return result;
    }

}  // namespace luadebug::injector
