// The script-facing and native entry points of the single bridge library.
//
// Two ways in, one binding rule:
//
//   * a Lua script loads `luaopen_luadebug_<profile>` from this same DSO. The
//     entry captures its own caller address (the target's `package.loadlib`
//     call frame), which identifies the *module* that must be used - not "the
//     first library in the process that exports lua_gettop".
//   * a native host calls `luadebug_bridge_bind` with an explicit descriptor
//     naming module, resolver and state.
//
// Both validate the profile before touching the target stack; a mismatch is an
// error, never a fallback to another layout.

#include <cstdlib>
#include <cstring>
#include <string>

#include "bridge/bridge_abi.h"
#include "bridge/session_boot.h"
#include "common/log.h"
#include "vm/boot.h"
#include "vm/module.h"
#include "vm/vm.h"

#if defined(_WIN32)
#    include <cstdlib>
#endif

namespace luadebug::bridge {

    namespace {

        // Resolver handed to `Vm` for the descriptor path: the host's own.
        struct ResolveContext {
            void* (*resolver)(void* module, const char* name);
            void* module;
        };

        // Per-binding context behind the C ABI ops.
        struct NativeBridge {
            vm::Vm* vm       = nullptr;
            vm::State* state = nullptr;
            std::string last_error;
        };

        // --- Lua module table -------------------------------------------------
        int l_start(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            std::size_t size        = 0;
            const char* config_json = L->tolstring(1, &size);
            BootOptions options;
            if (config_json) {
                options.config_json.assign(config_json, size);
            }
            options.attach_only = L->toboolean(2);
            std::string error;
            const bool started = startup(L, options, error);
            if (!started) {
                luadebug::debugger::log_error("bridge: %s", error.c_str());
            }
            L->pushboolean(started);
            return 1;
        }

        int l_pump(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            const auto wait_ms = static_cast<unsigned>(L->tointeger(1));
            const bool ok      = pump_state(L, wait_ms) || pump(wait_ms);
            L->pushboolean(ok);
            return 1;
        }

        int l_stop(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            shutdown(L);
            return 0;
        }

        int l_running(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                L = nullptr;
            }
            L->pushboolean(L && running(L));
            return 1;
        }

        int l_worker_id(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            L->pushinteger(static_cast<long long>(worker_id(L)));
            return 1;
        }

        int l_profile(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            const char* name = vm::profile_name(L->vm()->profile());
            L->pushstring(name);
            return 1;
        }

        // Exception and thread reporting is native; hosts that still call
        // `debugger.event(...)` keep working with nothing to forward.
        int l_event(lua_State*) {
            return 0;
        }

        int l_setenv(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            const char* name  = L->tostring(1);
            const char* value = L->tostring(2);
            if (!name || !value) {
                return 0;
            }
#if defined(_WIN32)
            std::string joined = std::string(name) + "=" + value;
            _putenv(joined.c_str());
#else
            ::setenv(name, value, 1);
#endif
            return 0;
        }

#if defined(_WIN32)
        int l_a2u(lua_State* raw) {
            vm::State* L = vm::State::find(raw);
            if (!L) {
                return 0;
            }
            std::size_t size = 0;
            const char* text = L->tolstring(1, &size);
            if (!text) {
                return 0;
            }
            const std::string converted = ansi_to_utf8(std::string_view(text, size));
            L->pushlstring(converted.data(), converted.size());
            return 1;
        }
#endif

        // Builds the module table. `requested` is the profile the entry point was
        // called through; the running module must have proved it matches.
        int push_module_table(vm::State* L, const char* requested) {
            L->createtable(0, 10);
            const int table = L->gettop();

            struct Entry {
                const char* name;
                vm::CFunc fn;
            };
            const Entry entries[] = {
                { "start", l_start },
                { "pump", l_pump },
                { "stop", l_stop },
                { "running", l_running },
                { "worker_id", l_worker_id },
                { "profile", l_profile },
                { "event", l_event },
                { "setenv", l_setenv },
#if defined(_WIN32)
                { "a2u", l_a2u },
#endif
            };
            for (const Entry& entry : entries) {
                L->pushcclosure(entry.fn, 0);
                L->setfield(table, entry.name);
            }
            L->pushstring(requested);
            L->setfield(table, "requestedProfile");
            return 1;
        }

