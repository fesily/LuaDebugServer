local selfsource = ...

if not selfsource then
    local source = debug.getinfo(1, "S").source
    if source:sub(1, 1) == "@" then
        local filepath = source:sub(2)
        selfsource = filepath
    end
end

local IsWindows = package.config:sub(1, 1) == "\\"
local root = selfsource
    :match "(.+)[/][^/]+$"
    :match "(.+)[/][^/]+$"

if debug.getregistry()["lua-debug-server"] then
    local dbg = debug.getregistry()["lua-debug-server"]
    local empty = { root = dbg.root }
    function empty:init()
        return self
    end

    function empty:start()
        return self
    end

    function empty:attach()
        return self
    end

    function empty:event(what, ...)
        if what == "setThreadName" then
            dbg:event(what, ...)
        end
        return self
    end

    function empty:set_wait()
        return self
    end

    return empty
end

local function detectLuaDebugPath(cfg)
    local PLATFORM = cfg.platform or os.getenv "LUA_DEBUG_PLATFORM"
    if not PLATFORM then
        local function shell(command)
            --NOTICE: io.popen可能会多线程不安全
            local f = assert(io.popen(command, 'r'))
            local r = f:read '*l'
            f:close()
            return r:lower()
        end
        local function detect_windows()
            if os.getenv "PROCESSOR_ARCHITECTURE" == "AMD64" then
                PLATFORM = "win32-x64"
            else
                PLATFORM = "win32-ia32"
            end
        end
        local function detect_linux()
            local machine = shell "uname -m"
            if machine == "x86_64" or machine == "amd64" then
                PLATFORM = "linux-x64"
            elseif machine == "aarch64" then
                PLATFORM = "linux-arm64"
            else
                error "unknown ARCH"
            end
        end
        local function detect_android()
            PLATFORM = "linux-arm64"
        end
        local function detect_macos()
            if shell "uname -m" == "arm64" then
                PLATFORM = "darwin-arm64"
            else
                PLATFORM = "darwin-x64"
            end
        end
        local function detect_bsd()
            local machine = shell "uname -m"
            if machine == "x86_64" or machine == "amd64" then
                PLATFORM = "bsd-x64"
            else
                error "unknown ARCH"
            end
        end
        if IsWindows then
            detect_windows()
        else
            local name = shell 'uname -s'
            if name == "linux" then
                if shell 'uname -o' == 'android' then
                    detect_android()
                else
                    detect_linux()
                end
            elseif name == "darwin" then
                detect_macos()
            elseif name == "netbsd" or name == "freebsd" then
                detect_bsd()
            else
                error "unknown OS"
            end
        end
    end

    -- One bridge per platform/arch serves every supported Lua: the profile is
    -- chosen by the running VM, not by a version subdirectory.
    return root.."/runtime/"..PLATFORM.."/luadebug."..(IsWindows and "dll" or "so")
end

-- The profile the running VM belongs to. `cfg.luaVersion` only cross-checks it:
-- a configuration may not reinterpret a real VM as another version.
local function detectProfile(cfg)
    local running
    if jit and jit.version then
        running = "luajit"
        jit.off()
    elseif _VERSION == "Lua 5.1" then
        running = "lua51"
    elseif _VERSION == "Lua 5.2" then
        running = "lua52"
    elseif _VERSION == "Lua 5.3" then
        running = "lua53"
    elseif _VERSION == "Lua 5.4" then
        running = "lua54"
    elseif _VERSION == "Lua 5.5" then
        running = "lua55"
    else
        error(_VERSION.." is not supported.")
    end
    if cfg.luaVersion and cfg.luaVersion ~= "auto" then
        local configured = cfg.luaVersion
        if configured == "lua-latest" then
            configured = "lua55"
        end
        if configured ~= running then
            error(("luaVersion %q does not match the running VM (%s)"):format(cfg.luaVersion, running))
        end
    end
    return running
end

-- The launcher, published next to the bridge. It is the only entry that can bind
-- a VM from an externally supplied symbol table, so the launch shapes that cannot
-- inject it (a plain `luaexe`) load it as a plain Lua C module instead.
local function detectLauncherPath()
    if IsWindows then
        -- One file per target architecture, matching the interpreter's width.
        local arch = (os.getenv "PROCESSOR_ARCHITECTURE" or ""):lower()
        if arch == "amd64" then
            return root.."/bin/launcher.x64.dll"
        elseif arch == "arm64" then
            return root.."/bin/launcher.arm64.dll"
        elseif arch == "x86" then
            return root.."/bin/launcher.x86.dll"
        end
        return root.."/bin/launcher.x64.dll"
    end
    return root.."/bin/launcher.so"
end

