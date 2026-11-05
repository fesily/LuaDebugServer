// Target-side hook machinery, with no dependency on the debugger VM.
//
// Extracted from the old `rdebug_hookmgr.cpp`; every event now goes to a
// hook::Sink instead of a Lua callback.

#include "hook/hook_core.h"

#include <boost/unordered/unordered_flat_map.hpp>

#include <utility>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>

#include "rdebug_eventfree.h"
#include "thunk/thunk.h"
#include "vm/vm.h"

namespace luadebug::hook {

    namespace {

        struct timer {
            std::chrono::steady_clock::time_point last;
            bool update(int ms) {
                auto now = std::chrono::steady_clock::now();
                if (last.time_since_epoch().count() == 0) {
                    last = now;
                    return true;
                }
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < ms) {
                    return false;
                }
                last = now;
                return true;
            }
        };

        class bpmap {
        public:
            enum class status {
                None,
                Break,
                Ignore,
            };

            void set(void* proto, status status) noexcept {
                switch (status) {
                case status::None:
                    m_flatmap.erase(proto);
                    break;
                case status::Break:
                    m_flatmap.insert_or_assign(proto, true);
                    break;
                case status::Ignore:
                    m_flatmap.insert_or_assign(proto, false);
                    break;
                default:
                    std::unreachable();
                }
            }

            status get(void* proto) const noexcept {
                auto it = m_flatmap.find(proto);
                if (it != m_flatmap.end()) {
                    return it->second ? status::Break : status::Ignore;
                }
                return status::None;
            }

        private:
            boost::unordered_flat_map<void*, bool> m_flatmap;
        };