        // Resolves module + profile for a Lua entry point and binds it.
        vm::Vm* bind_from_caller(int profile_index, const void* caller, lua_State* raw, std::string& error) {
            const auto profile = static_cast<vm::Profile>(profile_index);
            vm::Vm* bound      = vm::bind_module_at(profile, caller, error);
            if (!bound) {
                return nullptr;
            }
            // Adopt the calling state into the binding (idempotent).
            if (!bound->state(raw)) {
                error = "cannot adopt the calling state";
                return nullptr;
            }
            return bound;
        }

    }  // namespace

    // --- native ABI -----------------------------------------------------
    int n_start(void* ctx, const char* config_json, uint32_t size, uint32_t attach_only) {
        auto* self = static_cast<NativeBridge*>(ctx);
        if (!self || !self->state) {
            return 0;
        }
        BootOptions options;
        if (config_json) {
            options.config_json.assign(config_json, size);
        }
        options.attach_only = attach_only != 0;
        std::string error;
        if (!startup(self->state, options, error)) {
            self->last_error = error;
            luadebug::debugger::log_error("bridge: %s", error.c_str());
            return 0;
        }
        return 1;
    }

    int n_pump(void* ctx, uint32_t wait_ms) {
        auto* self = static_cast<NativeBridge*>(ctx);
        if (!self || !self->state) {
            return 0;
        }
        const bool ok = pump_state(self->state, wait_ms) || pump(wait_ms);
        return ok ? 1 : 0;
    }

    void n_stop(void* ctx) {
        auto* self = static_cast<NativeBridge*>(ctx);
        if (self && self->state) {
            shutdown(self->state);
        }
    }

    int n_running(void* ctx) {
        auto* self = static_cast<NativeBridge*>(ctx);
        return self && self->state && running(self->state) ? 1 : 0;
    }

    uint64_t n_worker_id(void* ctx) {
        auto* self = static_cast<NativeBridge*>(ctx);
        return self && self->state ? static_cast<uint64_t>(worker_id(self->state)) : 0;
    }

    const char* n_last_error(void* ctx) {
        auto* self = static_cast<NativeBridge*>(ctx);
        return self ? self->last_error.c_str() : "";
    }

    void n_release(void* ctx) {
        auto* self = static_cast<NativeBridge*>(ctx);
        if (!self) {
            return;
        }
        if (self->state) {
            shutdown(self->state);
        }
        delete self;
    }

    int32_t n_profile(void* ctx) {
        auto* self = static_cast<NativeBridge*>(ctx);
        return self && self->vm ? static_cast<int32_t>(self->vm->profile()) : -1;
    }

    const LdBridgeOps kNops = {
        sizeof(LdBridgeOps),
        LDB_BRIDGE_ABI_VERSION,
        n_start,
        n_pump,
        n_stop,
        n_running,
        n_worker_id,
        n_release,
        n_last_error,
        n_profile,
    };

    // Translates the C descriptor into the binding layer's spec. The module is
    // optional; the table wins over every other source, so a caller with a
    // complete table needs no handle and no exported target symbol.
    bool spec_from_descriptor(const LdBridgeDescriptor& descriptor, vm::VmSpec& spec, std::string& error) {
        if (!descriptor.state) {
            error = "bridge: descriptor must name a state";
            return false;
        }
        if (descriptor.symbol_count > 0 && !descriptor.symbols) {
            error = "bridge: descriptor declares symbols without a table";
            return false;
        }
        // A table, a resolver and a module are independent: a resolver that
        // carries its own `resolver_user` needs neither of the other two.
        if (!descriptor.module && descriptor.symbol_count == 0 && !descriptor.resolver) {
            error = "bridge: descriptor must name a symbol table, a resolver or a module";
            return false;
        }
        spec.profile = descriptor.profile;
        if (descriptor.module) {
            spec.module.handle = descriptor.module;
            spec.module.path   = "descriptor";
        }
        for (uint32_t i = 0; i < descriptor.symbol_count; ++i) {
            const LdBridgeSymbol& entry = descriptor.symbols[i];
            if (!entry.name || !*entry.name) {
                error = "bridge: symbol table holds an empty name";
                return false;
            }
            if (!entry.address) {
                error = std::string("bridge: symbol `") + entry.name + "` has no address";
                return false;
            }
            for (uint32_t j = i + 1; j < descriptor.symbol_count; ++j) {
                const LdBridgeSymbol& other = descriptor.symbols[j];
                if (other.name && std::strcmp(other.name, entry.name) == 0) {
                    error = std::string("bridge: symbol `") + entry.name + "` is repeated";
                    return false;
                }
            }
        }
        spec.symbols       = reinterpret_cast<const vm::SymbolDef*>(descriptor.symbols);
        spec.symbol_count  = descriptor.symbol_count;
        spec.resolver      = descriptor.resolver;
        spec.resolver_user = descriptor.resolver_user;
        return true;
    }

}  // namespace luadebug::bridge

namespace {