-- Opens the bridge through the launcher with the symbol source the environment
-- carries. Returns the module table, or raises the launcher's message.
local function openFromSource(profile)
    local launcher = detectLauncherPath()
    local entry, loadlib_error = package.loadlib(launcher, "luaopen_luadebug_launcher_"..profile)
    if not entry then
        error(("lua-debug: cannot load %s (entry luaopen_luadebug_launcher_%s): %s")
            :format(launcher, profile, tostring(loadlib_error)))
    end
    local result = entry()
    if type(result) ~= "table" then
        error(tostring(result or
            "lua-debug: the launcher refused the supplied VM source (see the target's stderr)"))
    end
    if type(result.profile) ~= "function" or result.profile() ~= profile then
        error(("lua-debug: the launcher bound %s but the running VM is %s")
            :format(tostring(result.profile and result.profile()), profile))
    end
    return result
end

local function initDebugger(dbg, cfg)
    if type(cfg) == "string" then
        cfg = { address = cfg }
    end

    local luadebug = os.getenv "LUA_DEBUG_CORE"
    local updateenv = false
    if not luadebug then
        luadebug = detectLuaDebugPath(cfg)
        updateenv = true
    end

    local profile = detectProfile(cfg)

    -- Three ways in, in order of preference:
    --   * a table the launcher already bound (the injected paths),
    --   * the launcher's own entry, when the environment carries a symbol source
    --     (a plain `luaexe` launch, where injection is not available),
    --   * the bridge's own entry, which identifies the caller's module itself.
    if cfg.nativeBridgeError and cfg.nativeBridgeError ~= "" then
        error(cfg.nativeBridgeError)
    end

    local bridge = cfg.nativeBridge
    if bridge ~= nil then
        if type(bridge) ~= "table" or type(bridge.profile) ~= "function" then
            error "lua-debug: cfg.nativeBridge is not a bridge module table"
        end
        local actual = bridge.profile()
        if actual ~= profile then
            error(("lua-debug: the launcher bound %s but the running VM is %s")
                :format(tostring(actual), profile))
        end
    elseif os.getenv "LUA_DEBUG_VM_SOURCE" == "1" then
        bridge = openFromSource(profile)
    else
        -- Same library, profile-specific entry: `luaopen_luadebug_lua54` and
        -- friends capture the caller's module themselves, so no process-wide
        -- symbol search can bind the wrong Lua.
        bridge = assert(package.loadlib(luadebug, "luaopen_luadebug_"..profile))()
    end

    ---@type LuaDebug
    dbg.rdebug = bridge
    dbg.core = luadebug
    dbg.profile = profile
    if not os.getenv "LUA_DEBUG_PATH" then
        bridge.setenv("LUA_DEBUG_PATH", selfsource)
    end
    if updateenv then
        bridge.setenv("LUA_DEBUG_CORE", luadebug)
    end

    local function utf8(s)
        if cfg.ansi and IsWindows and bridge.a2u then
            return bridge.a2u(s)
        end
        return s
    end
    dbg.root = utf8(root)
    dbg.address = cfg.address and utf8(cfg.address) or nil
end

local dbg = {}

-- Minimal JSON encoding for the session config: the debuggee may not have a JSON
-- module on its package.path, and the payload is a flat table of strings/bools.
local function json_escape(text)
    return (tostring(text):gsub('[%c"\\]', function (c)
        if c == '"' then return '\\"' end
        if c == '\\' then return '\\\\' end
        if c == '\n' then return '\\n' end
        if c == '\r' then return '\\r' end
        if c == '\t' then return '\\t' end
        return ('\\u%04x'):format(c:byte())
    end))
end

local function session_config(root, cfg)
    local parts = {}
    local function str(name, value)
        if value ~= nil then
            parts[#parts + 1] = ('"%s":"%s"'):format(name, json_escape(value))
        end
    end
    local function bool(name, value)
        if value then
            parts[#parts + 1] = ('"%s":true'):format(name)
        end
    end
    str('address', cfg.address)
    bool('client', cfg.client)
    str('luaVersion', cfg.luaVersion)
    bool('ansi', cfg.ansi)
    str('root', root)
    -- No `table.concat` here: the first hook can fire while `luaL_openlibs` is
    -- still running (Lua 5.5 opens `table` later than `string`), and the session
    -- config must be buildable from whatever the VM already has.
    local out = '{'
    for i = 1, #parts do
        if i > 1 then
            out = out..','
        end
        out = out..parts[i]
    end
    return out..'}'
end

function dbg:start(cfg)
    initDebugger(self, cfg)

    -- The session (address, client mode) is created by the runtime inside this
    -- process; the adapter connects to it exactly as it did to the Lua master.
    assert(self.rdebug.start(session_config(self.root, cfg), false),
        "lua-debug: cannot start the native session")
    return self
end

function dbg:attach(cfg)
    initDebugger(self, cfg or {})

    -- Attach joins the session the adapter already started.
    assert(self.rdebug.start(session_config(self.root, cfg or {}), true),
        "lua-debug: cannot join the native session")
    return self
end

function dbg:stop()
    self.rdebug.stop()
end

function dbg:event(...)
    self.rdebug.event(...)
    return self
end

function dbg:set_wait(name, f)
    _G[name] = function (...)
        _G[name] = nil
        f(...)
        self:event 'wait'
    end
    return self
end


debug.getregistry()["lua-debug-server"] = dbg

return dbg
