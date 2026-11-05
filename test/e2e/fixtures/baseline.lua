-- Baseline fixture: exercises breakpoint, step in/out, local scopes and
-- debugger-driven assignment. Progress is written to the file named by
-- LUA_DEBUG_E2E_TRACE so the harness can prove real execution even when the
-- adapter drops its socket before the last output events are delivered.
local trace_path = os.getenv("LUA_DEBUG_E2E_TRACE") or "e2e_trace.txt"
local trace = assert(io.open(trace_path, "w"))
trace:write("start\n")
trace:flush()

local function add(a, b)
    local sum = a + b
    return sum
end

local x = 1
local y = add(x, 2)
trace:write("y=" .. tostring(y) .. "\n"); trace:flush(); print("y=" .. tostring(y))
local z = y * 2
trace:write("z=" .. tostring(z) .. "\n")
trace:flush()
local t = { k = z }
trace:write("k=" .. tostring(t.k) .. "\n")
trace:write("done\n")
trace:close()
