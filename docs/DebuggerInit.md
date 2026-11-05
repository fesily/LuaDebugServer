### 初始化流程
简单概述几种加载luadebug调试器的方式

前端vscode的参数会被完整传递到前端程序 lua-debug 代理进程中。该进程本身不链接
Lua：它按 carrier 把 launcher 送进目标进程，由 launcher/bridge 在目标 VM 上建立会话。

参数传递分为两个阶段,初始化阶段目前只传递了部分参数, 第二阶段调试器加载后DAP协议的initialized消息中传递了所有参数

## Launch
1. 应用程序支持标准lua支持的-e参数
2. 注入器

## Attach
1. 主动连接调试器
2. 注入器

### 主动连接 
在源代码里主动dofile debugger.lua

## Details

### '-e'

通过`DBG 'address[/ansi]/luaversion'` 在字符串中传递启动参数

这条通道只传 address、utf8（`ansi` 标记）与 luaversion 三项。它需要目标能执行
`dofile`，适合“解释器自己启动我们的脚本”的形态。

需要外部符号源（见下）时，脚本启动形态改走环境变量：adapter 在子进程环境里放
`LUA_DEBUG_CONFIG`（bootstrap 文档）与 `LUA_DEBUG_VM_SOURCE=1`，`debugger.lua` 只在该
标记存在时改用 launcher 的 Lua 入口，argv/cwd/终端选择都不变。`LUA_DEBUG_CONFIG` 由
adapter 自己拥有，用户 `env` 里的同名值会被覆盖，其他键原样保留。

### Injector

注入由 `src/injector/`（frida-core）完成，launcher 的入口是
`void luadebug_frida_entry(const char* data, int32_t* unload_policy, void*)`。

`data` 是一个 UTF-8 JSON 对象（bootstrap 契约，见
`src/debugger/common/bootstrap.h`），字段为
`mode` / `address` / `client` / `luaVersion` / `ansi` / `platform` / `root`，以及可选的
VM 源：`luaBinary` / `luaSymbols` / `luaSymbolsFile`（语义见 `docs/luadebug.md`）。
可选字段的取值上限仍是 4095 字节，所以内联大表由 adapter 先写成会话临时文件，再以
`luaSymbolsFile` 传递；adapter 只清理自己创建的文件。
它与目标进程的 `LUA_DEBUG_CONFIG` 环境变量使用同一份文档：非空 `data` 整份优先，
否则读环境变量；两者都为空时使用默认值（attach、pid socket、自动识别 Lua 版本）。

旧的 `ipc_<pid>_luaVersion` 文件已删除：配置随 `data`/环境进入 launcher，再以 Lua
table 传给 `attach.lua`（`path, pid, cfg`），`debugger.lua` 用它构造 bridge 的
session config。

符号源只随“能把它送到目标”的载体走：注入类请求走 payload，`luaexe` 启动走子进程环境，
而按 `address` attach、`inject: "none"` 的 `runtimeExecutable` 启动、以及 `useWSL` 会被
明确拒绝（见 `docs/luadebug.md` 的形态表），不会静默忽略。

`luaVersion` 不再选择动态库目录：每个平台只有一个 `runtime/<platform>/luadebug.so`，
它按 `luaopen_luadebug_<profile>` 入口与调用者所属模块绑定。`auto`（缺省）表示在目标里
检测；显式值是约束，与检测结果冲突时拒绝接入。

会话固定在一个 ABI 组：launcher 用 `luaVersion`（或首个成功接入的模块）确定组别，只向
该组注入；其他组的模块照常运行、不参与会话。显式版本在进程里找不到匹配的组时，整个
进程拒绝接入并报告，不会退回自动猜测。

### 目标里的两条入口

|形态|入口|说明|
|---|---|---|
|注入（frida / preload / lldb）|`attach.lua` → `debugger.lua`|launcher 已按解析出的符号表打开 bridge，把模块表作为 `cfg.nativeBridge` 传下去；脚本核对 `profile()` 后直接使用。|
|解释器自启（`luaexe`，console/terminal）|`luaopen_luadebug_launcher_<profile>`|launcher 读环境里的 bootstrap 文档，解析符号源并调用 `luadebug_bridge_open`；不装 watchdog、不跑 `attach.lua`，因此不会递归启动。|

没有 `nativeBridge`、也没有 `LUA_DEBUG_VM_SOURCE` 时，脚本仍走原来的
`package.loadlib(luadebug.so, "luaopen_luadebug_<profile>")`：该入口自己捕获调用者模块。

### 加载时机

- **launch**：目标以 `DYLD_INSERT_LIBRARIES` / `LD_PRELOAD` 预加载 launcher，
  constructor 在第一个 Lua state 出现之前完成 hook（Windows 用 frida 的
  CREATE_SUSPENDED spawn + 注入 + resume）。
- **attach**：frida 注入 launcher，等 `uninjected` 后再接入 DAP；`uninjected`
  只代表入口返回，不代表握手完成。