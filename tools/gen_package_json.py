#!/usr/bin/env python3
"""Generate the VS Code extension manifest for a release platform.

This is the CMake-era replacement for ``compile/common/package_json.lua`` (the
data source) plus the ``json-beautify`` writing step of
``compile/common/write_json.lua``.  Standard library only; one platform table,
no per-platform copies of the manifest.

Usage:
    python3 tools/gen_package_json.py --platform darwin-arm64 --output package.json
"""

import argparse
import json
import re
import sys

# Release platforms this generator knows how to describe.  Each one is
# "<os>-<arch>" exactly as the old Lua generator parsed it.
SUPPORTED_PLATFORMS = (
    "darwin-arm64",
    "darwin-x64",
    "linux-arm64",
    "linux-x64",
    "win32-x64",
)

PLATFORM_PATTERN = re.compile(r"^([^-]+)-([^-]+)$")


def _base_manifest():
    """The manifest skeleton, identical for every platform.

    Mirrors the ``local json = { ... }`` literal of package_json.lua, including
    the empty ``body.arg`` table (empty Lua tables serialise as ``[]``).
    """
    return {
        "name": "lua-debug-server",
        "version": "2.3.0",
        "publisher": "fesily",
        "displayName": "Lua Debug",
        "description": "VSCode debugger extension for Lua",
        "icon": "images/logo.png",
        "private": True,
        "author": {
            "name": "actboy168",
        },
        "bugs": {
            "url": "https://github.com/actboy168/lua-debug/issues",
        },
        "repository": {
            "type": "git",
            "url": "https://github.com/actboy168/lua-debug",
        },
        "keywords": [
            "lua",
            "debug",
            "debuggers",
        ],
        "categories": [
            "Debuggers",
        ],
        "engines": {
            "vscode": "^1.75.0",
        },
        "extensionKind": [
            "workspace",
        ],
        "main": "./js/extension.js",
        "activationEvents": [
            "onDebugInitialConfigurations",
            "onDebugDynamicConfigurations",
            "onDebugResolve:lua",
        ],
        "capabilities": {
            "untrustedWorkspaces": {
                "description": "Debugging is disabled in Restricted Mode.",
                "supported": False,
            },
        },
        "contributes": {
            "breakpoints": [
                {
                    "language": "lua",
                },
                {
                    "language": "html",
                },
            ],
            "commands": [
                {
                    "command": "extension.lua-debug-server.runEditorContents",
                    "icon": "$(play)",
                    "title": "Run File",
                },
                {
                    "command": "extension.lua-debug-server.debugEditorContents",
                    "icon": "$(debug-alt-small)",
                    "title": "Debug File",
                },
                {
                    "command": "extension.lua-debug-server.showIntegerAsDec",
                    "title": "Show as Dec",
                },
                {
                    "command": "extension.lua-debug-server.showIntegerAsHex",
                    "title": "Show as Hex",
                },
            ],
            "configuration": {
                "properties": {
                    "lua.debug.variables.showIntegerAsHex": {
                        "default": False,
                        "description": "Show integer as hex.",
                        "type": "boolean",
                    },
                },
            },
            "debuggers": [
                {
                    "type": "lua",
                    "languages": [
                        "lua",
                    ],
                    "label": "Lua Debug",
                    "configurationSnippets": [
                        {
                            "label": "Lua Debug: Launch Script",
                            "description": "A new configuration for launching a lua debug program",
                            "body": {
                                "type": "lua",
                                "request": "launch",
                                "name": "${1:launch}",
                                "stopOnEntry": True,
                                "program": "^\"\\${workspaceFolder}/${2:main.lua}\"",
                                "arg": [],
                            },
                        },
                        {
                            "label": "Lua Debug: Attach",
                            "description": "A new configuration for attaching a lua debug program",
                            "body": {
                                "type": "lua",
                                "request": "attach",
                                "name": "${1:attach}",
                                "stopOnEntry": True,
                                "address": "127.0.0.1:4278",
                            },
                        },
                    ],
                },
            ],
            "menus": {
                "debug/variables/context": [
                    {
                        "command": "extension.lua-debug-server.showIntegerAsDec",
                        "group": "1_view",
                        "when": (
                            "debugConfigurationType == 'lua' && "
                            "debugProtocolVariableMenuContext == 'integer/hex'"
                        ),
                    },
                    {
                        "command": "extension.lua-debug-server.showIntegerAsHex",
                        "group": "1_view",
                        "when": (
                            "debugConfigurationType == 'lua' && "
                            "debugProtocolVariableMenuContext == 'integer/dec'"
                        ),
                    },
                ],
                "editor/title/run": [
                    {
                        "command": "extension.lua-debug-server.runEditorContents",
                        "when": "resourceLangId == lua",
                    },
                    {
                        "command": "extension.lua-debug-server.debugEditorContents",
                        "when": "resourceLangId == lua",
                    },
                ],
            },
        },
    }


