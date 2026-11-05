-- Module with its own breakpoint: the client sets breakpoints per source, and a
-- second source must never drop the first one's entries.
local M = {}

function M.step(value)
    local doubled = value * 2
    return doubled
end

return M
