#include "vm/vm.h"

#include <algorithm>
#include <cstring>

#include "vm/boot.h"

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif
#include "common/vm_version.h"
#include "vm/mapping.h"
#include "vm/module.h"
#include "vm/raw_table.h"

// Backend entry points: one implementation per ABI/layout profile, all inside
// this single bridge library. Each backend is compiled against its own Lua
// headers, so no Lua header or per-version type reaches the shared core.
namespace luadebug::vm {

    const char* profile_name(Profile profile) {
        switch (profile) {
        case Profile::lua51:
            return "lua51";
        case Profile::lua52:
            return "lua52";
        case Profile::lua53:
            return "lua53";
        case Profile::lua54:
            return "lua54";
        case Profile::lua55:
            return "lua55";
        case Profile::luajit:
            return "luajit";
        }
        return "unknown";
    }

    const char* profile_symbol(Profile profile) {
        // Same spelling as the profile: the bootstrap entry is
        // `luaopen_luadebug_<profile>`.
        return profile_name(profile);
    }

    bool profile_parse(const char* text, Profile& out) {
        if (!text) {
            return false;
        }
        for (int i = 0; i < kProfileCount; ++i) {
            const Profile candidate = static_cast<Profile>(i);
            if (std::strcmp(text, profile_name(candidate)) == 0) {
                out = candidate;
                return true;
            }
        }
        // `lua-latest` is the bundled interpreter whose sources are identical to
        // 5.5; it shares the 5.5 profile rather than inventing a second ABI.
        if (std::strcmp(text, "lua-latest") == 0) {
            out = Profile::lua55;
            return true;
        }
        return false;
    }

    namespace {
        // Written during static initialization (single-threaded), read afterwards.
        const Api* g_backends[kProfileCount] = {};
    }  // namespace

    void register_backend(Profile profile, const Api* api) {
        const int index = static_cast<int>(profile);
        if (index >= 0 && index < kProfileCount) {
            g_backends[index] = api;
        }
    }

    const Api* backend_for(Profile profile) {
        const int index = static_cast<int>(profile);
        if (index < 0 || index >= kProfileCount) {
            return nullptr;
        }
        return g_backends[index];
    }

    namespace {

        // State registry: raw pointer -> State. A state belongs to exactly one
        // module, so a single lookup table is enough for hook trampolines and
        // for callbacks that only receive a raw state.
        struct Registry {
            std::mutex mutex;
            std::unordered_map<lua_State*, State*> states;
        };

        // Deliberately leaked: states and bindings are torn down during static
        // destruction, in an order the language does not define. The registry
        // must outlive every binding, so it is never destroyed.
        Registry& registry() {
            static Registry* instance = new Registry();
            return *instance;
        }

        void* raw_resolver(void* module, const char* name) {
            if (!module || !name) {
                return nullptr;
            }
#if defined(_WIN32)
            return reinterpret_cast<void*>(
                ::GetProcAddress(static_cast<HMODULE>(module), name)
            );
#else
            return ::dlsym(module, name);
#endif
        }

        // Where each symbol may come from, in priority order: the caller's table
        // first (it is the only source that works for an image exporting
        // nothing), then its resolver, then the module's own exports.
        struct Sources {
            const SymbolDef* symbols = nullptr;
            std::size_t count        = 0;
            const Module* module     = nullptr;
            Resolver resolver        = nullptr;
            void* resolver_user      = nullptr;

            bool usable() const {
                return count > 0 || resolver != nullptr || (module && module->valid());
            }

            void* find(std::string_view name) const {
                const std::string text(name);
                for (std::size_t i = 0; i < count; ++i) {
                    if (symbols[i].name && text == symbols[i].name) {
                        return symbols[i].address;
                    }
                }
                if (resolver) {
                    if (void* hit = resolver(resolver_user, text.c_str())) {
                        return hit;
                    }
                }
                if (module && module->valid()) {
                    return module_symbol(*module, text.c_str());
                }
                return nullptr;
            }

            bool present(std::string_view name) const {
                return find(name) != nullptr;
            }

            // True once the module's own export/symbol list answered at least one
            // lookup. A `dlopen` handle to a stripped or hidden-visibility image is
            // valid while its symbol list is empty, and an empty list says nothing
            // about the version: only an image that answers can make an *absence*
            // informative.
            bool image_answered() const {
                for (const char* name : kSampleNames) {
                    if (module && module->valid() && module_symbol(*module, name)) {
                        return true;
                    }
                }
                return false;
            }

            static constexpr const char* kSampleNames[] = {
                "lua_gettop",
                "lua_settop",
                "lua_type",
                "lua_ident",
                "lua_pushvalue",
            };
        };

