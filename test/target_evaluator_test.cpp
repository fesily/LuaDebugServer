// Verifies target expression evaluation at a stop:
//   * expression mode sees the frame's locals, upvalues and varargs,
//   * it never writes bindings back (readonly),
//   * statement mode writes back only directly referenced names,
//   * a failing statement leaves the bindings untouched,
//   * the frame's environment resolves globals as the frame sees them.

#include <cstdio>
#include <cstring>
#include <string>

#include "lua_compat.h"
#include "target/value_store.h"
#include "util/refvalue.h"
#include "target/evaluator.h"

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    // worker(...) pauses with locals a, b, an upvalue `up` and varargs.
    const char* kChunk = R"(
        local up = 100
        function worker(...)
            local a = 42
            local b = { x = 7 }
            coroutine.yield()
            return a, up
        end
        return worker
    )";

    struct Paused {
        lua_State* L = nullptr;
        lua_State* co = nullptr;
    };

    bool pause_frame(Paused& out) {
        out.L = luaL_newstate();
        luaL_openlibs(out.L);
        if (luaL_dostring(out.L, kChunk) != LUA_OK) {
            std::fprintf(stderr, "chunk: %s\n", lua_tostring(out.L, -1));
            return false;
        }
        // worker is on the stack as the chunk's result.
        out.co = lua_newthread(out.L);
        lua_pushvalue(out.L, -2);      // the worker function
        lua_xmove(out.L, out.co, 1);
        int nresults = 0;
        lua_pushinteger(out.co, 5);    // a vararg
        lua_pushinteger(out.co, 6);
        if (lua_resume(out.co, out.L, 2, &nresults) != LUA_YIELD) {
            std::fprintf(stderr, "resume: %s\n",
                         lua_tostring(out.co, -1) ? lua_tostring(out.co, -1) : "?");
            return false;
        }
        return true;
    }

    std::string eval_string(luadebug::target::Evaluator& evaluator, const char* source,
                            bool& ok) {
        auto outcome = evaluator.evaluate(source);
        ok           = outcome.status == luadebug::target::CallStatus::ok;
        if (!ok) {
            std::fprintf(stderr, "  evaluate(%s) failed: %s\n", source, outcome.error.c_str());
            return {};
        }
        return {};
    }

} // namespace

int main() {
    Paused paused;
    if (!pause_frame(paused)) {
        return 1;
    }
    lua_State* co = paused.co;

    // The paused `worker` frame sits above `coroutine.yield`'s C frame, so its
    // absolute level is 1 (level 0 is the yield's C frame).
    // Find the level that actually holds the worker frame from the caller's view.
    int frame_level = -1;
    for (int level = 0; level < 4; ++level) {
        lua_Debug probe;
        if (!lua_getstack(co, level, &probe)) {
            continue;
        }
        if (!lua_getinfo(co, "S", &probe)) {
            continue;
        }
        for (int i = 1;; ++i) {
            const char* name = lua_getlocal(co, &probe, i);
            if (!name) {
                break;
            }
            lua_pop(co, 1);
            if (std::strcmp(name, "a") == 0 && frame_level < 0) {
                frame_level = level;
            }
        }
    }
    expect(frame_level >= 0, "found the frame holding `a`");
    luadebug::target::Evaluator evaluator(test_bind(co), frame_level);
    // Statement write-back goes through the same value store the session uses.
    luadebug::target::ValueStore store(test_bind(co));
    evaluator.set_write_back([&](const luadebug::refvalue::value* path, std::size_t size,
                                 luadebug::vm::State* from, int index) {
        if (size == 0 || !from) {
            return false;
        }
        std::vector<luadebug::refvalue::value> copy(path, path + size);
        auto handle = store.add_path(std::move(copy));
        const int saved = lua_gettop(co);
        from->pushvalue(index);
        from->xmove(test_bind(co), 1);
        std::string error;
        const auto status = store.assign(handle, error);
        lua_settop(co, saved);
        return status == luadebug::target::EvalStatus::ok;
    });

    auto eval = [&](const char* source) -> std::string {
        auto outcome = evaluator.evaluate(source);
        if (outcome.status != luadebug::target::CallStatus::ok) {
            std::fprintf(stderr, "  evaluate(%s): %s\n", source, outcome.error.c_str());
            return "<error>";
        }
        std::string result;
        if (lua_gettop(co) > 0) {
            const char* text = luaL_tolstring(co, -1, nullptr);
            result           = text ? text : "<nil>";
            lua_pop(co, 1);  // luaL_tolstring's string
        }
        lua_settop(co, 0);
        return result;
    };

    expect(eval("a + 1") == "43", "expression sees the local `a`");
    expect(eval("b.x") == "7", "expression reads a table local");
    expect(eval("up") == "100", "expression sees the frame's upvalue");
    expect(eval("select('#', ...)") == "2", "expression sees the frame's varargs");
    expect(eval("(...)") == "5", "first vararg value");
    expect(eval("_VERSION ~= nil and 'env' or 'none'") == "env",
           "globals resolve through the frame environment");

    // readonly: reading through several expressions leaves the binding alone
    expect(eval("a") == "42", "expression mode did not write the binding");

    // statement mode: direct write-back
    {
        auto outcome = evaluator.run("a = a + 5", luadebug::target::EvalMode::statement);
        expect(outcome.status == luadebug::target::CallStatus::ok, "statement executes");
        lua_settop(co, 0);
    }
    expect(eval("a") == "47", "statement mode wrote the local back");

    // statement mode failure: no write-back
    {
        auto outcome = evaluator.run("a = 1; error('boom')", luadebug::target::EvalMode::statement);
        expect(outcome.status == luadebug::target::CallStatus::error, "failing statement reports an error");
        expect(outcome.error.find("boom") != std::string::npos, "error text preserved");
        lua_settop(co, 0);
    }
    expect(eval("a") == "47", "failed statement did not write the binding");

    // A statement that only touches a nested table leaves `a` alone and writes
    // through the referenced table.
    {
        auto outcome = evaluator.run("b.x = 9", luadebug::target::EvalMode::statement);
        if (outcome.status != luadebug::target::CallStatus::ok) {
            std::fprintf(stderr, "  statement(b.x = 9): %s\n", outcome.error.c_str());
        }
        expect(outcome.status == luadebug::target::CallStatus::ok, "member statement executes");
        lua_settop(co, 0);
    }
    expect(eval("type(b)") == "table", "local b still holds its table");
    expect(eval("b.x") == "9", "member write visible through the live table");
    expect(eval("a") == "47", "unrelated local untouched");

    lua_close(paused.L);
    if (failures == 0) {
        std::printf("target_evaluator_test: OK\n");
        return 0;
    }
    std::printf("target_evaluator_test: %d failure(s)\n", failures);
    return 1;
}