def _common_attributes():
    """``attributes.common``: options shared by launch and attach."""
    return {
        # `auto` is the default: the ABI is detected in the target, and an
        # explicit value is a constraint the detected VM must satisfy. A
        # materialised "lua54" default would reject every other interpreter.
        "luaVersion": {
            "default": "auto",
            "enum": [
                "auto",
                "lua51",
                "lua52",
                "lua53",
                "lua54",
                "lua55",
                "lua-latest",
                "luajit",
            ],
            "enummarkdownDescriptions": [
                "%lua.debug.launch.luaVersion.auto.description%",
                "Lua 5.1 ABI.",
                "Lua 5.2 ABI.",
                "Lua 5.3 ABI.",
                "Lua 5.4 ABI.",
                "Lua 5.5 ABI.",
                "Newest bundled ABI (Lua 5.5).",
                "LuaJIT ABI.",
            ],
            "markdownDescription": "%lua.debug.launch.luaVersion.description%",
            "type": "string",
        },
        # Optional description of the debuggee's Lua implementation, for binaries
        # that export nothing. All three fields are independent, and none of them
        # implies another.
        "luaBinary": {
            "markdownDescription": "%lua.debug.launch.luaBinary.description%",
            "type": "string",
        },
        "luaSymbols": {
            "markdownDescription": "%lua.debug.launch.luaSymbols.description%",
            "type": "object",
        },
        "luaSymbolsFile": {
            "markdownDescription": "%lua.debug.launch.luaSymbolsFile.description%",
            "type": "string",
        },
        "outputCapture": {
            "default": [],
            "items": {
                "enum": [
                    "print",
                    "io.write",
                    "stdout",
                    "stderr",
                ],
            },
            "markdownDescription": "From where to capture output messages: print or stdout/stderr streams.",
            "type": "array",
        },
        "pathFormat": {
            "default": "path",
            "enum": [
                "path",
                "linuxpath",
            ],
            "markdownDescription": "Path format",
            "type": "string",
        },
        "sourceFormat": {
            "default": "path",
            "enum": [
                "path",
                "string",
                "linuxpath",
            ],
            "markdownDescription": "Source format",
            "type": "string",
        },
        "sourceMaps": {
            "default": [
                [
                    "./*",
                    "${workspaceFolder}/*",
                ],
            ],
            "markdownDescription": "The source path of the remote host and the source path of local.",
            "type": "array",
        },
        "skipFiles": {
            "default": [],
            "items": {
                "type": "string",
            },
            "markdownDescription": "An array of glob patterns for files to skip when debugging.",
            "type": "array",
        },
        "stopOnEntry": {
            "default": False,
            "markdownDescription": "Automatically stop after entry.",
            "type": "boolean",
        },
        "stopOnThreadEntry": {
            "default": True,
            "markdownDescription": "Automatically stop after thread entry.",
            "type": "boolean",
        },
        "keepSessionAlive": {
            "default": False,
            "markdownDescription": (
                "Keep the debug session alive when the current debugged Lua VM is closed and "
                "later recreated in the same process."
            ),
            "type": "boolean",
        },
        "address": {
            "markdownDescription": (
                "Debugger address.\n"
                "1. IPv4 e.g. `127.0.0.1:4278`\n"
                "2. IPv6 e.g. `[::1]:4278`\n"
                "3. Unix domain socket e.g. `@c:\\\\unix.sock`"
            ),
            "type": "string",
        },
        "client": {
            "default": True,
            "markdownDescription": "Choose whether to `connect` or `listen`.",
            "type": "boolean",
        },
        "inject": {
            "default": "frida",
            "markdownDescription": (
                "How the debugger gets into the target process.\n"
                "\n"
                "- `frida`: inject the launcher with Frida (default for attaching to a process and\n"
                "  for `runtimeExecutable` launches).\n"
                "- `none`: never inject. Use it together with `address` (JSON socket / TCP) or\n"
                "  when the host already loads the debugger itself.\n"
                "\n"
                "The old `gdb`, `lldb`, `hook` strategies and `inject_executable` were removed;\n"
                "a configuration that still uses them is rejected with a migration error."
            ),
            "enum": [
                "none",
                "frida",
            ],
            "type": "string",
        },
    }


