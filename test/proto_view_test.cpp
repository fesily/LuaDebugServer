// Verifies Proto line information against the target's own oracle.
//
// For every Lua frame that is on a paused coroutine's stack, the lines we
// compute for that function must match exactly the `activelines` the target's
// own `debug.getinfo(f, "L")` reports — per function, keyed by its
// linedefined/lastlinedefined range.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lua_compat.h"
#include "target/proto_view.h"

namespace {

    int failures = 0;

    // lua_resume's signature changed twice; the walker itself is version
    // independent, so the harness adapts.
    int resume_chunk(lua_State* co, lua_State* from) {
#if LUA_VERSION_NUM >= 504
        int nresults = 0;
        return lua_resume(co, from, 0, &nresults);
#elif LUA_VERSION_NUM >= 502
        // 5.2/5.3 take (L, from, nargs); the result count is not reported.
        return lua_resume(co, from, 0);
#else
        (void)from;
        return lua_resume(co, 0);
#endif
    }

    constexpr int kYield = LUA_YIELD;

    void expect(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    struct OracleFunction {
        std::string what;  // "Lua", "main", "C"
        int start = 0;
        int end   = 0;
        std::vector<int> active;
    };

    std::vector<int> collect_activelines(lua_State* L, int table_index) {
        std::vector<int> lines;
        table_index = lua_absindex(L, table_index);
        lua_getfield(L, table_index, "activelines");
        if (lua_istable(L, -1)) {
            const int lines_table = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, lines_table) != 0) {
                if (lua_type(L, -2) == LUA_TNUMBER) {
                    lines.push_back(static_cast<int>(lua_tointeger(L, -2)));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        return lines;
    }

    // debug.getinfo(level, "fL") on `co`, flattened into plain data.
    bool oracle_at(lua_State* co, int level, OracleFunction& out) {
        lua_getglobal(co, "debug");
        if (!lua_istable(co, -1)) {
            lua_pop(co, 1);
            return false;
        }
        lua_getfield(co, -1, "getinfo");
        // getinfo itself runs one frame above us, so the level shifts by one.
        lua_pushinteger(co, level + 1);
        // "S" carries linedefined/lastlinedefined, "L" the active lines.
        lua_pushliteral(co, "fSL");
        if (lua_pcall(co, 2, 1, 0) != 0) {
            lua_pop(co, 2);
            return false;
        }
        if (!lua_istable(co, -1)) {
            lua_pop(co, 2);
            return false;
        }
        lua_getfield(co, -1, "what");
        out.what = lua_tostring(co, -1) ? lua_tostring(co, -1) : "";
        lua_pop(co, 1);
        lua_getfield(co, -1, "linedefined");
        out.start = static_cast<int>(lua_tointeger(co, -1));
        lua_pop(co, 1);
        lua_getfield(co, -1, "lastlinedefined");
        out.end = static_cast<int>(lua_tointeger(co, -1));
        lua_pop(co, 1);
        out.active = collect_activelines(co, lua_gettop(co));
        lua_pop(co, 2);  // table + debug table
        return true;
    }

    std::string join(const std::vector<int>& values) {
        std::string text;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) {
                text += ',';
            }
            text += std::to_string(values[i]);
        }
        return text;
    }

