-- Function-line breakpoints added after the function already ran: the session
-- classifies a prototype on its first call, and that classification has to be
-- dropped when the breakpoint table changes. `plain` is the prototype whose
-- line events the debugger must skip cheaply, `target` is the one that gains a
-- breakpoint while the target is stopped.
local trace_path = os.getenv("LUA_DEBUG_E2E_TRACE") or "e2e_trace.txt"
local trace = assert(io.open(trace_path, "w"))

local function plain(n)
    local total = 0
    for i = 1, 100 do
        total = total + i
    end
    return total + n
end

local function target(n)
    return n + 1
end

local first = target(1)
local work = plain(10)
trace:write("work=" .. tostring(work) .. "\n")
trace:flush()
local second = target(work)
trace:write("second=" .. tostring(second) .. "\n")
trace:flush()

local co = coroutine.wrap(function()
    return plain(1)
end)
local co_value = co()
trace:write("co=" .. tostring(co_value) .. "\n")
trace:write("done\n")
trace:close()
