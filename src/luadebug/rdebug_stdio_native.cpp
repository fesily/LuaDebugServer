// Native half of the stdio capture: the `print` / `io.write` patches and the
// sink that receives what they capture.
//
// Nothing here is process-global except the sink table, which is keyed by VM
// binding: a captured call is attributed to the VM the closure was installed
// on, never to "the last VM that printed".

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>

#include "rdebug_redirect.h"

namespace luadebug::stdio {

    namespace {
        // Fixed, lock-free table: output capture is installed when a session
        // starts and read from the target's own `print`, so it must not allocate,
        // take a lock, or be destroyed at exit (a static with a destructor can be
        // torn down while the target still runs user code).
        constexpr int kMaxSinks = 8;
        struct SinkSlot {
            std::atomic<vm::Vm*> vm { nullptr };
            std::atomic<output_sink*> sink { nullptr };
        };
        SinkSlot g_sinks[kMaxSinks];

        output_sink* sink_for(vm::Vm* vm) {
            if (!vm) {
                return nullptr;
            }
            for (const SinkSlot& slot : g_sinks) {
                if (slot.vm.load(std::memory_order_acquire) == vm) {
                    return slot.sink.load(std::memory_order_acquire);
                }
            }
            return nullptr;
        }

        // Pushes the version's `io.output()` file object.
        int push_io_output(vm::State* hL) {
            if (hL->vm()->profile() == vm::Profile::lua51 || hL->vm()->profile() == vm::Profile::luajit) {
                // 5.1 keeps `_IO_output` in the running function's environment.
                return hL->rawgeti(vm::idx::environ, 2);
            }
            return hL->getfield(vm::idx::registry, "_IO_output");
        }

        int call_original(vm::State* hL) {
            hL->pushvalue(vm::State::upvalue(1));
            hL->insert(1);
            hL->call(hL->gettop() - 1, vm::kMultiRet);
            return hL->gettop();
        }

        int redirect_print(vm::State* hL) {
            if (output_sink* sink = sink_for(hL->vm())) {
                if (sink->output(hL, "print", 1)) {
                    return 0;
                }
            }
            return call_original(hL);
        }

        int redirect_f_write(vm::State* hL) {
            push_io_output(hL);
            const bool is_output = hL->type(-1) == vm::type::userdata && hL->rawequal(-1, 1);
            hL->pop(1);
            if (is_output) {
                if (output_sink* sink = sink_for(hL->vm())) {
                    if (sink->output(hL, "iowrite", 2)) {
                        hL->settop(1);
                        return 1;
                    }
                }
            }
            return call_original(hL);
        }

        int redirect_io_write(vm::State* hL) {
            if (output_sink* sink = sink_for(hL->vm())) {
                if (sink->output(hL, "iowrite", 1)) {
                    push_io_output(hL);
                    return 1;
                }
            }
            return call_original(hL);
        }

        // Raw entry points: Lua calls these with the target's own state, which
        // the registry maps back to the bound VM. A closure installed on a VM the
        // bridge does not know is a bug, and is treated as "no redirect".
        int print_entry(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            return L ? redirect_print(L) : 0;
        }
        int file_write_entry(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            return L ? redirect_f_write(L) : 0;
        }
        int io_write_entry(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            return L ? redirect_io_write(L) : 0;
        }

        // Swaps `f` in (enable) or restores the original (disable). The original
        // is kept as the closure's first upvalue.
        bool openhook(vm::State* hL, bool enable, vm::CFunc f) {
            if (enable) {
                hL->pushcclosure(f, 1);
                return true;
            }
            if (hL->tocfunction(-1) == f) {
                if (hL->getupvalue(-1, 1)) {
                    hL->remove(-2);
                    return true;
                }
            }
            hL->pop(1);
            return false;
        }

    }  // namespace

    void set_output_sink(vm::Vm* vm, output_sink* sink) {
        if (!vm) {
            return;
        }
        for (SinkSlot& slot : g_sinks) {
            if (slot.vm.load(std::memory_order_acquire) == vm) {
                slot.sink.store(sink, std::memory_order_release);
                if (!sink) {
                    slot.vm.store(nullptr, std::memory_order_release);
                }
                return;
            }
        }
        if (!sink) {
            return;
        }
        for (SinkSlot& slot : g_sinks) {
            vm::Vm* expected = nullptr;
            if (slot.vm.compare_exchange_strong(expected, vm, std::memory_order_acq_rel)) {
                slot.sink.store(sink, std::memory_order_release);
                return;
            }
        }
    }

    output_sink* output_sink_of(vm::Vm* vm) {
        return sink_for(vm);
    }

    void open_print(vm::State* hL, bool enable) {
        if (!hL) {
            return;
        }
        hL->getglobal("print");
        if (openhook(hL, enable, print_entry)) {
            hL->setglobal("print");
        }
    }

    void open_iowrite(vm::State* hL, bool enable) {
        if (!hL) {
            return;
        }
        if (push_io_output(hL) == vm::type::userdata) {
            if (hL->getmetatable(-1)) {
                if (hL->vm()->api().lua_version_num() >= 504) {
                    // 5.4 moved the file methods into a `__index` table.
                    hL->pushstring("__index");
                    hL->rawget(-2);
                    if (hL->type(-1) == vm::type::table) {
                        hL->remove(-2);
                        hL->pushstring("write");
                        hL->pushvalue(-1);
                        hL->rawget(-3);
                        if (openhook(hL, enable, file_write_entry)) {
                            hL->rawset(-3);
                        }
                        hL->pop(1);
                    } else {
                        hL->pop(1);
                    }
                } else {
                    hL->pushstring("write");
                    hL->pushvalue(-1);
                    hL->rawget(-3);
                    if (openhook(hL, enable, file_write_entry)) {
                        hL->rawset(-3);
                    }
                    hL->pop(1);
                }
            }
        }
        hL->pop(1);
        if (hL->getglobal("io") == vm::type::table) {
            hL->pushstring("write");
            hL->pushvalue(-1);
            hL->rawget(-3);
            if (openhook(hL, enable, io_write_entry)) {
                hL->rawset(-3);
            }
        }
        hL->pop(1);
    }

}  // namespace luadebug::stdio