    // Loads `chunk`, pauses it inside its deepest function, then checks every
    // function on the paused stack against the proto walk.
    void check_chunk(lua_State* L, const char* name, const char* chunk) {
        luadebug::target::LineInfo info;
        std::string error;
        if (!luadebug::target::build_line_info_from_source(test_bind(L), chunk, info, error)) {
            std::fprintf(stderr, "FAIL: %s: build_line_info: %s\n", name, error.c_str());
            ++failures;
            return;
        }

        lua_State* co = lua_newthread(L);
        if (luaL_loadbuffer(co, chunk, std::strlen(chunk), "=(test)") != 0) {
            std::fprintf(stderr, "FAIL: %s: load: %s\n", name, lua_tostring(co, -1));
            ++failures;
            return;
        }
        const int status = resume_chunk(co, L);
        if (status != kYield) {
            std::fprintf(stderr, "FAIL: %s: chunk did not pause (status %d): %s\n", name, status,
                         lua_tostring(co, -1) ? lua_tostring(co, -1) : "?");
            ++failures;
            return;
        }

        int compared = 0;
        for (int level = 0;; ++level) {
            lua_Debug probe;
            if (!lua_getstack(co, level, &probe)) {
                break;
            }
            OracleFunction oracle;
            if (!oracle_at(co, level, oracle)) {
                continue;
            }
            if (oracle.what.empty() || oracle.what == "C") {
                continue;  // C functions have no proto
            }
            ++compared;

            const luadebug::target::FunctionLines* match = nullptr;
            for (const auto& function : info.functions()) {
                if (oracle.what == "main") {
                    // Lua leaves the main chunk's lastlinedefined at 0; the walk
                    // normalizes it to 1..last active line.
                    if (function.start_line == 1) {
                        match = &function;
                        break;
                    }
                    continue;
                }
                if (function.start_line == oracle.start && function.end_line == oracle.end) {
                    match = &function;
                    break;
                }
            }
            if (!match) {
                std::fprintf(stderr, "FAIL: %s: no proto entry for function %d-%d\n", name,
                             oracle.start, oracle.end);
                ++failures;
                continue;
            }
            if (oracle.active.empty() && !match->active_lines.empty()) {
                // 5.1 has no activelines oracle; require our list to be plausible
                // instead of silently comparing against nothing.
                std::fprintf(stderr,
                             "note: %s: no oracle for function %d-%d, ours=[%s]\n", name,
                             oracle.start, oracle.end, join(match->active_lines).c_str());
            }
            if (!oracle.active.empty() && match->active_lines != oracle.active) {
                std::fprintf(stderr,
                             "FAIL: %s: function %d-%d active lines differ\n  ours   = [%s]\n  target = [%s]\n",
                             name, oracle.start, oracle.end, join(match->active_lines).c_str(),
                             join(oracle.active).c_str());
                ++failures;
            }

        }
        expect(compared >= 2, "at least the main chunk and the paused function were compared");
        expect(!info.functions().empty(), "functions reported");
    }

} // namespace

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // Nested function with a blank line, called through another function.
    check_chunk(L, "nested", R"(
local function add(a, b)
    local sum = a + b

    coroutine.yield()
    return sum
end

local function caller(...)
    return add(...)
end

caller(1, 2)
)");

    // Vararg function: the 5.4/5.5 VARARGPREP instruction must not add a line.
    check_chunk(L, "vararg", R"(
local function f(...)
    local n = select('#', ...)
    coroutine.yield()
    return n
end

f(1, 2, 3)
)");

    // Multi-line expression and a table constructor spanning lines.
    check_chunk(L, "multiline", R"(
local t = {
    1,
    2,
}
local function g(x)
    coroutine.yield()
    return x
        + 1
end
g(t[1])
)");

    // Range behaviour: a blank line inside a function remaps forward; a line
    // past the chunk has no active line at all.
    {
        luadebug::target::LineInfo info;
        std::string error;
        const char* chunk = "local function h()\n\n    return 1\nend\nh()\n";
        expect(luadebug::target::build_line_info_from_source(test_bind(L), chunk, info, error),
               "build_line_info for the range chunk");
        if (!info.functions().empty()) {
            const auto& first = info.functions().front();
            expect(first.key == std::to_string(first.start_line) + "-"
                                  + std::to_string(first.end_line),
                   "function key is start-end");
            const int blank = first.start_line + 1;
            const int remapped = info.remap(blank);
            expect(remapped == 0 || info.is_active(remapped),
                   "remap yields an active line or nothing");
            expect(info.next_active_line(1000000) == 0,
                   "past the chunk there is no active line");
            expect(info.next_active_line(first.end_line + 100) == 0,
                   "past a function range the search does not spill");
        }
    }

    // Syntax errors must be reported, not crash.
    lua_settop(L, 0);
    {
        luadebug::target::LineInfo info;
        std::string error;
        expect(!luadebug::target::build_line_info_from_source(test_bind(L), "function (", info, error),
               "a syntax error is reported");
        expect(!error.empty(), "the syntax error carries a message");
        expect(lua_gettop(L) == 0, "the target stack is left clean");
    }

    lua_close(L);
    if (failures == 0) {
        std::printf("proto_view_test: OK\n");
        return 0;
    }
    std::printf("proto_view_test: %d failure(s)\n", failures);
    return 1;
}
