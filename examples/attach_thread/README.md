* Build the extension first (`cmake --build --preset <platform>-release`), which
  produces the bundled interpreters under `publish/runtime/<platform>/`.
* Open VSCode, and press `F5`.

The example's `luaexe` points at the bundled LuaJIT interpreter of the
`darwin-arm64` runtime tree; change the platform directory when running on
another host.
