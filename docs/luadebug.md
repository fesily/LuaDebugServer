# 为什么需要 luadebug

调试器对目标程序的影响要尽可能小，这决定了整体形态：调试器的运行时**不在**被调试
的 Lua VM 上执行。它是一套原生 C++ 实现（`src/debugger/` + `src/luadebug/`），
通过 bridge 直接读写目标 VM 的状态，因此：

* 调试目标不需要 `require` 任何 Lua 调试库，也不会有调试器字节码进入它的 VM；
* 断点、单步、求值与变量读取都走原生 hook 与 `proto_view` 行表，不依赖目标 VM 执行
  调试脚本来解析源码；
* 目标 VM 与调试器运行时的生命周期相互独立：目标退出前可以停止调试，调试器停止后
  目标继续运行。

## 一个 bridge 文件，多个 ABI 后端

同一个平台/架构只发布一个 `luadebug.so`（Windows 为 `luadebug.dll`），内部按 Lua
5.1/5.2/5.3/5.4/5.5/LuaJIT 的布局分后端编译。bridge 通过**模块身份**（调用它的那个
模块，而不是进程里第一个导出 `lua_*` 的模块）确定目标 VM 的实际 ABI，然后选择后端并
构造该模块私有的 API 表。

* 同一进程可以同时存在不同版本的 Lua VM；
* 一个调试会话只绑定一个 ABI 组：先加入的 VM 固定组别，其他组的 VM 正常运行但不参与
  该会话，显式接入会得到明确的组冲突错误，不会安装 hook 或创建 worker；
* 同组的多个 VM 可以同时调试：每个 VM 是一个 worker（`threads` 各占一行），断点按
  源文件分别保存，`continue` 只恢复“当下确实停住”的 worker——不会把别人的请求转成
  自己下一次停止的放行命令；
* `luaVersion` 只在启动随附解释器时选择可执行文件；对目标 VM 它是**约束**：显式值与
  检测到的版本冲突时拒绝接入（不安装 hook、不注册 worker），缺省时（`auto`）在目标里
  检测。同组多 VM 进程里，其他组的模块照常运行、只是不参与会话。

## 原有 Lua 侧 API

旧实现把调试逻辑写在目标 VM 里的 Lua 脚本中（`require "luadebug"` 得到的
`hookmgr`/`visitor`/`stdio` 等模块）。这些模块已经移除：`luadebug` 模块现在只剩
宿主侧的几个辅助函数（`setenv`、Windows 的 `a2u`），调试器本身完全在原生层。

## 无导出符号的目标：外部符号源

bridge 一直按**调用它的模块**识别目标 VM。这条路在两种目标上走不通：静态链接的解释器
（API 不在任何模块里）与剥掉符号的私有构建（没有可查的导出/符号表）。为此可以完全从
外部描述目标，三个字段彼此独立，任选其一即可：

|字段|含义|
|---|---|
|`luaBinary`|**已加载**镜像的完整路径或文件名，用来选定“去哪里找”。不会为了匹配去加载什么；同名的两个模块会列出完整路径并报错，不按枚举顺序挑一个。|
|`luaVersion`|ABI 约束。`auto`（缺省）表示让目标自己判断。|
|`luaSymbols`|符号名到地址规则的对象（内联，小表）。|
|`luaSymbolsFile`|同一对象写成目标可读的 UTF-8 JSON 文件；与内联形式**互斥**，大表走这里。|

每条规则只能三选一：

* `address`：十六进制字符串，**目标当前进程**里的绝对地址；进程重启（ASLR）后必须重新
  取得，不能复用上次的值。
* `offset`：十六进制非负 RVA，基准是 Gum 报告的**已加载模块基址**，不是文件偏移；必须
  配一个唯一的 `luaBinary`。越界、溢出、落在非可执行页都会失败。
* `pattern`：Gum 语法（如 `FD 7B ?? A9`），命中地址即函数入口。**必须唯一命中**：非法
  语法、零命中、多命中分别报错，绝不取第一个。

符号名是**封闭列表**（`src/luadebug/vm/symbol_names.h`）：只有 bridge 实际消费的 C API、
launcher 的内部 watch point，以及 `lua_ident` 这类版本证据。写错名字不会静默无效，而是
被拒绝——否则绑定会退回到“模块导出”，看起来像成功，实际错位。

## 版本判定

`lua_ident` 是精确证据（5.1 为 `$Lua: ...`，5.2+ 为 `$LuaVersion: Lua 5.x`；LuaJIT 不
定义它，靠 `luaJIT_setmode` 这类独有符号判定）。除此之外只用**正向标记**：

* 5.5 独有：`lua_numbertocstring`、`luaL_openselectedlibs`；
* 5.4+：`lua_resetthread`、`lua_newuserdatauv`；
* 5.3+：`lua_isinteger`；5.2+：`lua_rawlen`、`lua_callk`、`luaL_loadbufferx`。

**缺失符号不等于旧版本**：外部表可能不完整。因此只点名两种版本共有的 API 时，判定为
“不确定”并要求显式 `luaVersion`，而不是拿最像的那个去调用。只有镜像自己的导出/符号表
（本身完整）才把“没有 5.5 独有符号”当作信息。

显式版本在这里有两个作用，且顺序明确：证据**能**判定时它是约束（判定结果不同即拒绝）；
证据**不能**判定时（表只覆盖共有 API）它就是答案，launcher 直接采用并交给 binding 层
校验。`probe` 只验证所选 profile 的必需 API，不会依次尝试多个后端取第一个通过者，也不按
多种布局试读 `lua_State`。反过来也要清楚它的边界：符号存在
并不证明任意私有布局或编译选项都兼容——改过内部布局的 lua 仍需按
`docs/ported.md` 重编后端。

## 绑定身份与 ABI 2

`bridge_abi.h` 的描述符里只有 `state` 是必需的；`module`、`resolver`、符号表三者都可以
单独使用（表优先，缺名字才问 resolver，最后才是模块导出）。绑定成功后会**复制**需要的
函数指针，输入表、名字与 `resolver_user` 不必活得比调用更久。

绑定身份是 **profile + 完整解析出的函数地址集合**（逐字段比较，不 memcmp padding，也不
用临时表地址或空模块去重）：

* 两个同版本模块各有各的绑定，绝不互相顶替；
* 同一张表可以服务多个 state；
* 已经绑定过的 state 不允许换绑到另一套地址——hook 跳板与 worker 都按裸 state 指针查
  绑定，换绑只会让它们指向错误的 ABI。

哪些请求形态能携带符号源，是明确的（不是“尽量带上”）：

|形态|携带方式|
|---|---|
|注入（frida / preload / lldb）|随注入的 bootstrap 文档进入目标；launcher 解析后把表交给 bridge|
|`luaexe` 启动（console / `runInTerminal`）|adapter 自己起解释器，子进程环境里放 `LUA_DEBUG_CONFIG` 与 `LUA_DEBUG_VM_SOURCE=1`|
|按 `address` attach|**拒绝**：会话连接的 VM 已经在跑，无法重新绑定|
|`inject: "none"` + `runtimeExecutable`|**拒绝**：既没有注入，adapter 也不启动解释器，文档无处投递|
|`useWSL`|**拒绝**：符号源在目标里解析，而主机侧路径与环境变量都不随之进入 WSL|

`luadebug_bridge_open` 是同一条绑定路径的入口，额外把模块表压进目标栈：launcher 用它把
`cfg.nativeBridge` 交给 `debugger.lua`，脚本因此完全不必去找目标导出的
`luaopen_luadebug_<profile>`。错误只通过返回值和字符串返回，不跨 C ABI longjmp。
