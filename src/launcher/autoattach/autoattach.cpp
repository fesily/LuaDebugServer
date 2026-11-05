#include <autoattach/autoattach.h>
#include <autoattach/ctx.h>
#include <autoattach/lua_module.h>
#include <autoattach/wait_dll.h>
#include <fmt/format.h>
#include <common/vm_version.h>
#include <resolver/lua_resolver.h>
#include <resolver/module_lookup.h>
#include <resolver/symbol_source.h>
#include <util/log.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <gumpp.hpp>
#include <memory>
#include <string>
#include <set>
#include <string_view>
#include <thread>

namespace luadebug::autoattach {
    constexpr auto find_lua_module_key = "lua_newstate";
    constexpr auto lua_module_strings  = std::array<const char*, 3> {
        "luaJIT_BC_%s",  // luajit
        " $\n"
        "$Authors: "
        "R. Ierusalimschy, L. H. de Figueiredo & W. Celes"
        " $\n"
        "$URL: www.lua.org $\n",  // lua51
        "$LuaAuthors: "
        "R. Ierusalimschy, L. H. de Figueiredo, W. Celes"
        " $",  // others
    };
    static bool is_lua_module(const Gum::Module& module, bool check_export = true, bool check_strings = false) {
        if (check_export && module.find_export_by_name(find_lua_module_key)) return true;
        if (module.find_symbol_by_name(find_lua_module_key)) return true;
        // TODO: when signature mode, check strings
        if (check_strings) {
            for (auto str : lua_module_strings) {
                if (!Gum::search_module_string(module, str).empty()) {
                    return true;
                }
            }
        }
        return false;
    }
    static bool is_lua_module(const char* module_path, bool check_export = true, bool check_strings = false) {
        bool found = false;
        with_module(module_path, [&](const Gum::Module& module) {
            found = is_lua_module(module, check_export, check_strings);
        });
        return found;
    }

    static void start();

    // Maps a configured `luaVersion` name to the ABI/layout group. Shared with
    // the bridge so the two can never disagree about what "lua-latest" means.
    static int group_of_name(std::string_view name) {
        return debugger::profile_from_name(name);
    }

