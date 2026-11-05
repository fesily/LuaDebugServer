// Verifies the target-side error boundary used by every bridge call:
//   * results of a successful protected call,
//   * a Lua error becomes status+message with the stack restored,
//   * a C++ exception never escapes into the Lua C callback,
//   * the boundary still works after an error (no leaked stack slots).

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "lua_compat.h"
#include "target/protected.h"

namespace {

    int failures = 0;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    int push_two(luadebug::vm::State* L, void*) {
        L->pushinteger(2);
        return 1;
    }

    int raise_lua_error(luadebug::vm::State* L, void*) {
        L->pushstring("boom from lua");
        return L->error();
    }

    int raise_cpp_error(luadebug::vm::State*, void*) {
        throw std::runtime_error("cpp failure");
    }

    int index_nil_and_throw(luadebug::vm::State* L, void*) {
        L->getglobal("definitely_absent_table");
        L->pushstring("key");
        L->gettable(-2);  // raises "attempt to index a nil value"
        return 1;
    }

} // namespace

int main() {
    lua_State* L = luaL_newstate();
    luadebug::vm::State* S = test_bind(L);
    if (!L) {
        std::fprintf(stderr, "FAIL: luaL_newstate\n");
        return 1;
    }
    luaL_openlibs(L);

    // 1. Successful call
    {
        luadebug::target::StackGuard guard(S);
        auto result = luadebug::target::pcall(S, &push_two, nullptr, 0, 1);
        expect(result.status == luadebug::target::CallStatus::ok, "successful pcall");
        expect(lua_gettop(L) == guard.top() + 1, "successful pcall leaves one result");
        expect(lua_tointeger(L, -1) == 2, "result value");
    }
    expect(lua_gettop(L) == 0, "stack restored after success");

    // 2. Lua error -> status + message, stack restored
    {
        luadebug::target::StackGuard guard(S);
        auto result = luadebug::target::pcall(S, &raise_lua_error, nullptr, 0, 0);
        expect(result.status == luadebug::target::CallStatus::error, "lua error reported");
        expect(result.message.find("boom from lua") != std::string::npos,
               "lua error message preserved");
    }
    expect(lua_gettop(L) == 0, "stack restored after lua error");

    // 3. C++ exception must not cross the C callback
    {
        luadebug::target::StackGuard guard(S);
        auto result = luadebug::target::pcall(S, &raise_cpp_error, nullptr, 0, 0);
        expect(result.status == luadebug::target::CallStatus::error,
               "c++ exception turned into an error status");
        expect(result.message.find("cpp failure") != std::string::npos,
               "c++ exception message preserved");
    }
    expect(lua_gettop(L) == 0, "stack restored after c++ exception");

    // 4. A real target error raised from inside the target's own runtime
    {
        luadebug::target::StackGuard guard(S);
        auto result = luadebug::target::pcall(S, &index_nil_and_throw, nullptr, 0, 1);
        expect(result.status == luadebug::target::CallStatus::error, "runtime error reported");
        expect(result.message.find("attempt to index") != std::string::npos,
               "runtime error message preserved");
    }
    expect(lua_gettop(L) == 0, "stack restored after runtime error");

    // 5. Still usable after all of the above
    {
        luadebug::target::StackGuard guard(S);
        auto result = luadebug::target::pcall(S, &push_two, nullptr, 0, 1);
        expect(result.status == luadebug::target::CallStatus::ok, "usable after errors");
    }

    lua_close(L);
    if (failures == 0) {
        std::printf("target_protected_test: OK\n");
        return 0;
    }
    std::printf("target_protected_test: %d failure(s)\n", failures);
    return 1;
}