def _attach_attributes():
    """``attributes.attach``: options only meaningful when attaching."""
    return {
        "processId": {
            "default": "${command:pickProcess}",
            "markdownDescription": "Id of process to attach to.",
            "type": "string",
        },
        "processName": {
            "default": "lua.exe",
            "markdownDescription": "Name of process to attach to.",
            "type": "string",
        },
        "waitForDebugger": {
            "default": False,
            "markdownDescription": "Wait for debugger to attach. (It needs to be implemented in the debugging host.)",
            "type": "boolean",
        },
    }


def _launch_attributes(os_name):
    """``attributes.launch``: options only meaningful when launching."""
    return {
        "luaexe": {
            "markdownDescription": "Absolute path to the lua exe.",
            "type": "string",
        },
        "program": {
            "default": "${workspaceFolder}/main.lua",
            "markdownDescription": "Lua program to debug - set this to the path of the script",
            "type": "string",
        },
        "arg": {
            "default": [],
            "markdownDescription": "Command line argument, arg[1] ... arg[n]",
            "type": "array",
        },
        "arg0": {
            "default": [],
            "markdownDescription": "Command line argument, arg[-n] ... arg[0]",
            "type": [
                "string",
                "array",
            ],
        },
        "path": {
            "default": "${workspaceFolder}/?.lua",
            "markdownDescription": "%lua.debug.launch.path.description%",
            "type": [
                "string",
                "array",
                "null",
            ],
        },
        "cpath": {
            "markdownDescription": "%lua.debug.launch.cpath.description%",
            "type": [
                "string",
                "array",
                "null",
            ],
        },
        "luaArch": {
            "markdownDescription": "%lua.debug.launch.luaArch.description%",
            "type": "string",
        },
        "cwd": {
            "default": "${workspaceFolder}",
            "markdownDescription": "Working directory at program startup",
            "type": [
                "string",
                "null",
            ],
        },
        "env": {
            "additionalProperties": {
                "type": [
                    "string",
                    "null",
                ],
            },
            "default": {
                "PATH": "${workspaceFolder}",
            },
            # Wording (including the missing space) is copied from package_json.lua.
            "markdownDescription": (
                "Environment variables passed to the program. The value `null` removes the"
                "variable from the environment."
            ),
            "type": "object",
        },
        "console": {
            "default": "integratedTerminal",
            "enum": [
                "internalConsole",
                "integratedTerminal",
                "externalTerminal",
            ],
            "enummarkdownDescriptions": [
                "%lua.debug.launch.console.internalConsole.description%",
                "%lua.debug.launch.console.integratedTerminal.description%",
                "%lua.debug.launch.console.externalTerminal.description%",
            ],
            "markdownDescription": "%lua.debug.launch.console.description%",
            "type": "string",
        },
        "runtimeExecutable": {
            "default": (
                "${workspaceFolder}/lua.exe" if os_name == "win32" else "${workspaceFolder}/lua"
            ),
            # Wording (including the missing space) is copied from package_json.lua.
            "markdownDescription": (
                "Runtime to use. Either an absolute path or the name of a runtime available"
                "on the PATH."
            ),
            "type": [
                "string",
                "null",
            ],
        },
        "runtimeArgs": {
            "default": "${workspaceFolder}/main.lua",
            "markdownDescription": "Arguments passed to the runtime executable.",
            "type": [
                "string",
                "array",
                "null",
            ],
        },
    }


def _supported_archs(os_name, arch):
    """Architectures the Lua debugger ships for this OS/arch pair."""
    if os_name == "win32":
        return ["x86_64", "x86"]
    if os_name == "darwin":
        return ["arm64", "x86_64"] if arch == "arm64" else ["x86_64"]
    if os_name == "linux":
        return ["arm64"] if arch == "arm64" else ["x86_64"]
    raise ValueError("no supported architectures for %s-%s" % (os_name, arch))