    // Shared by `luadebug_bridge_bind` and `luadebug_bridge_open`: the descriptor
    // rules and the error channel must be identical, so the two entry points
    // differ only in whether the module table is pushed.
    bool bind_descriptor(const LdBridgeDescriptor* descriptor, luadebug::vm::Vm*& bound, luadebug::vm::State*& state, std::string& error) {
        if (!descriptor || descriptor->size < sizeof(LdBridgeDescriptor)) {
            error = "bridge: incomplete descriptor";
            return false;
        }
        if (descriptor->abi_version != LDB_BRIDGE_ABI_VERSION) {
            error = "bridge: descriptor ABI version mismatch (expected " + std::to_string(LDB_BRIDGE_ABI_VERSION) + ")";
            return false;
        }
        luadebug::vm::VmSpec spec;
        if (!luadebug::bridge::spec_from_descriptor(*descriptor, spec, error)) {
            return false;
        }
        auto* raw_state = static_cast<lua_State*>(descriptor->state);
        bound           = luadebug::vm::bind_spec(spec, raw_state, error);
        if (!bound) {
            return false;
        }
        state = bound->state(raw_state);
        if (!state) {
            error = "bridge: cannot adopt the target state";
            return false;
        }
        return true;
    }

}  // namespace

extern "C" LDB_BRIDGE_EXPORT const LdBridgeOps*
luadebug_bridge_bind(const LdBridgeDescriptor* descriptor, void** ctx_out, const char** error) {
    static std::string static_error;
    auto fail = [error](const std::string& why) -> const LdBridgeOps* {
        static_error = why;
        if (error) {
            *error = static_error.c_str();
        }
        return nullptr;
    };
    if (error) {
        *error = nullptr;
    }
    luadebug::vm::Vm* bound    = nullptr;
    luadebug::vm::State* state = nullptr;
    std::string why;
    if (!bind_descriptor(descriptor, bound, state, why)) {
        return fail(why);
    }
    auto* ctx  = new luadebug::bridge::NativeBridge();
    ctx->vm    = bound;
    ctx->state = state;
    if (ctx_out) {
        *ctx_out = ctx;
    }
    return &luadebug::bridge::kNops;
}

extern "C" LDB_BRIDGE_EXPORT int luadebug_bridge_open(const LdBridgeDescriptor* descriptor, const char** error) {
    static std::string static_error;
    auto fail = [error](const std::string& why) {
        static_error = why;
        if (error) {
            *error = static_error.c_str();
        }
        return 0;
    };
    if (error) {
        *error = nullptr;
    }
    luadebug::vm::Vm* bound    = nullptr;
    luadebug::vm::State* state = nullptr;
    std::string why;
    if (!bind_descriptor(descriptor, bound, state, why)) {
        return fail(why);
    }
    // The module table is built on the target stack exactly as the script entry
    // does; a failure here leaves nothing on the stack.
    const int top    = state->gettop();
    const int pushed = luadebug::bridge::push_module_table(
        state, luadebug::vm::profile_symbol(bound->profile())
    );
    if (pushed != 1 || state->gettop() != top + 1) {
        state->settop(top);
        return fail("bridge: cannot build the module table");
    }
    return 1;
}

extern "C" LDB_BRIDGE_EXPORT const char* luadebug_bridge_profile(void) {
    // The DSO serves every profile; the interesting one is reported by the Lua
    // module table and by the bound VM.
    return "multi";
}

// --- Lua entry points -------------------------------------------------------
//
// One per profile, in the same DSO. The caller address is captured *here*, in
// the entry function itself: a helper would report the bridge's own frame.

#define LDB_LUA_ENTRY(profile_name_literal, profile_index)                                         \
    extern "C" __attribute__((visibility("default"))) int luaopen_luadebug_##profile_name_literal( \
        lua_State* raw                                                                             \
    ) {                                                                                            \
        const void* caller = __builtin_return_address(0);                                          \
        std::string error;                                                                         \
        luadebug::vm::Vm* bound =                                                                  \
            luadebug::bridge::bind_from_caller(profile_index, caller, raw, error);                 \
        if (!bound) {                                                                              \
            luadebug::debugger::log_error("bridge: %s", error.c_str());                            \
            return 0;                                                                              \
        }                                                                                          \
        luadebug::vm::State* L = bound->state(raw);                                                \
        if (!L) {                                                                                  \
            return 0;                                                                              \
        }                                                                                          \
        return luadebug::bridge::push_module_table(L, #profile_name_literal);                      \
    }

LDB_LUA_ENTRY(lua51, 0)
LDB_LUA_ENTRY(lua52, 1)
LDB_LUA_ENTRY(lua53, 2)
LDB_LUA_ENTRY(lua54, 3)
LDB_LUA_ENTRY(lua55, 4)
LDB_LUA_ENTRY(luajit, 5)

#undef LDB_LUA_ENTRY
