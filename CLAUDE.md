# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 构建系统

使用 CMake + Ninja 构建（要求 CMake ≥ 4.4 与 C++26 编译器）；扩展打包与测试编排只用
Python 3 标准库，构建期不依赖 Lua。

```bash
cmake --preset darwin-arm64-release      # 或 linux-x64-release / win32-x64-release
cmake --build --preset darwin-arm64-release
ctest --preset darwin-arm64-release      # 原生测试
```

- 依赖（Boost 1.86、fmt 12.1、Frida 17.25.2 devkit）在首次 configure 时下载并按固定
  SHA256 校验，缓存在 `3rd/_deps` 与 `3rd/frida_*`；`-DLUA_DEBUG_DEP_CACHE=<dir>` 指向
  已有缓存可离线构建。
- `cmake/ToolchainChecks.cmake` 强制 C++26 语言模式 + 协程（Asio/Process 协程探针），
  不满足时报 `C++26 coroutine toolchain required`。
- 自有 C++ 目标统一 C++26，C 源保持 C11；平台层是 Boost.Asio / Boost.Process v2 /
  Boost.Filesystem / Boost.Unordered 与 fmt，不再使用 bee。
- 产物装配到 `publish/`；`cmake/Companion.cmake` 负责多架构发行里的第二个架构。

## 架构

这是一个 VS Code 调试器扩展，支持调试 Lua 5.1–5.5 和 LuaJIT。扩展入口是 `extension/js/extension.js`。

### 会话模型（全部原生）

```
VS Code (JS) ──DAP──> Adapter 进程                        目标进程
                      src/debugger/frontend/               luadebug.so（bridge）
                          │  ──socket(DAP framing)──>       ├── src/debugger/session/  (DAP master + transport)
                          │                                └── src/luadebug/          (hook / worker session)
                          └── 共享 libluadebug_runtime（会话 hub、master、id 编码）
```

- **Adapter (`src/debugger/frontend/`)**：`publish/bin/lua-debug` 是原生可执行文件（C++17，不链接 Lua），负责 launch/attach 编排、进程管理、注入、WSL 路径转换，并把 DAP 帧代理到目标进程里的会话。
- **Runtime (`src/debugger/runtime/`, `session/`)**：每个平台/架构只有一份 `libluadebug_runtime`。它持有 `SessionHub`（worker 注册表、命令队列）与 `Master`（DAP 路由、id 编码、config 缓存、断点重放、线程注册表、stdout/stderr 捕获）。传输用与 frontend 相同的 `transport::Channel`（Content-Length + JSON）。
- **Bridge (`src/luadebug/`)**：每个 Lua 版本一个 `luadebug.so`，被加载进目标进程。`luadebug_bridge_start`（`bridge/bridge_entry.cpp`）把**目标** `lua_State*` 绑给 `worker::WorkerSession`，注册到 hub，并在 `startup` 里阻塞到 Master 完成配置，避免启动期断点丢失。
- **`worker::WorkerSession`**（`src/luadebug/worker/`）：断点、单步、调用栈、变量、表达式求值、内存读写、反汇编、指令断点、输出捕获。所有命令在目标线程上执行；hook 事件经 `hook::Sink` 直接回调，不再经过第二个 Lua VM。
- **`hook::Core`**（`src/luadebug/hook/`）：hook mask 选择、按 Proto 启停断点、单步、异常/线程 hook。旧版由 Lua 回调驱动的部分已移除。

### 目标侧原生模块 (`src/luadebug/`)

编译为动态库（`luadebug.so`/`luadebug.dll`），加载进目标进程：
- `bridge/` — `luadebug_bridge_start/pump/stop`，加载并注册 runtime，安装 hook，泵命令
- `worker/worker_session.cpp` — 命令实现与停止循环
- `hook/` — hook 核心与 sink 接口
- `target/` — 跨版本目标操作（值存储、求值器、proto 行表、反汇编）
- `compat/` — Lua 版本抽象层（5.x 通过 `5x/`，LuaJIT 通过 `jit/`），按版本分别编译
- `thunk/` — 各架构的 hook 跳板（x86、x64、ARM64；Windows/macOS/Linux）
- `util/refvalue.h` — 引用值系统（在目标 VM 中惰性定位值）
- `rdebug_module.cpp` — 脚本可见的 `luadebug` 模块（`setenv`、Windows `a2u`、兼容用 `event`）

