-- Attach fixture: no debugger bootstrap, just an ordinary Lua program that keeps
-- calling a function so a breakpoint set after the attach can still be hit.
--
-- The test sets the gate file once breakpoints are configured, so the loop below
-- only runs when the debugger is ready for it.
local gate = assert(os.getenv("ATTACH_GATE"))

local function step(n)
    local value = n + 1
    return value
end

local waited = 0
while not io.open(gate) do
    -- Keep the VM busy so the launcher's watch points fire and the session comes
    -- up while the test is still issuing requests.
    waited = step(waited)
    local deadline = os.time()
    while os.time() == deadline do end
    if waited > 100000 then
        break
    end
end

for i = 1, 400 do
    step(i)
    local deadline = os.time()
    while os.time() == deadline do end
end
