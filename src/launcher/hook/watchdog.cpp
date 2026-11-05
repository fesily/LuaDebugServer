#include <hook/watchdog.h>

#include <utility>
#include <util/log.h>

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
        unhook();
    }

    bool watchdog::init() {
        return interceptor != nullptr;
    }

    bool watchdog::init_watch(std::vector<watch_point>&& points) {
        // A version's candidate list is a *menu*: an interpreter may hide some of
        // the internal functions (or the build may rename them), so one resolved
        // point is enough to make the module observable.
        bool any = false;
        for (auto& point : points) {
            if (point.find_symbol(resolver_)) {
                any = true;
            }
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
                ok = interceptor->attach(point.address, &listener_common, this);
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
        return true;
    }

    void watchdog::unhook() {
        std::lock_guard guard(mtx);
        if (!interceptor) {
            return;
        }
        interceptor->detach(&listener_common);
        interceptor->detach(&listener_luajit_global);
        interceptor->detach(&listener_luajit_jit);
        interceptor->detach(&listener_ret);
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
        std::lock_guard guard(mtx);
        for (lua::state existing : state_hooked) {
            if (existing == L) {
                return;
            }
        }
        install_luahook(L);
        state_hooked.push_back(L);
    }

}  // namespace luadebug::autoattach