        Raw resolve_raw(const Sources& sources) {
            Raw raw {};
#define LDB_RESOLVE(field, symbol) \
    raw.field = reinterpret_cast<decltype(raw.field)>(sources.find(#symbol));
            LDB_RAW_OPS(LDB_RESOLVE)
#undef LDB_RESOLVE
            return raw;
        }

        // Identity of a binding: the profile plus every resolved address, in the
        // fixed order of the raw table. Comparing real fields (never a struct
        // memcmp, which would read padding) is what keeps two same-version
        // modules apart and lets two descriptions of one VM share a binding.
        std::string identity_of(Profile profile, const Raw& raw) {
            std::string key(profile_name(profile));
            char buffer[32];
#define LDB_IDENTITY(field, symbol)                                                                                               \
    std::snprintf(buffer, sizeof(buffer), ":%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(raw.field))); \
    key += buffer;
            LDB_RAW_OPS(LDB_IDENTITY)
#undef LDB_IDENTITY
            return key;
        }

        // Decides the ABI/layout profile when the caller did not name one.
        //
        // `lua_ident` is exact, but a stripped image only has it if the caller
        // named its address. Otherwise the answer comes from positive markers
        // only: a missing 5.5 symbol proves nothing about a table that may be
        // partial, so an undecidable case reports what it needs instead of
        // calling `lua_gettop` through a guessed layout.
        bool detect_profile(const Sources& sources, const Raw& raw, Profile& out, std::string& error) {
            debugger::ProfileEvidence evidence;
            evidence.luajit     = raw.setmode != nullptr;
            evidence.lua55_only = sources.present("lua_numbertocstring") || sources.present("luaL_openselectedlibs");
            evidence.lua54_plus = sources.present("lua_resetthread") || sources.present("lua_newuserdatauv");
            evidence.lua53_plus = sources.present("lua_isinteger");
            evidence.lua52_plus = sources.present("lua_rawlen") || sources.present("lua_callk") || sources.present("luaL_loadbufferx");
            // Only an image whose symbol list actually answered can make an
            // absence informative; a partial external table (or a stripped image
            // with a valid handle) must stay "unknown" instead of falling back to
            // the oldest version.
            evidence.complete = sources.image_answered();

            int decided = debugger::kProfileUnknown;
            if (void* ident = sources.find("lua_ident")) {
                std::string text;
                if (read_cstring(ident, kLuaIdentBytes, text)) {
                    decided = debugger::profile_from_lua_ident(text);
                }
            }
            if (decided == debugger::kProfileUnknown) {
                decided = debugger::profile_from_evidence(evidence);
            }
            if (decided == debugger::kProfileUnknown) {
                error.clear();
                return false;
            }
            out = static_cast<Profile>(decided);
            return true;
        }

        std::unique_ptr<Vm> build_vm(const VmSpec& spec, Profile profile, const Sources& sources, std::string& error) {
            const Api* api = backend_for(profile);
            if (!api) {
                error = std::string("no backend for profile ") + profile_name(profile);
                return nullptr;
            }
            const Raw raw = resolve_raw(sources);
            // Required by every profile; `lua_getglobal` is deliberately absent
            // (5.1 spells it as a macro, and the backend falls back to lua_getfield).
            struct Required {
                const void* fn;
                const char* name;
            };
            const Required required[] = {
                { reinterpret_cast<const void*>(raw.gettop), "lua_gettop" },
                { reinterpret_cast<const void*>(raw.settop), "lua_settop" },
                { reinterpret_cast<const void*>(raw.type), "lua_type" },
                { reinterpret_cast<const void*>(raw.pushvalue), "lua_pushvalue" },
                { reinterpret_cast<const void*>(raw.getfield), "lua_getfield" },
                { reinterpret_cast<const void*>(raw.rawget), "lua_rawget" },
            };
            for (const Required& entry : required) {
                if (!entry.fn) {
                    error = std::string("the target VM symbol source does not provide ") + entry.name + " (profile " + profile_name(profile) + ")";
                    if (spec.module.valid()) {
                        error += ", neither from the table, the resolver, nor module " + spec.module.path;
                    }
                    return nullptr;
                }
            }
            // A function pointer outside an executable mapping is a caller error:
            // calling it would fault inside the target instead of failing here.
            std::string bad;
            auto check_code = [&bad](const void* fn, const char* name) {
                if (fn && bad.empty() && !mapping_executable(fn)) {
                    bad = name;
                }
            };
#define LDB_CHECK_CODE(field, symbol) \
    check_code(reinterpret_cast<const void*>(raw.field), #symbol);
            LDB_RAW_OPS(LDB_CHECK_CODE)
#undef LDB_CHECK_CODE
            if (!bad.empty()) {
                error = "the target VM symbol `" + bad + "` is not in an executable mapping";
                return nullptr;
            }
            if (api->probe) {
                std::string why;
                if (!api->probe(&raw, &why)) {
                    error = std::string("the target VM does not behave like ") + profile_name(profile) + ": " + why;
                    return nullptr;
                }
            }
            return std::make_unique<Vm>(profile, identity_of(profile, raw), api, raw);
        }

        // The state registry is authoritative for "which VM owns this state", so a
        // state already bound to one VM cannot be rebound to another: hook
        // trampolines and the worker look bindings up by raw state pointer.
        bool state_conflicts(lua_State* state, const std::string& identity, std::string& error) {
            State* existing = state ? State::find(state) : nullptr;
            if (!existing || !existing->vm()) {
                return false;
            }
            if (existing->vm()->identity() == identity) {
                return false;
            }
            error = std::string("the target state is already bound to a different VM (") + existing->vm()->identity() + "); refusing to rebind it to " + identity;
            return true;
        }

        struct VmRegistry {
            std::mutex mutex;
            std::vector<std::unique_ptr<Vm>> vms;
        };

        VmRegistry& vms() {
            static VmRegistry* instance = new VmRegistry();
            return *instance;
        }

    }  // namespace

    namespace {
        // Two-entry direct-mapped cache in front of the registry. It is a cache:
        // a miss (or a stale hit after a state was reused) simply takes the
        // registry path, which is authoritative.
        struct Cache {
            lua_State* keys[4] = {};
            State* values[4]   = {};
        };
        thread_local Cache cache;

        inline std::size_t cache_slot(lua_State* raw) {
            return (reinterpret_cast<std::uintptr_t>(raw) >> 4) & 3;
        }

        void cache_put(lua_State* raw, State* state) {
            const std::size_t slot = cache_slot(raw);
            cache.keys[slot]       = raw;
            cache.values[slot]     = state;
        }

        void cache_clear(lua_State* raw) {
            const std::size_t slot = cache_slot(raw);
            if (cache.keys[slot] == raw) {
                cache.keys[slot]   = nullptr;
                cache.values[slot] = nullptr;
            }
        }
    }  // namespace

    State* State::find(lua_State* raw) {
        if (!raw) {
            return nullptr;
        }
        // Only resolved states may be cached: a cached miss would survive the
        // later registration of that state (coroutine states are registered when
        // the first hook is armed on them), and every hook event of that state
        // would be dropped for the rest of the session.
        const std::size_t slot = cache_slot(raw);
        if (cache.keys[slot] == raw && cache.values[slot]) {
            return cache.values[slot];
        }
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.mutex);
        auto it      = reg.states.find(raw);
        State* state = it == reg.states.end() ? nullptr : it->second;
        if (state) {
            cache_put(raw, state);
        }
        return state;
    }

    Vm* vm_of(lua_State* raw) {
        State* state = State::find(raw);
        return state ? state->vm() : nullptr;
    }

    Vm::Vm(Profile profile, std::string identity, const Api* api, const Raw& raw)
        : profile_(profile)
        , identity_(std::move(identity))
        , api_(api)
        , raw_(raw) {}

    Vm::~Vm() {
        forget_all();
    }

    State* Vm::state(lua_State* raw) {
        if (!raw) {
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = states_.find(raw);
            if (it != states_.end()) {
                return it->second.get();
            }
        }
        // Create outside `mutex_`: State construction is trivial, but the global
        // registry takes its own lock and lock order must stay acyclic.
        auto owned   = std::make_unique<State>(raw, this);
        State* state = owned.get();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = states_.find(raw);
            if (it != states_.end()) {
                return it->second.get();
            }
            states_.emplace(raw, std::move(owned));
        }
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.mutex);
        reg.states[raw] = state;
        cache_put(raw, state);
        return state;
    }

