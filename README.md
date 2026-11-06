# lua-debug

[![Build Status](https://github.com/actboy168/lua-debug/workflows/build/badge.svg)](https://github.com/actboy168/lua-debug/actions?workflow=build)

## Acknowledgements

This project is based on [actboy168/lua-debug](https://github.com/actboy168/lua-debug).
Thanks to [@actboy168](https://github.com/actboy168) and all upstream contributors
for their work.

## Requirements

* Lua 5.1 - 5.5 or luajit
* Platform: Windows, macOS, Linux, Android, NetBSD, FreeBSD
* macOS binaries: 11.0+ on Apple Silicon, 10.15+ on Intel (`std::filesystem`,
  which the path handling uses, starts there)

## Feature

* Breakpoints
* Function breakpoints
* Conditional breakpoints
* Hit Conditional breakpoints
* Step over, step in, step out
* Watches
* Evaluate expressions
* Exception
* Remote debugging
* Support WSL

## Injection

The debugger gets into a target process in exactly two ways, selected by
`inject`:

| value | behaviour |
|---|---|
| `auto` (default) | Pick a backend: `frida` when the adapter and the target share an architecture, `lldb` when they do not. |
| `frida` | Load the launcher into the target with [Frida](https://frida.re) (attach) or through the dynamic loader (launch). |
| `lldb` | Load the launcher with a batch `lldb` session (`process load` + the entry call). macOS only. |
| `none` | Never inject. Use it with an explicit `address`, or when the host already loads the debugger itself. |

`LUA_DEBUG_INJECTOR=frida|lldb` overrides the choice for a run (the e2e runner
exposes it as `--injector`). The old `gdb` and `hook` strategies and the
`inject_executable` setting are gone; a configuration that still uses them is
rejected with a message that names the remaining values.

### Injector helper and backends

The adapter drives

    adapter ── helper (optional) ── launcher (optional) ── debugger

and only the *helper* is per-architecture:

* `bin/lua-debug-injector-<arch>` — this project's helper (frida backend). It
  hosts frida-core, so it stays a separate process per architecture; the adapter
  runs the one matching the target.
* `lldb` — the system helper for the lldb backend, with the same rule (it needs
  the target architecture's shared cache). `LUA_DEBUG_LLDB` points elsewhere when
  the build machine keeps lldb outside `/usr/bin`.
* none — Linux and Windows link frida-core into the adapter.

The launcher is one fat `bin/launcher.so` on macOS (dyld picks the target's
slice) and the debugger is one bridge per platform/arch, so neither is duplicated
per architecture.

Both backends end in the same place: the launcher mapped in the target and its
entry point called with the bootstrap document, after which the adapter waits for
the target to connect.

* **frida** speaks frida-core; the default whenever architectures match, and the
  only carrier for a *launch* on macOS/Linux (the launcher travels through the
  dynamic loader).
* **lldb** drives `lldb --batch`: for an attach, `-p <pid>` + `process load
  <launcher>` + an `expression` calling the entry point + `detach`; for a launch,
  `process launch --stop-at-entry` + the same load/entry + `detach`, which lets
  the launcher in *before* the interpreter creates a state. It exists for the
  cross-architecture case — an arm64 adapter and a Rosetta x86_64 target — where
  frida's agent cannot create a thread in the target
  (`thread_create returned invalid argument`). The session keeps running after a
  failed command so its `detach` always executes, and an attach that lands while
  the target is still inside dyld initialization is retried.

### What `inject: frida` does per platform

* **Attach to a running process** (`processId` / `processName`): the adapter asks
  Frida to inject `bin/launcher.so` (Windows: `launcher.x86.dll` /
  `launcher.x64.dll`, chosen by the *target's* architecture) and waits until
  Frida reports that this injection's entry point finished.
  * macOS injects through a per-architecture helper
    (`bin/lua-debug-injector-arm64`, `-x64`): an arm64 process cannot resolve
    libSystem inside a Rosetta x86_64 target, so the helper must match the
    target's architecture, and the adapter starts the Intel one through
    `/usr/bin/arch -x86_64`. A refused `task_for_pid` is retried once through
    `osascript ... with administrator privileges`, and only when the helper
    reported `permission_denied` itself.
* **Launch** (`runtimeExecutable`): the target is started with the launcher in
  `DYLD_INSERT_LIBRARIES` (macOS) / `LD_PRELOAD` (Linux), so the hook is installed
  before the first Lua state exists. Windows has no equivalent, so it uses
  Frida's gated spawn (`CREATE_SUSPENDED`), injects, and resumes.
  Both loader variables split their value on spaces and `:`, so when the published
  tree lives under such a path the launcher is preloaded through a symlink in the
  temp directory; the bootstrap document still carries the real `root`.
  There is no injection barrier on this path: the adapter waits (bounded) for the
  backend to actually *connect*, and fails the launch — killing the child — when
  it never does.

### Bootstrap document

The launcher receives one UTF-8 JSON object, from exactly one carrier:

1. the injector `data` argument, when non-empty (normal attach and Windows
   launch), otherwise
2. the target's own `LUA_DEBUG_CONFIG` environment variable.

Fields (all optional; `{}` is valid):

| field | meaning |
|---|---|
| `mode` | `launch` or `attach`; omitted means `attach` |
| `address` | session address; omitted means `@$tmp/luadbg_<target pid>` |
| `client` | from the *target's* point of view: `false` listen (default), `true` connect |
| `luaVersion` | the VM profile (`lua51`…`lua55`, `luajit`, `lua-latest`); the launcher uses it to pick the candidate group, the bridge cross-checks it against the module's real ABI. Omitted means "detect from the target VM" |
| `ansi` | Windows legacy encoding for paths |
| `platform` | published platform directory (`darwin-x64`, `win32-x64`, …); omitted means "detect from the target" |
| `root` | the published tree; needed because a Frida-mapped image has no dyld path, so `dladdr` cannot locate it |

Unknown fields are ignored; a wrong type, an unknown `mode`, an empty string or a
decoded NUL is rejected, and a rejected document installs no hook and does not
fall back to the default socket.

Environment-only injection is meant for hosts that inject on their own:

```sh
LUA_DEBUG_PRELOAD=1 \
LUA_DEBUG_CONFIG='{"mode":"launch","address":"@$tmp/luadbg_1"}' \
DYLD_INSERT_LIBRARIES=$PWD/publish/bin/launcher.so \
  ./lua myscript.lua
```

`LUA_DEBUG_PRELOAD` is required so that a process which merely happens to have
`LUA_DEBUG_CONFIG` in its environment is never injected silently. A normal
frontend always sends a non-empty `data`, so `arguments.env.LUA_DEBUG_CONFIG`
does not override it.

### Limits

* **macOS**: injecting into another process needs `task_for_pid` permission,
  which the system grants for debuggable targets
  (`com.apple.security.get-task-allow`) or to an administrator; hence the
  authorization prompt. Signing a target is what building for debugging does.
* **macOS + Rosetta**: attaching goes through the `lldb` backend automatically
  (frida cannot create a thread in a translated target). A translated process
  must not be attached to *while it is still starting*: the kernel raises
  `SIGSYS` inside dyld's own cache probe and the target dies. Measured on this
  machine: attach 50 ms after spawn -> 10/10 targets died; attach after 800 ms ->
  0/10. The adapter therefore waits out a 1.2 s grace period when the target
  started that recently (`process_age_seconds` in
  `src/debugger/frontend/inject.cpp`), and only then attaches. Launching works
  through either carrier: the loader one (the debuggee links libc++, which the
  launcher needs) or `inject: lldb`, which spawns the target from lldb instead.
* Windows/Linux builds link the Frida core devkit. Linux arm64 has been exercised
  end to end on the current LTS (`linux-arm64-release`, Ubuntu 26.04 LTS as a
  Docker guest on Apple Silicon, LLVM Clang 21, CMake 4.4.5 from upstream because
  the distro ships 4.2.3): 46/46 native tests pass. The "module loads later"
  watcher (`wait_module`) used to be the one platform gap; it is now the Frida
  module registry, whose loader notifications exist on every platform the launcher
  targets, so the case is no longer skipped on either platform where the native
  suite is built (the suite's hosts are POSIX-only). The Windows devkit is
  compiled against the static release CRT, so the adapter and injector objects are
  built with `/MT` even in debug (`cmake/Dependencies.cmake`); a Windows debug
  build that mixes CRT variants fails at link time rather than silently
  mismatching.
* ELF needs two things the Mach-O tree gets for free, both handled in CMake: strict
  `-std=c11` hides the POSIX surface glibc provides (`cmake/ToolchainChecks.cmake`
  and `cmake/LuaJIT.cmake` define `_DEFAULT_SOURCE` for C sources), and an embedded
  Lua host must export its symbols (`ENABLE_EXPORTS`, i.e. `--export-dynamic`) for
  the bridge's `dlsym` on the executable. `vm::module_by_address` also has to use
  `dlopen(NULL)` for the main program, since glibc refuses to `dlopen` a PIE
  executable; every hit is containment-checked so the search still stays inside
  that one image.

## Binary size

The adapter and the injector helper link the Frida devkits, whose static archives
carry every embedded resource Frida can need (agent dylibs and helper binaries for
each OS, icons, JS support scripts - about 80 MB of `__const` alone). Both are
linked with `-Wl,-dead_strip` and stripped of local symbols (`cmake/ToolchainChecks.cmake`'s
`lua_debug_apply_link_trim`), which keeps only what the injector actually reaches:

| artifact | before | after |
|---|---|---|
| `bin/lua-debug-injector-arm64` | 99.5 MB | 16.2 MB |
| `bin/lua-debug-injector-x64` | 66 MB | 13.4 MB |
| `bin/lua-debug` | 864 KB | 524 KB |

Interpreters, `launcher.so` and the bridges are deliberately *not* trimmed: they
resolve their own symbols at runtime (`lua_*` from the host, the module entry
points), so dead-stripping them silently breaks injection.

## Runtime layout

`publish/runtime/<platform>/` holds **one** `luadebug.so` (`luadebug.dll` on
Windows), one `libluadebug_runtime` and one directory per bundled interpreter
(`lua51` … `lua55`, `luajit`, `lua-latest`) that contains only that interpreter.

The single bridge supports every Lua version the extension does. It identifies the
target VM from the module that calls into it - never from a process-wide symbol
search - and picks the matching ABI backend from there. Consequences:

* a process may host VMs of *different* versions at the same time;
* one debugging session is bound to a single ABI group (one version profile). The
  first VM that joins fixes the group; a VM of another group keeps running
  normally but cannot join that session, and the refusal is reported instead of
  installing hooks or a worker;
* `luaVersion` selects which bundled interpreter to launch and, for the launcher,
  which candidate group to inject. It never reinterprets a real VM as another
  version: the module's own ABI evidence wins.
* automatic injection follows the same rule: only modules of the selected group
  are hooked, a group that loads later never extends a running session, and a
  process with several groups and no `luaVersion` is reported as ambiguous
  instead of being hooked by guesswork;
* several VMs of that group can be debugged at once (a service-per-state host):
  each is its own worker, `threads` lists them, breakpoints are kept per source
  file, and `continue` resumes exactly the workers that are stopped - a worker
  that has not stopped yet is never resumed by someone else's request.

## Build

Prerequisites: CMake >= 4.4, Ninja, Python 3 (packaging steps only), a C++26
compiler with coroutines, and git submodules. Node/npm are needed only to package
the extension and to run the official DAP client tests.

``` bash
git clone --recursive https://github.com/actboy168/lua-debug
cd lua-debug
cmake --preset darwin-arm64-release   # or linux-x64-release, win32-x64-release, ...
cmake --build --preset darwin-arm64-release
ctest --preset darwin-arm64-release   # native tests (LUA_DEBUG_BUILD_TESTS defaults ON in the presets)
```

The build downloads and verifies the pinned dependencies (Boost 1.86, {fmt}
12.1, the Frida 17.25.2 devkits) into `3rd/_deps` and `3rd/frida_*` on first
configure; point `-DLUA_DEBUG_DEP_CACHE=<dir>` at a populated cache to build
offline.

Toolchain requirements are enforced at configure time (`cmake/ToolchainChecks.cmake`):
the C++26 language mode plus coroutine support must be available, otherwise the
configure step stops with `C++26 coroutine toolchain required`. macOS uses
AppleClang >= 17, Linux uses LLVM Clang 21 (`clang-21`), Windows uses LLVM
clang-cl 21 inside an MSVC developer environment.

Everything lands in `publish/`: the adapter (`bin/lua-debug`), the per-version
interpreters and bridges (`runtime/<platform>/<version>/`), the shared runtime
library, the launcher and the macOS inject clients.

## Install to VSCode

1. Install extension `fesily.lua-debug-server`
2. Open [repo](https://github.com/actboy168/lua-debug) in VSCode
3. Run task：Copy Publish

## Todo

* iOS example.
