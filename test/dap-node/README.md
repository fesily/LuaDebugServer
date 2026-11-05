# DAP integration tests (Node)

Drives `publish/bin/lua-debug` through Microsoft's
[`@vscode/debugadapter-testsupport`](https://github.com/microsoft/vscode-debugadapter-node)
(`DebugClient`), i.e. the same client vscode-mock-debug uses. It speaks real DAP
over the adapter's stdio, so it exercises exactly what VS Code does: initialize
capabilities, launch, deferred breakpoint verification, stepping, evaluate,
setVariable, conditional breakpoints, entry stop.

```sh
cd test/dap-node
npm install
npm test                     # all versions
LUA_DEBUG_VERSIONS=lua54 npm test
LUA_DEBUG_RUNTIME_ROOT=/path/to/runtime/<platform> npm test
LUA_DEBUG_SHOW_ADAPTER_STDERR=1 npm test
```

Prerequisites: a release build (`cmake --build --preset <platform>-release`) and
Node.js.

Relationship to `test/e2e`: that harness (Python, stdlib only) can also drive the
backend socket directly and instrument timings; this one keeps us honest against
the official client, including optional DAP fields (`arguments` on `disconnect`,
`context` on `evaluate`) that a hand-written client might always send.

Known gaps in the current (Lua) backend, asserted but expected to pass only after
the C++ cutover: logpoints emit no output (`test/e2e --suite full`).
