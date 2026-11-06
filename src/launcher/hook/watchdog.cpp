#include <hook/watchdog.h>

#include <utility>
#include <util/log.h>

#include <algorithm>
#include <cassert>
#include <map>

namespace luadebug::autoattach {

    namespace {
        // Per-state hook bookkeeping. A plain C hook function cannot capture a
        // context, so the trampoline looks the state up here; the record carries
        // the module's own API, which keeps two modules in one process apart.
        struct hook_table {
            std::mutex mutex;
            std::map<lua::state, watchdog::hook_record> records;
        };

        hook_table& hooks() {
            static hook_table instance;
            return instance;
        }

        // The pre-attach hook. It restores the target's own hook before running
        // the debugger's bootstrap, so the bootstrap (and the session it starts)
        // sees the state exactly as the target left it.
        void attach_trampoline(lua::state L, lua::debug ar) {
            watchdog::hook_record record;
            watchdog* owner = nullptr;
            {
                hook_table& table = hooks();
                std::lock_guard<std::mutex> guard(table.mutex);
                auto it = table.records.find(L);
                if (it == table.records.end()) {
                    return;
                }
                record = it->second;
                owner  = record.owner;
            }
            if (record.origin_hook) {
                record.origin_hook(L, ar);
            }
            if (!owner) {
                return;
            }
            const lua_api& api = owner->api();
            // Restore only what we captured, and never touch a hook that is not
            // ours: the bridge installs its own hook while the bootstrap runs.
            if (watchdog::owns_hook(api, L)) {
                api.sethook(L, record.origin_hook, record.origin_mask, record.origin_count);
            }
            owner->attach_lua(L, ar);
        }
    }  // namespace

    watchdog::hook_record* watchdog::find_hook(lua::state L, bool erase) {
        hook_table& table = hooks();
        std::lock_guard<std::mutex> guard(table.mutex);
        auto it = table.records.find(L);
        if (it == table.records.end()) {
            return nullptr;
        }
        if (!erase) {
            return &it->second;
        }
        table.records.erase(it);
        return nullptr;
    }

    bool watchdog::owns_hook(const lua_api& api, lua::state L) {
        if (!api.gethook) {
            return true;
        }
        lua::hook current = api.gethook(L, nullptr);
        return current == nullptr || current == attach_trampoline;
    }

    int watchdog::luahook_entry(lua::state L) {
        hook_table& table = hooks();
        std::lock_guard<std::mutex> guard(table.mutex);
        auto it = table.records.find(L);
        if (it == table.records.end()) {
            return 0;
        }
        watchdog* owner = it->second.owner;
        if (!owner) {
            return 0;
        }
        const lua_api& api = owner->api();
        if (!api.gethook) {
            return 0;
        }
        // Someone else owns the hook now (the debugger attached from another
        // path): leave it alone.
        if (!owns_hook(api, L)) {
            return 0;
        }
        if (!it->second.snapshot_taken) {
            it->second.origin_hook    = api.gethook(L, nullptr);
            it->second.origin_mask    = api.gethookmask(L);
            it->second.origin_count   = api.gethookcount(L);
            it->second.snapshot_taken = true;
        }
        api.sethook(L, attach_trampoline, lua::mask_call | lua::mask_line | lua::mask_ret, 0);
        return 0;
    }

    watchdog::watchdog(const lua_api& api, const lua_resolver& resolver, lua_version version)
        : api_(api)
        , resolver_(resolver)
        , version_(version)
        , interceptor { Gum::Interceptor_obtain() } {}

    watchdog::~watchdog() {
        stop_discovery_deadline();
        unhook();
    }

    bool watchdog::init() {
        return interceptor != nullptr;
    }

    bool watchdog::init_watch(std::vector<watch_point>&& points) {
        // A version's candidate list is a *menu*: an interpreter may hide some of
        // the internal functions (or the build may rename them), so one resolved
        // point is enough to make the module observable.
        //
        // Two names can also resolve to one function (`lua_pcall`/`lua_pcallk` in
        // a compatibility build): `hook()` cannot install the same point twice,
        // and one listener per address is all that is needed.
        bool any = false;
        std::vector<void*> seen;
        for (auto& point : points) {
            if (!point.find_symbol(resolver_)) {
                continue;
            }
            if (std::find(seen.begin(), seen.end(), point.address) != seen.end()) {
                point.address = nullptr;
                continue;
            }
            seen.push_back(point.address);
            any = true;
        }
        if (any) {
            watch_points = std::move(points);
        }
        return any;
    }