        struct THUNK_MGR_T {};
        inline THUNK_MGR_T THUNK_MGR;

    }  // namespace

    struct Core::Impl {
        //
        // break
        //
        bpmap break_proto;
        unsigned break_mask = 0;

        void break_add(vm::State* hL, void* p) {
            break_proto.set(p, bpmap::status::Break);
        }
        void break_del(vm::State* hL, void* p) {
            break_proto.set(p, bpmap::status::Ignore);
        }
        void break_freeobj(void* p) {
            break_proto.set(p, bpmap::status::None);
        }
        // While breakpoints exist the line mask stays on. Arming per prototype is an
        // optimization that depends on seeing a call (or the matching return) for
        // that prototype; a chunk that was already on the stack when the debugger
        // attached (the debuggee's main chunk in launch mode) never produces one, so
        // its breakpoints would silently never fire.
        bool line_always = false;
        void break_open(vm::State* hL, bool enable) {
            line_always = enable;
            if (enable) {
                break_update(hL, hL->callinfo(), vm::event::call);
            } else {
                break_hookmask(hL, 0);
            }
        }
        void break_openline(vm::State* hL) {
            break_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
        }
        void break_closeline(vm::State* hL) {
            if (line_always) {
                break_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
                return;
            }
            break_hookmask(hL, vm::mask::call | vm::mask::ret);
        }
        bool break_new(vm::State* hL, void* p, int event) {
            break_del(hL, p);

            if (!sink_) {
                return false;
            }
            // The consumer registers the prototype through break_add/break_del as a
            // side effect; the decision is read back from the map, exactly like the
            // pre-refactor flow (the callback's return value is not the decision).
            sink_->new_proto(hL, p, event == vm::event::ret);
            return break_proto.get(p) == bpmap::status::Break;
        }
        bool break_has(vm::State* hL, void* p, int event) {
            if (!p) {
                return false;
            }
            auto status = break_proto.get(p);
            if (status == bpmap::status::None) {
                return break_new(hL, p, event);
            }
            return status == bpmap::status::Break;
        }
        void break_update(vm::State* hL, void* ci, int event) {
            if (break_has(hL, hL->ci2proto(ci), event)) {
                break_openline(hL);
            } else {
                break_closeline(hL);
            }
        }
        void break_hook_call(vm::State* hL, const vm::HookInfo& ar) {
            void* ci = nullptr;
            if (!hL->hook_ci(ar, &ci) || !ci) {
                return;
            }
            break_update(hL, ci, ar.event);
        }
        void break_hook_return(vm::State* hL, const vm::HookInfo& ar) {
            // 5.2+ chain their call frames, so the caller is one hop away. 5.1 and
            // LuaJIT have no chain: their frames live in one array and the caller is
            // stack level 1.
            void* ci = nullptr;
            if (hL->hook_ci(ar, &ci) && ci) {
                if (void* previous = hL->ci_previous(ci)) {
                    break_update(hL, previous, ar.event);
                    return;
                }
            }
            vm::FrameInfo outer;
            if (!hL->getstack(1, &outer)) {
                return;
            }
            break_update(hL, hL->debug2ci(&outer), ar.event);
        }
        void break_hookmask(vm::State* hL, unsigned mask) {
            if (break_mask != mask) {
                break_mask = mask;
                updatehookmask(hL);
            }
        }

        //
        // funcbp
        //
        unsigned funcbp_mask = 0;
        void funcbp_hook(vm::State* hL, const vm::HookInfo& ar) {
            sink_->function_break(hL);
        }
        void funcbp_open(vm::State* hL, bool enable) {
            funcbp_hookmask(hL, enable ? vm::mask::call : 0);
        }
        void funcbp_hookmask(vm::State* hL, unsigned mask) {
            if (funcbp_mask != mask) {
                funcbp_mask = mask;
                updatehookmask(hL);
            }
        }

        //
        // step
        //
        vm::State* stepL       = 0;
        int step_current_level = 0;
        int step_target_level  = 0;
        unsigned step_mask     = 0;

        void step_in(vm::State* hL) {
            step_current_level = 0;
            step_target_level  = 0;
            stepL              = 0;
            // 5.4+ deliver a return hook for every call the line mask is waiting on,
            // so entering a function can be detected without line events.
            if (hL->vm()->api().lua_version_num() >= 504) {
                step_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
            } else {
                step_hookmask(hL, vm::mask::line);
            }
        }
        void step_out(vm::State* hL) {
            step_current_level = hL->stacklevel();
            step_target_level  = step_current_level - 1;
            stepL              = hL;
            step_hookmask(hL, vm::mask::call | vm::mask::ret);
        }
        void step_over(vm::State* hL) {
            step_current_level = hL->stacklevel();
            step_target_level  = step_current_level;
            stepL              = hL;
            step_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
        }
        void step_cancel(vm::State* hL) {
            step_current_level = 0;
            step_target_level  = 0;
            stepL              = 0;
            step_hookmask(hL, 0);
        }
        void step_hook_call(vm::State* hL, const vm::HookInfo& ar) {
            // LuaJIT enters the hook for a C function call but never for its return,
            // so C calls must not move the step level.
            if (hL->vm()->profile() == vm::Profile::luajit && !hL->is_lua_func(ar)) {
                return;
            }
            step_current_level++;
            if (step_current_level > step_target_level) {
                step_hookmask(hL, vm::mask::call | vm::mask::ret);
            } else {
                step_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
            }
        }
        void step_hook_return(vm::State* hL, const vm::HookInfo& ar) {
            step_current_level = hL->stacklevel() - 1;
            if (step_current_level > step_target_level) {
                step_hookmask(hL, vm::mask::call | vm::mask::ret);
            } else {
                step_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
            }
        }
        void step_hook_line(vm::State* hL, const vm::HookInfo& ar) {
            step_current_level = hL->stacklevel();
            if (step_current_level > step_target_level) {
                step_hookmask(hL, vm::mask::call | vm::mask::ret);
            } else {
                step_hookmask(hL, vm::mask::call | vm::mask::ret | vm::mask::line);
            }
        }
        void step_hookmask(vm::State* hL, unsigned mask) {
            if (step_mask != mask) {
                step_mask = mask;
                updatehookmask(hL);
            }
        }

        //
        // instbreak (instruction breakpoints via count hook)
        //
        unsigned instbreak_mask = 0;

        void instbreak_open(vm::State* hL, bool enable) {
            instbreak_mask = enable ? vm::mask::count : 0;
            updatehookmask(hL);
        }

        //
        // exception
        //
        unsigned exception_mask = 0;
        void exception_hookmask(vm::State* hL, unsigned mask) {
            if (exception_mask != mask) {
                exception_mask = mask;
                updatehookmask(hL);
            }
        }
        void exception_open(vm::State* hL, bool enable) {
            exception_hookmask(hL, enable ? vm::mask::exception : 0);
        }
        void exception_hook(vm::State* hL, const vm::HookInfo& ar) {
            if (!sink_) {
                return;
            }
            // bee's exception hook passes the error object's absolute stack index in
            // the record; the object itself is the top value, so it is reachable as
            // the usual relative index -1 (no top juggling needed).
            sink_->exception(hL, hL->vm()->api().exception_code(hL->vm()->raw(), hL->raw(), ar.record));
        }

        //
        // thread
        //
        unsigned thread_mask = 0;
        boost::unordered_flat_map<vm::State*, vm::State*> coroutine_tree;
        void thread_hookmask(vm::State* hL, unsigned mask) {
            if (thread_mask != mask) {
                thread_mask = mask;
                updatehookmask(hL);
            }
        }
        void thread_open(vm::State* hL, bool enable) {
            thread_hookmask(hL, enable ? vm::mask::thread : 0);
        }
        void thread_hook(vm::State* co, const vm::HookInfo& ar) {
            vm::State* from = static_cast<vm::State*>(co->touserdata(-1));
            if (from) {
                int type = ar.currentline;
                if (type == 0) {
                    coroutine_tree.insert_or_assign(co, from);
                } else if (type == 1) {
                    coroutine_tree.erase(from);
                }
            }
            updatehookmask(co);
        }
        vm::State* coroutine_from(vm::State* co) {
            auto it = coroutine_tree.find(co);
            if (it == coroutine_tree.end()) {
                return nullptr;
            }
            return it->second;
        }

        //
        // common
        //
        std::unique_ptr<thunk> sc_full_hook;
        std::unique_ptr<thunk> sc_idle_hook;
        void* eventfree = nullptr;

        luadebug::hook::Sink* sink_ = nullptr;

        Impl() = default;

        bool last_hook_call_in_c = false;
        // Cached profile flag: the hook path must not touch a registry per event.
        bool is_luajit = false;
        void full_hook(vm::State* hL, const vm::HookInfo& ar) {
            switch (ar.event) {
            case vm::event::line:
                if (is_luajit && last_hook_call_in_c) {
                    thread_mask &= (~vm::mask::thread);
                    updatehookmask(hL);
                    last_hook_call_in_c = false;
                    if (break_mask)
                        break_hook_call(hL, ar);
                }
                if (is_luajit) {
                    if (stepL == hL) {
                        if (step_mask & vm::mask::ret) {
                            step_hook_line(hL, ar);
                        }
                    }
                    if (!((step_mask & vm::mask::line) && (!stepL || stepL == hL)) && !(break_mask & vm::mask::line)) {
                        return;
                    }
                }
                break;
            case vm::event::call:
            case vm::event::tail_call:
                if (funcbp_mask) {
                    funcbp_hook(hL, ar);
                }
                if (break_mask & vm::mask::call) {
                    break_hook_call(hL, ar);
                }
                if (stepL == hL) {
                    if (step_mask & vm::mask::call) {
                        step_hook_call(hL, ar);
                    }
                }
                if (is_luajit) {
                    // LuaJIT reports C calls as a call event with no matching return,
                    // so a line event is armed to close them.
                    last_hook_call_in_c = !hL->is_lua_func(ar);
                    if (last_hook_call_in_c) {
                        thread_mask |= vm::mask::line;
                        updatehookmask(hL);
                        if (update_mask) {
                            update_hook(hL);
                        }
                    }
                }
                return;
            case vm::event::ret:
                if (update_mask) {
                    update_hook(hL);
                }
                if (break_mask & vm::mask::ret) {
                    break_hook_return(hL, ar);
                }
                if (stepL == hL) {
                    if (step_mask & vm::mask::ret) {
                        step_hook_return(hL, ar);
                    }
                } else if (hL->vm()->api().lua_version_num() >= 504 && (step_mask & vm::mask::line)) {
                    // step in: the return event of the enclosing call is the line
                    // event 5.4+ reports after the callee finished.
                    break;
                }
                return;
            case vm::event::count:
                if (instbreak_mask && sink_) {
                    void* ci = nullptr;
                    if (hL->hook_ci(ar, &ci) && ci) {
                        if (void* proto = hL->ci2proto(ci)) {
                            sink_->instruction_break(hL, proto);
                        }
                    }
                }
                update_hook(hL);
                return;
            case vm::event::exception:
                exception_hook(hL, ar);
                return;
            case vm::event::thread:
                thread_hook(hL, ar);
                return;
            default:
                return;
            }
            if (!sink_) {
                return;
            }
            const bool stepping = (step_mask & vm::mask::line) && (!stepL || stepL == hL);
            void* proto         = nullptr;
            void* ci            = nullptr;
            if (hL->hook_ci(ar, &ci) && ci) {
                proto = hL->ci2proto(ci);
            }
            sink_->line_stop(hL, stepping, ar.currentline, proto);
        }

        void idle_hook(vm::State* hL, const vm::HookInfo& ar) {
            switch (ar.event) {
            case vm::event::ret:
            case vm::event::count:
                update_hook(hL);
                return;
            case vm::event::exception:
                exception_hook(hL, ar);
                return;
            case vm::event::thread:
                thread_hook(hL, ar);
                return;
            default:
                return;
            }
        }

        void sethook(vm::State* hL, vm::HookFn func, unsigned mask, int count) {
            hL->sethook(func, mask, count);
            // LuaJIT keeps hook state in the global state, so one call covers every
            // thread; the 5.x family stores it per thread and needs both.
            if (!is_luajit) {
                vm::State* mainL = hL->mainthread();
                if (mainL && mainL != hL) {
                    mainL->sethook(func, mask, count);
                }
            }
        }

        void updatehookmask(vm::State* hL) {
            int mask = break_mask | funcbp_mask;
            if (!stepL || stepL == hL) {
                mask |= step_mask;
            }
            int count = 0;
            if (instbreak_mask) {
                mask |= vm::mask::count;
                count = 1;
            }
            if (mask) {
                sethook(hL, reinterpret_cast<vm::HookFn>(sc_full_hook->data), mask | exception_mask | thread_mask, count);
            } else if (update_mask) {
                sethook(hL, reinterpret_cast<vm::HookFn>(sc_idle_hook->data), update_mask | exception_mask | thread_mask, 0xfffff);
            } else if (exception_mask | thread_mask) {
                sethook(hL, reinterpret_cast<vm::HookFn>(sc_idle_hook->data), exception_mask | thread_mask, 0);
            } else {
                sethook(hL, nullptr, 0, 0);
            }
        }

        unsigned update_mask = 0;
        timer update_timer;
        void update_open(vm::State* hL, bool enable) {
            update_mask = enable ? vm::mask::ret : 0;
            updatehookmask(hL);
        }
        void update_hook(vm::State* hL) {
            if (!update_timer.update(200)) {
                return;
            }
            sink_->update(hL);
        }

        vm::State* hL = 0;
        void init(vm::State* target) {
            this->hL  = target;
            is_luajit = target->vm()->profile() == vm::Profile::luajit;
            // Without a hook thunk the pointer handed to `lua_sethook` *is* the hook
            // function, so it has to be the `(lua_State*, ar)` entry that looks the
            // manager up in the registry. The thunk variants bake `this` in and use
            // the `(Impl*, lua_State*, ar)` signature instead; passing one for the
            // other called the manager pointer as a hook.
#if defined(LUADEBUG_DISABLE_THUNK)
            thunk_set(target, &THUNK_MGR, (intptr_t)this);
            nothunk_slot() = this;
            sc_full_hook.reset(thunk_create_hook(
                reinterpret_cast<intptr_t>(this),
                reinterpret_cast<intptr_t>(&full_hook_entry_nothunk)
            ));
            sc_idle_hook.reset(thunk_create_hook(
                reinterpret_cast<intptr_t>(this),
                reinterpret_cast<intptr_t>(&idle_hook_entry_nothunk)
            ));
#else
            sc_full_hook.reset(thunk_create_hook(
                reinterpret_cast<intptr_t>(this),
                reinterpret_cast<intptr_t>(&full_hook_entry)
            ));
            sc_idle_hook.reset(thunk_create_hook(
                reinterpret_cast<intptr_t>(this),
                reinterpret_cast<intptr_t>(&idle_hook_entry)
            ));
#endif
            eventfree = luadebug::eventfree::create(hL, freeobj_callback, this);
        }
        ~Impl() {
            if (!hL) {
                return;
            }
            luadebug::eventfree::destroy(hL, eventfree);
            hL->sethook(nullptr, 0, 0);
#if defined(LUADEBUG_DISABLE_THUNK)
            thunk_set(hL, &THUNK_MGR, intptr_t(nullptr));
            if (nothunk_slot() == this) {
                nothunk_slot() = nullptr;
            }
#endif
            if (hL->vm()->api().has_exception_hook()) {
                exception_open(hL, 0);
            }
            hL = 0;
        }
#if defined(LUADEBUG_DISABLE_THUNK)
        static Impl*& nothunk_slot() {
            static Impl* mgr = nullptr;
            return mgr;
        }
        static Impl* nothunk_manager(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            Impl* mgr    = L ? reinterpret_cast<Impl*>(thunk_get(L, &THUNK_MGR)) : nullptr;
            return mgr ? mgr : nothunk_slot();
        }
#endif
        static void freeobj_callback(void* mgr, void* ptr) {
            static_cast<Impl*>(mgr)->break_freeobj(ptr);
        }

        // Hook entry: the trampoline bakes in `this`, so the hot path never looks a
        // core up. A coroutine's state is adopted through its own VM, which we
        // already know.
        void dispatch(lua_State* raw, const void* ar, bool idle) {
            if (!hL) {
                return;
            }
            vm::State* L = (raw == hL->raw()) ? hL : hL->vm()->state(raw);
            if (!L) {
                return;
            }
            vm::HookInfo info;
            hL->vm()->api().hook_info(hL->vm()->raw(), ar, &info);
            if (idle) {
                idle_hook(L, info);
            } else {
                full_hook(L, info);
            }
        }

        static void full_hook_entry(Impl* mgr, lua_State* raw, const void* ar) {
            mgr->dispatch(raw, ar, false);
        }
        static void idle_hook_entry(Impl* mgr, lua_State* raw, const void* ar) {
            mgr->dispatch(raw, ar, true);
        }
#if defined(LUADEBUG_DISABLE_THUNK)
        // Without a code thunk the hook entry cannot bake the manager in, and the
        // registry lookup below only resolves states the binding already knows: a
        // coroutine inherits `hook` when it is created, so its first events arrive
        // before anything registered that state, and they would be dropped. One
        // bridge hosts one session, so the manager is reachable per library.
        static int full_hook_entry_nothunk(lua_State* raw, const void* ar) {
            if (Impl* mgr = nothunk_manager(raw)) {
                mgr->dispatch(raw, ar, false);
            }
            return 0;
        }
        static int idle_hook_entry_nothunk(lua_State* raw, const void* ar) {
            if (Impl* mgr = nothunk_manager(raw)) {
                mgr->dispatch(raw, ar, true);
            }
            return 0;
        }
#endif
    };

    Core::Core(vm::State* target, Sink* sink)
        : impl_(new Impl()) {
        impl_->sink_ = sink;
        if (target) {
            impl_->init(target);
        }
    }

    Core::~Core() = default;

    bool Core::valid() const {
        return impl_ && impl_->hL != nullptr;
    }

    vm::State* Core::target() const {
        return impl_ ? impl_->hL : nullptr;
    }

    // The Impl keeps the target's own Proto type; the public API stays opaque.
    void Core::break_add(vm::State* L, void* proto) {
        impl_->break_add(L, proto);
    }
    void Core::break_del(vm::State* L, void* proto) {
        impl_->break_del(L, proto);
    }
    void Core::break_freeobj(void* proto) {
        impl_->break_freeobj(proto);
    }
    void Core::break_open(vm::State* L, bool enable) {
        impl_->break_open(L, enable);
    }
    void Core::break_closeline(vm::State* L) {
        impl_->break_closeline(L);
    }
    void Core::funcbp_open(vm::State* L, bool enable) {
        impl_->funcbp_open(L, enable);
    }
    void Core::step_in(vm::State* L) {
        impl_->step_in(L);
    }
    void Core::step_over(vm::State* L) {
        impl_->step_over(L);
    }
    void Core::step_out(vm::State* L) {
        impl_->step_out(L);
    }
    void Core::step_cancel(vm::State* L) {
        impl_->step_cancel(L);
    }

    void Core::step(StepMode mode) {
        if (!impl_ || !impl_->hL) {
            return;
        }
        switch (mode) {
        case StepMode::over:
            impl_->step_over(impl_->hL);
            break;
        case StepMode::in:
            impl_->step_in(impl_->hL);
            break;
        case StepMode::out:
            impl_->step_out(impl_->hL);
            break;
        case StepMode::none:
            impl_->step_cancel(impl_->hL);
            break;
        }
    }
    void Core::instbreak_open(vm::State* L, bool enable) {
        impl_->instbreak_open(L, enable);
    }
    void Core::update_open(vm::State* L, bool enable) {
        impl_->update_open(L, enable);
    }
    void Core::exception_open(vm::State* L, bool enable) {
        // Profiles without exception hooks simply keep the mask at zero; the
        // session's capability matrix already reports that.
        if (L && L->vm()->api().has_exception_hook()) {
            impl_->exception_open(L, enable);
        }
    }
    void Core::thread_open(vm::State* L, bool enable) {
        if (L && L->vm()->api().has_thread_hook()) {
            impl_->thread_open(L, enable);
        }
    }
    vm::State* Core::coroutine_from(vm::State* co) {
        if (!co || !co->vm()->api().has_thread_hook()) {
            return nullptr;
        }
        return impl_->coroutine_from(co);
    }
    void Core::updatehookmask(vm::State* hL) {
        impl_->updatehookmask(hL);
    }

}  // namespace luadebug::hook
