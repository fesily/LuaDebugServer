// Verifies target value-handle lifetime semantics:
//   * a path handle resolves against the paused frame,
//   * it fails with "invalid_handle" after the stop ends instead of silently
//     resolving against a different frame,
//   * a pinned value survives a full GC and becomes collectable again after
//     release,
//   * assignment through a path handle writes the target binding.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lua_compat.h"
#include "target/value_store.h"

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    const char* kChunk = R"(
        function worker()
            local a = 42
            local b = { x = 7 }
            coroutine.yield()
            a = a + 1
            return a
        end
    )";

    // Runs `worker` on a fresh coroutine and yields inside it, so the coroutine
    // holds a paused frame with real locals.
    bool start_paused(lua_State* L, lua_State*& co) {
        if (luaL_dostring(L, kChunk) != LUA_OK) {
            std::fprintf(stderr, "chunk failed: %s\n", lua_tostring(L, -1));
            return false;
        }
        co            = lua_newthread(L);
        int nresults  = 0;
        lua_getglobal(co, "worker");
        if (lua_resume(co, L, 0, &nresults) != LUA_YIELD) {
            std::fprintf(stderr, "worker did not yield: %s\n",
                         lua_tostring(co, -1) ? lua_tostring(co, -1) : "?");
            return false;
        }
        return true;
    }

    // refvalue::value is a union-based variant with a deleted copy
    // constructor, so paths must be built in place (no initializer_list).
    std::vector<luadebug::refvalue::value> frame_local_path(int level, int index) {
        std::vector<luadebug::refvalue::value> path;
        path.emplace_back(luadebug::refvalue::FRAME_LOCAL {
            static_cast<std::uint16_t>(level), static_cast<std::int16_t>(index) });
        return path;
    }

    // Finds the index of a named local in frame `level` of `co`.
    int local_index_at(lua_State* co, int level, const char* name) {
        lua_Debug ar;
        if (lua_getstack(co, level, &ar) == 0) {
            return -1;
        }
        for (int i = 1;; ++i) {
            const char* local_name = lua_getlocal(co, &ar, i);
            if (!local_name) {
                return -1;
            }
            lua_pop(co, 1);
            if (std::strcmp(local_name, name) == 0) {
                return i;
            }
        }
    }

    struct LocalRef {
        int level = -1;
        int index = -1;
    };

    // Searches a few levels: the yield call itself occupies the top frame.
    LocalRef find_local(lua_State* co, const char* name) {
        for (int level = 0; level < 4; ++level) {
            int index = local_index_at(co, level, name);
            if (index > 0) {
                return { level, index };
            }
        }
        return {};
    }

} // namespace

int main() {
    lua_State* L = luaL_newstate();
    luadebug::vm::State* S = test_bind(L);
    luaL_openlibs(L);
    lua_State* co = nullptr;
    if (!start_paused(L, co)) {
        return 1;
    }

    luadebug::target::ValueStore store(test_bind(co));

    const auto local_a = find_local(co, "a");
    const auto local_b = find_local(co, "b");
    expect(local_a.index > 0, "found local `a`");
    expect(local_b.index > 0, "found local `b`");
    if (local_a.index <= 0 || local_b.index <= 0) {
        lua_close(L);
        return 1;
    }

    // 1. A path handle resolves against the paused frame.
    std::string error;
    auto handle_a = store.add_path(frame_local_path(local_a.level, local_a.index));
    expect(store.push(handle_a, error) == luadebug::target::EvalStatus::ok,
           "path handle resolves");
    expect(lua_tointeger(co, -1) == 42, "local a == 42");
    lua_pop(co, 1);

    // 2. Assignment through the path handle writes the binding.
    lua_pushinteger(co, 99);
    expect(store.assign(handle_a, error) == luadebug::target::EvalStatus::ok,
           "assign through a path handle");
    expect(store.push(handle_a, error) == luadebug::target::EvalStatus::ok,
           "path handle still resolves after assign");
    expect(lua_tointeger(co, -1) == 99, "local a == 99 after assign");
    lua_pop(co, 1);

    // 3. A pinned value survives GC and is released explicitly.
    auto handle_b = store.add_path(frame_local_path(local_b.level, local_b.index));
    expect(store.push(handle_b, error) == luadebug::target::EvalStatus::ok, "table local resolves");

    // Weak table on the debugger state referencing the target table: if the pin
    // is honest, the weak slot stays populated across a full collection.
    auto pinned = store.pin_top();
    expect(pinned != 0, "pin_top returns a handle");
    expect(store.pinned_count() == 1, "one pinned handle");

    lua_gc(co, LUA_GCCOLLECT, 0);
    expect(store.push(pinned, error) == luadebug::target::EvalStatus::ok,
           "pinned value survives GC");

    // 4. Ending the stop invalidates path handles but not pins.
    store.begin_stop();
    expect(store.push(handle_a, error) == luadebug::target::EvalStatus::invalid_handle,
           "stale path handle is rejected");
    expect(store.push(pinned, error) == luadebug::target::EvalStatus::ok,
           "pinned handle survives the stop boundary");
    expect(store.pinned_count() == 1, "pin still counted");

    // 5. Releasing the pin frees it.
    store.release(pinned);
    expect(store.pinned_count() == 0, "pin released");
    expect(store.push(pinned, error) == luadebug::target::EvalStatus::invalid_handle,
           "released pin is rejected");

    store.release_all();
    lua_close(L);

    if (failures == 0) {
        std::printf("target_value_store_test: OK\n");
        return 0;
    }
    std::printf("target_value_store_test: %d failure(s)\n", failures);
    return 1;
}
