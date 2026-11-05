#include "target/protected.h"

#include <string>

namespace luadebug::target {

    namespace {

        struct Callback {
            protected_fn fn = nullptr;
            void* user      = nullptr;
        };

        // Errors reach Lua as a string, never as a C++ exception.
        int trampoline_body(vm::State* L) {
            auto* callback = static_cast<Callback*>(L->touserdata(vm::State::upvalue(1)));
            if (!callback || !callback->fn) {
                L->pushstring("debugger: empty protected call");
                return L->error();
            }
            try {
                return callback->fn(L, callback->user);
            } catch (const std::exception& e) {
                L->pushstring(e.what());
                return L->error();
            } catch (...) {
                L->pushstring("debugger: unrecoverable C++ exception");
                return L->error();
            }
        }

        // Raw entry: the target calls this with its own state, which the registry
        // maps back to the bound VM.
        int trampoline(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            return trampoline_body(L);
        }

        std::string take_error(vm::State* L) {
            std::string message;
            if (const char* text = L->tostring(-1)) {
                message = text;
            } else {
                message = "unknown lua error";
            }
            L->pop(1);
            return message;
        }

    }  // namespace

    StackGuard::StackGuard(vm::State* L) noexcept
        : L_(L)
        , top_(L ? L->gettop() : 0) {
    }

    StackGuard::~StackGuard() {
        if (L_) {
            L_->settop(top_);
        }
    }

    CallResult pcall(vm::State* L, protected_fn fn, void* user, int nargs, int nresults) {
        CallResult result;
        if (!L) {
            result.status  = CallStatus::error;
            result.message = "debugger: no lua state";
            return result;
        }
        // Note: no StackGuard here — on success the results must stay on the
        // stack; only the failure paths restore the incoming top.
        const int top = L->gettop();

        // The callback lives in the closure's upvalue, so the trampoline itself
        // stays a plain C function with no state of its own.
        Callback callback { fn, user };
        L->pushlightuserdata(&callback);
        L->pushcclosure(trampoline, 1);

        // Move the closure below the arguments.
        int base = L->gettop() - nargs - 1;
        L->insert(base + 1);

        int status = L->pcall(nargs, nresults, 0);
        if (status != vm::kOk) {
            result.status   = CallStatus::error;
            result.message  = take_error(L);
            result.nresults = 0;
            L->settop(top - nargs);
            return result;
        }
        result.status   = CallStatus::ok;
        result.nresults = L->gettop() - (top - nargs);
        return result;
    }

}  // namespace luadebug::target
