local path, pid, cfg = ...
if _VERSION == nil
    or type == nil
    or assert == nil
    or tostring == nil
    or error == nil
    or dofile == nil
    or io == nil
    or os == nil
    or debug == nil
    or package == nil
    or string == nil
then
    return "wait initialized"
end

local is_luajit = tostring(assert):match('builtin') ~= nil
if is_luajit and jit == nil then
    return "wait initialized"
end

local function dofile(filename)
    local load = _VERSION == "Lua 5.1" and loadstring or load
    local f = assert(io.open(filename))
    local str = f:read "*a"
    f:close()
    return assert(load(str, "=(debugger.lua)"))(filename)
end

-- `cfg` is the launcher's bootstrap snapshot (address/client/ansi/luaVersion/
-- platform), built natively from the injection data or LUA_DEBUG_CONFIG. It is
-- complete on its own: no version IPC file and no environment re-read here, so
-- every later VM joins the session with the same settings.
local session_cfg = cfg or {}
if session_cfg.address == nil or session_cfg.address == "" then
    session_cfg.address = ("@$tmp/luadbg_%s"):format(pid)
end

local dbg = dofile(path.."/script/debugger.lua")
-- mode=attach also enters through dbg:start: this is the first Lua VM in the
-- target, not a join into an already running session (that is dbg:attach).
dbg:start {
    address = session_cfg.address,
    client = session_cfg.client,
    ansi = session_cfg.ansi,
    luaVersion = session_cfg.luaVersion,
    platform = session_cfg.platform,
    -- Bound by the launcher from the symbol source it resolved. Passing it here
    -- is what keeps the script from re-entering the bridge through an exported
    -- target symbol that a stripped image does not have.
    nativeBridge = session_cfg.nativeBridge,
    -- Set when the launcher had a symbol source and could not apply it; the script
    -- must fail rather than fall back to the target's exported entry.
    nativeBridgeError = session_cfg.nativeBridgeError,
}
dbg:event "wait"
return "ok"
