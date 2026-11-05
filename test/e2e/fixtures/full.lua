-- Full-surface fixture: conditional / hit-count / log / function breakpoints,
-- memory reads, disassembly source and exception stops.
local trace_path = os.getenv("LUA_DEBUG_E2E_TRACE") or "e2e_trace.txt"
local trace = assert(io.open(trace_path, "w"))

local function hit(n)
    return n * 2
end

local total = 0
for i = 1, 5 do
    total = total + hit(i)
end
local text = "hello-memory"
trace:write("total=" .. tostring(total) .. "\n")
trace:flush()

local function thrower()
    error("boom")
end

local ok = pcall(thrower)
trace:write("ok=" .. tostring(ok) .. "\n")
trace:write("done\n")
trace:close()