    bool watchdog::hook() {
        std::lock_guard guard(mtx);
        for (auto& point : watch_points) {
            if (!point.address) {
                continue;
            }
            bool ok;
            switch (point.listener) {
            case watch_point::type::common:
                ok = interceptor->attach(point.address, point.persistent ? &listener_persistent : &listener_common, this);
                break;
            case watch_point::type::luajit_global:
                ok = interceptor->attach(point.address, &listener_luajit_global, this);
                break;
            case watch_point::type::luajit_jit:
                ok = interceptor->attach(point.address, &listener_luajit_jit, this);
                break;
            case watch_point::type::ret:
            default:
                ok = interceptor->attach(point.address, &listener_ret, this);
                break;
            }
            if (!ok) {
                log::error("watchdog: cannot attach {}", point.funcname);
                return false;
            }
        }
        arm_discovery_deadline();
        return true;
    }

    void watchdog::unhook() {
        std::lock_guard guard(mtx);
        if (!interceptor) {
            return;
        }
        interceptor->detach(&listener_common);
        interceptor->detach(&listener_persistent);
        interceptor->detach(&listener_luajit_global);
        interceptor->detach(&listener_luajit_jit);
        interceptor->detach(&listener_ret);
        hot_points_released = true;
    }

    void watchdog::arm_discovery_deadline() {
        if (discovery_thread.joinable()) {
            return;
        }
        discovery_thread = std::thread([this] {
            std::unique_lock lock(discovery_mtx);
            if (discovery_cv.wait_for(lock, discovery_window, [this] { return discovery_stopped; })) {
                return;  // the watchdog is going away
            }
            lock.unlock();
            discovery_deadline_reached();
        });
    }

    void watchdog::stop_discovery_deadline() {
        {
            std::lock_guard guard(discovery_mtx);
            discovery_stopped = true;
        }
        discovery_cv.notify_all();
        if (discovery_thread.joinable()) {
            discovery_thread.join();
        }
    }

    void watchdog::release_hot_points() {
        std::lock_guard guard(mtx);
        if (!interceptor || hot_points_released) {
            return;
        }
        // `detach(listener)` drops every address that listener was attached to,
        // so the persistent points have their own instance and survive.
        interceptor->detach(&listener_common);
        interceptor->detach(&listener_luajit_global);
        interceptor->detach(&listener_luajit_jit);
        hot_points_released = true;
    }

    void watchdog::discovery_deadline_reached() {
        bool observed;
        {
            std::lock_guard guard(mtx);
            observed = observed_state;
        }
        // The per-call trampolines have done their job; creation and library
        // setup stay armed so a VM born later is still discovered.
        release_hot_points();
        if (!observed) {
            log::fatal(
                "no Lua state was seen within {} seconds; only creation entries stay armed",
                discovery_window.count()
            );
            return;
        }
        log::info("watchdog: per-call watch points released after {} seconds", discovery_window.count());
    }

    void watchdog::install_luahook(lua::state L) {
        if (!api_.sethook) {
            return;
        }
        {
            hook_table& table = hooks();
            std::lock_guard<std::mutex> guard(table.mutex);
            if (table.records.find(L) == table.records.end()) {
                hook_record record;
                record.owner = this;
                table.records.emplace(L, record);
            }
        }
        // (Re)installs the pre-attach trampoline. Re-arming is the point: while the
        // target is still opening its libraries the bootstrap returns "wait", and
        // the next event has to try again.
        luahook_entry(L);
    }

    void watchdog::attach_lua(lua::state L, lua::debug ar) {
        switch (autoattach::attach_lua(L, api_, version_)) {
        case attach_status::fatal:
        case attach_status::success:
            // Attached: the hook now belongs to the session (or to nobody).
            find_hook(L, true);
            break;
        case attach_status::wait:
            // Not ready yet: re-arm only when nothing else owns the hook.
            install_luahook(L);
            break;
        default:
            std::unreachable();
        }
    }

    void watchdog::watch_entry(lua::state L) {
        // A creation return hook fires even when creation failed: on 64-bit
        // LuaJIT without GC64 `lua_newstate` is a stub that returns NULL, and a
        // PUC `lua_newstate` returns NULL when the allocator fails. Installing a
        // hook on NULL would call the target's `lua_sethook` with a null state.
        if (!L) {
            return;
        }
        std::lock_guard guard(mtx);
        observed_state = true;
        for (lua::state existing : state_hooked) {
            if (existing == L) {
                return;
            }
        }
        install_luahook(L);
        state_hooked.push_back(L);
    }

}  // namespace luadebug::autoattach
