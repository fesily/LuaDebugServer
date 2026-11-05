# 移植指南

如果你打算使用自己的lua，而非调试器提供的，则需要注意以下几点。

## coroutine 与 exception 支持（native）

调试器运行时已经是原生实现：exception / thread 事件由 bridge 在目标 VM 上安装的 hook
直接产生，不再经过 Lua 侧的 `rdebug.event`（该入口保留为空操作，仅为兼容旧脚本）。
因此：

* 需要捕获错误的 Lua，必须能抛出 `LUA_HOOKEXCEPTION`（本仓库 `3rd/lua` 的补丁就是这样做的）；
* 需要调试 coroutine 的 Lua，必须能抛出 `LUA_HOOKTHREAD`；
* 下面基于 `rdebug.event` 的 Lua 侧变通方案已经失效，仅作为历史参考保留。

## coroutine支持（历史）

如果你需要调试coroutine内的代码，则需要让你的lua可以抛出THREAD的调试事件，具体可以参考调试器自带lua的实现(搜索THREAD)。

此外，你也可以在lua端自己抛这个事件而无需修改lua。例如
``` lua
local rdebug = require "luadebug"

local rawcoroutineresume = coroutine.resume
local rawcoroutinewrap   = coroutine.wrap
local rawcoroutineclose  = coroutine.close
local function coreturn(co, ...)
    rdebug.event("thread", co, 1)
    return ...
end
local function cocall(co, f, ...)
    --TODO: 处理f抛出错误的情况
    rdebug.event("thread", co, 0)
    return coreturn(co, f(...))
end
function coroutine.resume(co, ...)
    return cocall(co, rawcoroutineresume, co, ...)
end
function coroutine.wrap(f)
    --TODO: coroutine.wrap可以回调用coroutine.close,这里没法hook
    local wf = rawcoroutinewrap(f)
    local _, co = debug.getupvalue(wf, 1)
    return function(...)
        return cocall(co, wf, ...)
    end
end
function coroutine.close(co)
    return cocall(co, rawcoroutineclose, co)
end
```

## error支持

如果你需要捕获lua抛出的错误，则需要让你的lua可以抛出EXCEPTION的调试事件，具体可以参考调试器自带lua的实现(搜索EXCEPTION)。

此外，你也可以在lua端自己抛这个事件而无需修改lua。例如
``` lua
local rdebug = require "luadebug"

local rawxpcall = xpcall
function pcall(f, ...)
    return rawxpcall(f,
        function(msg)
            rdebug.event("exception", msg，2--[[LUA_ERRRUN]])
            return msg
        end,
    ...)
end
function xpcall(f, msgh, ...)
    return rawxpcall(f,
        function(msg)
            rdebug.event("exception", msg，22--[[LUA_ERRRUN]])
            return msgh and msgh(msg) or msg
        end
    , ...)
end
```

## 使用你的lua重新编译bridge

每个平台只有一个 `luadebug.so`，它按 profile 编译了多份布局后端（`src/luadebug/vm/backends/`）。
后端引用了非公开的 lua 头文件（`lstate.h`、`lobject.h` 等）；如果你的 lua 改过这些布局，
需要按你的版本重新编译对应后端，并让 profile 与之匹配。

绑定阶段会校验**已选 profile 的必需 API 是否存在**，并把符号集合固化成绑定身份，但
它**不能**证明任意私有布局或编译选项都兼容：符号齐备而内部结构被改动过的 lua 仍可能
读到错误的内存。真正的保证来自“用同一份源码重编后端”。

如果你的 lua 根本不导出（静态链接、strip、隐藏符号），不需要改后端：用
`luaBinary` / `luaSymbols` / `luaSymbolsFile` 从外部给出符号地址即可，bridge 可以在没有
模块句柄、没有导出符号的情况下完成绑定。三种地址语义（绝对地址、模块内 RVA、唯一
Gum pattern）与版本判定规则见 `docs/luadebug.md`。


## update优化

默认的情况下，调试器会在没有断点时加载一个count hook，以便调试器有时机可以响应GUI的请求（新的断点/暂停等）。但这也会影响到调试器没有断点时的运行效率，所以你可以将这个行为关闭，由lua在合适周期抛出update事件，从而将调试器对运行效率的影响降到最低(几乎可以忽略不记)。

update事件，只会影响调试运行过程中的执行GUI操作的响应时间（一般就是新的断点和暂停），所以一般不大于0.2秒就足够了。

关闭方法：使用autoUpdate事件。
