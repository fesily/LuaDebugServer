-- Coroutine fixture: the breakpoints are inside a coroutine body, which runs on
-- its own `lua_State` (and, from 5.2 on, its own hook mask).
local function worker(n)
    local a = n + 1
    coroutine.yield(a)
    local b = a * 2
    return b
end

-- Not `return worker(1)`: a tail call leaves `worker` without a caller frame, so
-- the debugger cannot name it (LuaJIT reports "(anonymous function)" there).
local co = coroutine.create(function()
    local result = worker(1)
    return result
end)
local ok, value = coroutine.resume(co)
assert(ok and value == 2, "first resume")
local ok2, out = coroutine.resume(co)
assert(ok2 and out == 4, "second resume")
print("done", out)
