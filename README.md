# LuaDebuggerX

Lua debugger base on frida/lua-debug.
This library will expand on lua-debug.

## What difference with lua-debug

1. lua-debug aims to compile with the lua virtual machine, because many projects have modified the virtual machine and cannot make a universal debugger. My goal is to be as compatible as possible with the standard VM.
2. The virtual machine will be modified to enable certain features not provided by lua, based on dynamic Instrumentation.

## MapRoad

- [ ] 5.1
- [ ] 5.4
- [ ] jit

## Future

- [ ] Support sethalt
- [ ] Support Coroutine
- [ ] Support Exception
- [ ] Adaptive Architecture debugger
- [ ] Debugger lua process (Don't mind if there is no export of lua symbols)
- [ ] CPU Profile
- [ ] Memory Profile
- [ ] Mixed Debugger C and lua stack like visual stdio

## Adaptive Architecture debugger

- [ ] pure lua
- [ ] pure lua api
- [ ] mixed lua api/source
- [ ] pure lua source
- [ ] lldb/gdb