    static std::string file_name_of(const std::string& path) {
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    static bool load_lua_module(const std::string& path) {
        constexpr auto check_export =
#ifdef _WIN32
            true
#else
            false
#endif
            ;
        // An image an external symbol table named is a candidate even when it
        // exports no Lua API at all - that is the case the table exists for.
        auto context               = ctx::get();
        const bool named_by_source = context->bootstrap_valid && !context->bootstrap.lua_binary.empty() && (path == context->bootstrap.lua_binary || file_name_of(path) == context->bootstrap.lua_binary);
        if (!named_by_source && !is_lua_module(path.c_str(), check_export, true)) {
            return false;
        }
        // find lua module lazy
        std::thread(start).detach();
        return true;
    }

    static lua_module* find_module(ctx* context, const std::string& path) {
        for (auto& existing : context->modules) {
            if (existing->path == path) {
                return existing.get();
            }
        }
        return nullptr;
    }

    // Resolves the external description once, before any candidate is prepared.
    // Everything downstream (API resolution, watch points, the bridge handoff)
    // reads the resulting immutable table.
    static void prepare_source(ctx* context) {
        if (!context->bootstrap_valid || !context->bootstrap.has_vm_source()) {
            return;
        }
        if (context->resolved_ready) {
            return;
        }
        symbol_source_result result;
        std::string error;
        resolved_symbols resolved;
        if (!resolve_vm_source(context->bootstrap, resolved, error, &result)) {
            if (result.module_missing) {
                // The named image is not mapped yet: keep the same late-load path
                // an undiscovered Lua module uses, and retry when it appears.
                log::info("vm source: {}", error);
                if (!context->wait_dll && wait_dll(load_lua_module)) {
                    context->wait_dll = true;
                }
                return;
            }
            log::fatal("vm source: {}", error);
            context->refused = true;
            return;
        }
        log::info("vm source: {} symbols resolved{}", resolved.entries.size(), resolved.module_path.empty() ? "" : (" from " + resolved.module_path));
        context->resolved       = std::move(resolved);
        context->resolved_ready = true;
    }

    // Adds the image the description selected, even when it exports nothing.
    static void add_resolved_candidate(ctx* context) {
        const auto& resolved = context->resolved;
        if (!resolved.valid() || resolved.module_path.empty()) {
            return;
        }
        if (find_module(context, resolved.module_path)) {
            return;
        }
        auto owned            = std::make_unique<lua_module>(context->mode.load());
        owned->memory_address = resolved.module_base;
        owned->memory_size    = resolved.module_size;
        owned->path           = resolved.module_path;
        owned->name           = file_name_of(resolved.module_path);
        log::info("vm source selects {}", owned->path);
        context->modules.push_back(std::move(owned));
    }

    // A complete table may describe a VM in an image that cannot be discovered at
    // all (statically linked, stripped). The table is then the only identity, and
    // the watch points hook the C API it names.
    static void add_external_candidate(ctx* context) {
        static constexpr char kName[] = "<external symbol table>";
        if (find_module(context, kName)) {
            return;
        }
        auto owned           = std::make_unique<lua_module>(context->mode.load());
        owned->external_only = true;
        owned->path          = kName;
        owned->name          = kName;
        context->modules.push_back(std::move(owned));
    }

    // Enumerates every loaded Lua module and keeps the ones we have not seen.
    static void collect_modules(ctx* context) {
        Gum::Process::enumerate_modules([context](const Gum::Module& module) -> bool {
            if (!is_lua_module(module)) {
                return true;
            }
            const char* path = module.path();
            if (!path || find_module(context, path)) {
                return true;
            }
            auto range            = module.range();
            auto owned            = std::make_unique<lua_module>(context->mode.load());
            owned->memory_address = range.base_address;
            owned->memory_size    = range.size;
            owned->path           = path;
            owned->name           = module.name() ? module.name() : "";
            log::info("find lua module path:{}", owned->path);
            context->modules.push_back(std::move(owned));
            return true;
        });
    }

    static void start_impl(luadebug::autoattach::ctx* ctx) {
        // A source that failed is a terminal decision: no hook, no worker, no
        // retry, and no silent fallback to discovery.
        prepare_source(ctx);
        if (ctx->refused) {
            return;
        }
        if (ctx->bootstrap_valid && ctx->bootstrap.has_vm_source() && !ctx->resolved_ready) {
            return;
        }
        collect_modules(ctx);
        if (ctx->resolved_ready) {
            add_resolved_candidate(ctx);
            if (ctx->modules.empty()) {
                add_external_candidate(ctx);
            }
        }
        if (ctx->modules.empty()) {
            if (ctx->wait_dll)
                return;
            if (!wait_dll(load_lua_module)) {
                log::fatal("can't find lua module");
            } else {
                ctx->wait_dll = true;
            }
            return;
        }

        // The session is fixed to one ABI/layout group. A configured Lua version
        // selects it; otherwise the first module that prepares picks it. With
        // several distinct groups and no configuration, injection is ambiguous
        // and nothing is hooked: guessing would attach to the wrong VM.
        int configured = -1;
        if (!ctx->bootstrap.lua_version.empty()) {
            configured = group_of_name(ctx->bootstrap.lua_version);
            if (configured < 0) {
                log::error("unknown luaVersion {}: nothing is injected", ctx->bootstrap.lua_version);
                ctx->refused = true;
                return;
            }
        }

        for (auto& module : ctx->modules) {
            if (!module->prepare()) {
                continue;
            }
        }
        {
            // Nothing prepared is a terminal state, not a quiet no-op: without it
            // the process would keep running with no hook, no worker and no message,
            // and the client would wait for a session that can never start.
            bool prepared = false;
            for (const auto& module : ctx->modules) {
                if (module->prepared()) {
                    prepared = true;
                    break;
                }
            }
            if (!prepared && ctx->wait_dll) {
                // A later image may still arrive; the late-load retry re-runs this.
                return;
            }
            if (!prepared) {
                if (ctx->bootstrap.has_vm_source()) {
                    log::error(
                        "the supplied VM source does not describe any Lua VM in this "
                        "process ({} candidate(s)); nothing is injected",
                        ctx->modules.size()
                    );
                    ctx->refused = true;
                } else if (configured >= 0) {
                    log::error(
                        "luaVersion {} does not match any Lua VM in this process: "
                        "nothing is injected",
                        ctx->bootstrap.lua_version
                    );
                    ctx->refused = true;
                } else {
                    log::error(
                        "no Lua VM in this process could be identified; nothing is "
                        "injected"
                    );
                    ctx->refused = true;
                }
                return;
            }
        }
        if (configured < 0 && ctx->selected_group < 0) {
            std::set<int> groups;
            for (auto& module : ctx->modules) {
                if (module->prepared()) {
                    groups.insert(module->group());
                }
            }
            if (groups.size() > 1) {
                log::error(
                    "{} Lua ABI groups coexist and no luaVersion was configured; "
                    "injection would be ambiguous, so nothing is hooked",
                    groups.size()
                );
                ctx->refused = true;
                return;
            }
            if (!groups.empty()) {
                ctx->selected_group = *groups.begin();
            }
        }
        if (configured >= 0) {
            // An explicit version is a constraint: it selects the group, and if no
            // candidate's own evidence matches it, the request contradicts the
            // process and is refused before any hook is installed.
            bool matched = false;
            for (auto& module : ctx->modules) {
                if (module->prepared() && module->group() == configured) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                log::error(
                    "luaVersion {} does not match any Lua VM in this process: nothing is "
                    "injected",
                    ctx->bootstrap.lua_version
                );
                ctx->refused = true;
                return;
            }
            ctx->selected_group = configured;
        }
        if (ctx->selected_group < 0) {
            return;
        }

        for (auto& module : ctx->modules) {
            if (!module->prepared()) {
                continue;
            }
            if (module->group() != ctx->selected_group) {
                log::info("skip {}: group {} is not the session group {}", module->path, module->group(), ctx->selected_group);
                continue;
            }
            module->attach();
        }
    }

    void start(work_mode mode) {
        auto ctx = ctx::get();
        std::lock_guard guard(ctx->mtx);
        ctx->mode = mode;
        start_impl(ctx);
    }

    void start() {
        auto ctx = ctx::get();
        std::lock_guard guard(ctx->mtx);
        start_impl(ctx);
    }

    std::once_flag initialize_flag;
    void initialize(work_mode mode) {
        std::call_once(initialize_flag, []() {
            log::info("initialize");
            Gum::runtime_init();
        });
        start(mode);
    }

    static work_mode to_work_mode(debugger::BootstrapMode mode) {
        return mode == debugger::BootstrapMode::launch ? work_mode::launch : work_mode::attach;
    }

    bool already_bootstrapped() {
        auto context = ctx::get();
        std::lock_guard guard(context->mtx);
        return context->bootstrapped;
    }

    void bootstrap(const char* data) {
        auto context = ctx::get();

        debugger::BootstrapConfig config;
        std::string error;
        {
            std::lock_guard guard(context->mtx);
            if (context->bootstrapped) {
                // A second injection into the same process keeps the snapshot the
                // live session was built from: never redirect an address that a
                // running session is already using.
                log::info("bootstrap: keep existing snapshot");
                if (!context->bootstrap_valid) {
                    log::error("bootstrap: existing config is invalid: {}", context->bootstrap_error);
                    return;
                }
                config = context->bootstrap;
            } else {
                context->bootstrapped = true;

                // The whole document comes from one carrier: a non-empty data
                // string wins outright, otherwise the target's own environment.
                // Fields are never merged across the two.
                const std::string_view from_data = data ? std::string_view(data) : std::string_view {};
                std::string from_env;
                if (from_data.empty()) {
                    if (const char* env = std::getenv("LUA_DEBUG_CONFIG")) {
                        from_env = env;
                    }
                }
                const std::string_view carrier = from_data.empty() ? std::string_view(from_env)
                                                                   : from_data;

                if (carrier.empty()) {
                    // Nothing configured: keep the historical default (attach
                    // mode, pid-derived socket, auto-detected Lua version).
                    config                   = debugger::BootstrapConfig {};
                    context->bootstrap_valid = true;
                    log::info("bootstrap: no config, using defaults");
                } else if (!debugger::parse_bootstrap(carrier, config, error)) {
                    context->bootstrap_valid = false;
                    context->bootstrap_error = error;
                    log::error("bootstrap: invalid config ({}): {}", from_data.empty() ? "env" : "data", error);
                    // Deliberately no hook installation and no fallback to the
                    // default socket: the session handshake fails instead.
                    return;
                } else {
                    context->bootstrap_valid = true;
                    log::info("bootstrap: config from {}", from_data.empty() ? "env" : "data");
                }
                context->bootstrap = config;
            }
        }
        // Outside the lock: fatal logging is reached from code that holds it.
        log::set_root(config.root);
        initialize(to_work_mode(config.mode));
    }
}