`debugger.lua` 只负责加载该动态库并调用 `luadebug_bridge_start`；调试器本身不再拥有第二个 Lua 解释器。

### 启动器 (`src/launcher/`)

一个小型可执行文件，用于启动目标 Lua 进程或附加到运行中的进程，然后注入调试器 DLL。

### 扩展 Lua 脚本 (`extension/script/`)

| 文件 | 用途 |
|---|---|
| `debugger.lua` | 入口：定位并加载 `luadebug.so`，调用 `luadebug_bridge_start` |
| `launch.lua` / `attach.lua` | 注入目标进程的启动脚本；`attach.lua` 收到 launcher 传来的会话配置 table（版本 IPC 文件已删除） |

## 关键文件

- `CMakeLists.txt` + `CMakePresets.json` — 根构建入口（configure / build / test 由 preset 驱动）
- `cmake/` — 构建模块：`LuaTargets.cmake`（7 个解释器）、`LuaJIT.cmake`（vendored `cmake/luajit`）、`Bridges.cmake`（每版本 bridge）、`Runtime.cmake`、`Adapter.cmake`、`Launcher.cmake`、`Frida.cmake`、`Dependencies.cmake`/`FetchDep.cmake`（Boost/fmt/devkit 下载与校验）、`Companion.cmake`（第二架构）、`Tests.cmake`、`Packaging.cmake`
- `tools/` — 打包与格式化脚本（Python 标准库）：`gen_package_json.py`、`assemble_extension.py`、`install_extension.py`、`format.py`
- `extension/package.json` — VS Code 扩展清单
- `src/debugger/` — 原生 debugger 代码：`common/`（JSON、DAP framing、日志、capabilities）、`transport/`（Boost.Asio 协程 reactor、socket/stdio 通道）、`frontend/`（adapter 主程序）、`runtime/`（跨平台 runtime ABI + hub）、`session/`（DAP master、id 编码、断点重放）
- `test/e2e/` — 端到端 DAP 测试（Python 标准库）：`run_e2e.py`、`dap_client.py`、`fixtures/`
- `test/dap-node/` — 用微软 `@vscode/debugadapter-testsupport`（`DebugClient`）驱动原生 adapter 的 DAP 集成测试，`npm test` 运行
- `extension/script/debugger.lua` — 调试器入口，由目标进程通过 `dofile` 加载
- `extension/script/launch.lua` / `attach.lua` — 注入目标进程的启动脚本
- `src/injector/` — frida-core 注入层：`frida_inject.cpp`（注入 + `uninjected` barrier）与 macOS 客户端 `main.cpp`（按目标架构选 arm64/x64 客户端，权限失败时走 osascript）
- `src/debugger/common/bootstrap.h/.cpp` — 前端与 launcher 共用的 bootstrap JSON 契约（`inject` data 与 `LUA_DEBUG_CONFIG` 两种载体）
- `src/luadebug/bridge/session_boot.cpp` — bridge 侧会话引导（加载 runtime、注册、握手、泵）
- `src/debugger/session/master.cpp` — DAP master：请求路由、引用 id 编码、config 缓存与断点重放
- `src/debugger/common/protocol.cpp` — DAP 线协议（Content-Length 头 + JSON）
- `cmake/ToolchainChecks.cmake` — 语言标准与工具链门禁（C11 / C++26）
- `assets/windows/` — Windows 资源脚本与图标

## 代码风格

- Lua：4 空格缩进，数学运算符两侧加空格，函数调用括号前不加空格，保留尾部表分隔符
- C/C++：clang-format，配置文件在 `.clang-format` 和 `.clang-format-ignore`
- `.editorconfig` 中有完整的 Lua 格式化规则

## Git 子模块

本仓库使用 git submodule。克隆或切换分支后若检测到子模块变更，请始终运行 `git submodule update --init`。