    State* Vm::find(lua_State* raw) const {
        if (!raw) {
            return nullptr;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = states_.find(raw);
        return it == states_.end() ? nullptr : it->second.get();
    }

    void Vm::forget(lua_State* raw) {
        if (!raw) {
            return;
        }
        {
            Registry& reg = registry();
            std::lock_guard<std::mutex> guard(reg.mutex);
            reg.states.erase(raw);
        }
        cache_clear(raw);
        std::lock_guard<std::mutex> guard(mutex_);
        states_.erase(raw);
    }

    void Vm::forget_all() {
        std::unordered_map<lua_State*, std::unique_ptr<State>> owned;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            owned.swap(states_);
        }
        if (owned.empty()) {
            return;
        }
        Registry& reg = registry();
        std::lock_guard<std::mutex> guard(reg.mutex);
        for (const auto& entry : owned) {
            auto it = reg.states.find(entry.first);
            if (it != reg.states.end() && it->second == entry.second.get()) {
                reg.states.erase(it);
            }
        }
    }

    std::size_t Vm::state_count() {
        std::lock_guard<std::mutex> guard(mutex_);
        return states_.size();
    }

    Vm* bind_spec(const VmSpec& spec, lua_State* state, std::string& error) {
        Sources sources;
        sources.symbols       = spec.symbols;
        sources.count         = spec.symbol_count;
        sources.module        = &spec.module;
        sources.resolver      = spec.resolver;
        sources.resolver_user = spec.resolver_user;
        if (!sources.usable()) {
            error = "the descriptor names neither a symbol table, a resolver, nor a module";
            return nullptr;
        }
        if (spec.symbol_count > 0 && !spec.symbols) {
            error = "the descriptor declares symbols without a table";
            return nullptr;
        }
        // Evidence is always consulted, even when the caller named a profile: an
        // explicit value is a constraint, and a table that proves it wrong must be
        // refused here rather than after a hook is installed on a mismatched
        // layout. Undecidable evidence (a partial table) is not a contradiction -
        // that is exactly when the caller's own value is authoritative.
        Profile detected   = Profile::lua51;
        const bool decided = detect_profile(sources, resolve_raw(sources), detected, error);
        Profile profile    = Profile::lua51;
        if (spec.profile < 0) {
            if (!decided) {
                error =
                    "cannot determine the Lua ABI of the target VM: the supplied symbols "
                    "are compatible with more than one version, so an explicit profile "
                    "(luaVersion) is required";
                return nullptr;
            }
            profile = detected;
        } else if (spec.profile >= static_cast<int>(Profile::lua51) && spec.profile <= static_cast<int>(Profile::luajit)) {
            profile = static_cast<Profile>(spec.profile);
            if (decided && detected != profile) {
                error = std::string("the target VM evidence says ") + profile_name(detected) + " but the caller asked for " + profile_name(profile);
                return nullptr;
            }
        } else {
            error = "the descriptor names an unknown profile";
            return nullptr;
        }

        VmRegistry& reg = vms();
        std::lock_guard<std::mutex> guard(reg.mutex);
        auto vm = build_vm(spec, profile, sources, error);
        if (!vm) {
            return nullptr;
        }
        // Deduplication happens *after* the full resolve and probe: only a
        // completely validated binding has an identity worth comparing.
        for (const auto& existing : reg.vms) {
            if (existing->identity() == vm->identity()) {
                if (state) {
                    if (state_conflicts(state, vm->identity(), error)) {
                        return nullptr;
                    }
                    if (!existing->state(state)) {
                        error = "cannot adopt the target state";
                        return nullptr;
                    }
                }
                return existing.get();
            }
        }
        if (state) {
            if (state_conflicts(state, vm->identity(), error)) {
                return nullptr;
            }
            if (!vm->state(state)) {
                error = "cannot adopt the target state";
                return nullptr;
            }
        }
        Vm* raw = vm.get();
        reg.vms.push_back(std::move(vm));
        return raw;
    }

    Vm* bind_module(Profile profile, const Module& module, std::string& error) {
        if (!module.valid()) {
            error = "no module identity for the target VM";
            return nullptr;
        }
        VmSpec spec;
        spec.profile       = static_cast<int>(profile);
        spec.module        = module;
        spec.resolver      = &raw_resolver;
        spec.resolver_user = module.handle;
        return bind_spec(spec, nullptr, error);
    }

    Vm* bind_module_at(Profile profile, const void* address, std::string& error) {
        const Module module = module_by_address(address);
        if (!module.valid()) {
            error = "cannot resolve the module that called into the bridge";
            return nullptr;
        }
        return bind_module(profile, module, error);
    }

    void release_module(Vm* vm) {
        if (!vm) {
            return;
        }
        std::unique_ptr<Vm> owned;
        VmRegistry& reg = vms();
        {
            std::lock_guard<std::mutex> guard(reg.mutex);
            for (auto it = reg.vms.begin(); it != reg.vms.end(); ++it) {
                if (it->get() == vm) {
                    owned = std::move(*it);
                    reg.vms.erase(it);
                    break;
                }
            }
        }
        // `owned` destructs outside the lock: it releases every state.
    }

}  // namespace luadebug::vm