def build_manifest(platform):
    """Build the manifest dict for ``platform`` (e.g. ``darwin-arm64``)."""
    if platform not in SUPPORTED_PLATFORMS:
        raise ValueError(
            "unsupported platform %r (allowed values: %s)"
            % (platform, ", ".join(SUPPORTED_PLATFORMS))
        )
    match = PLATFORM_PATTERN.match(platform)
    os_name, arch = match.group(1), match.group(2)

    document = _base_manifest()
    debugger = document["contributes"]["debuggers"][0]
    debugger["variables"] = {
        "pickProcess": "extension.lua-debug-server.pickProcess",
    }

    common = _common_attributes()
    attach = _attach_attributes()
    launch = _launch_attributes(os_name)

    snippets = debugger["configurationSnippets"]
    if os_name in ("win32", "darwin"):
        snippets.append(
            {
                "label": "Lua Debug: Launch Process",
                "description": "A new configuration for launching a lua process",
                "body": {
                    "type": "lua",
                    "request": "launch",
                    "name": "${1:launch process}",
                    "stopOnEntry": True,
                    "runtimeExecutable": "^\"\\${workspaceFolder}/lua.exe\"",
                    "runtimeArgs": "^\"\\${workspaceFolder}/${2:main.lua}\"",
                },
            }
        )
        snippets.append(
            {
                "label": "Lua Debug: Attach Process",
                "description": "A new configuration for attaching a lua debug program",
                "body": {
                    "type": "lua",
                    "request": "attach",
                    "name": "${1:attach}",
                    "stopOnEntry": True,
                    "processId": "^\"\\${command:pickProcess}\"",
                },
            }
        )

    if os_name == "win32":
        common["sourceCoding"] = {
            "default": "utf8",
            "enum": [
                "utf8",
                "ansi",
            ],
            "markdownDescription": "%lua.debug.launch.sourceCoding.description%",
            "type": "string",
        }
        common["useWSL"] = {
            "default": True,
            "description": "Use Windows Subsystem for Linux.",
            "type": "boolean",
        }
        launch["luaexe"]["default"] = "${workspaceFolder}/lua.exe"
        launch["cpath"]["default"] = "${workspaceFolder}/?.dll"
    else:
        launch["luaexe"]["default"] = "${workspaceFolder}/lua"
        launch["cpath"]["default"] = "${workspaceFolder}/?.so"

    archs = _supported_archs(os_name, arch)
    launch["luaArch"]["default"] = archs[0]
    launch["luaArch"]["enum"] = archs

    attach.update(common)
    launch.update(common)
    debugger["configurationAttributes"] = {
        "launch": {"properties": launch},
        "attach": {"properties": attach},
    }

    settings = document["contributes"]["configuration"]["properties"]
    for name in (
        "luaArch",
        "luaVersion",
        "luaBinary",
        "luaSymbols",
        "luaSymbolsFile",
        "sourceCoding",
        "console",
        "path",
        "cpath",
        "address",
    ):
        attr = launch.get(name) or attach.get(name)
        if attr is None:
            continue
        entry = {}
        for key, value in attr.items():
            if key == "markdownDescription":
                entry["description"] = value
            elif key == "enummarkdownDescriptions":
                entry["enumDescriptions"] = value
            else:
                entry[key] = value
        settings["lua.debug.settings." + name] = entry

    return document


def render(manifest):
    """Serialise ``manifest`` the way json.beautify does: 4 spaces, sorted keys."""
    return json.dumps(manifest, indent=4, sort_keys=True, ensure_ascii=False)


def _parse_args(argv):
    allowed = ", ".join(SUPPORTED_PLATFORMS)
    parser = argparse.ArgumentParser(
        description="Generate the lua-debug VS Code extension manifest.",
    )
    parser.add_argument("--platform", metavar="PLATFORM", help="release platform: " + allowed)
    parser.add_argument("--output", metavar="PATH", help="manifest file to write")
    args = parser.parse_args(argv)
    if args.platform is None:
        parser.error("--platform is required (allowed values: %s)" % allowed)
    if args.platform not in SUPPORTED_PLATFORMS:
        parser.error("unsupported platform %r (allowed values: %s)" % (args.platform, allowed))
    if args.output is None:
        parser.error("--output is required")
    return args


def main(argv=None):
    args = _parse_args(argv)
    text = render(build_manifest(args.platform))
    try:
        with open(args.output, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
    except OSError as exc:
        print("error: cannot write %s: %s" % (args.output, exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
