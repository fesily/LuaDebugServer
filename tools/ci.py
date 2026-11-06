#!/usr/bin/env python3
"""Run the CI flow, locally or on a runner.

One definition of what CI does, so `python3 tools/ci.py ... all` on a developer
machine and the GitHub workflow cannot drift apart in suite lists, version sets,
timeouts or paths. The workflow only supplies the toolchain and the artifact
hand-off; every command it runs comes from here.

Usage:
  python3 tools/ci.py [options] <command> [<command> ...]
  python3 tools/ci.py --help

Pipeline order:
  style          clang-format check over `src/` (same script developers run)
  configure      cmake --preset
  build          cmake --build --preset
  ctest          ctest --preset --output-on-failure
  e2e-pure       baseline / core / two-sources / vm-source, all interpreters
  e2e-surface    full / mixed-vm
  e2e-inject     launch-inject (preload into a child we start: works unprivileged)
  e2e-privileged attach / inject-config (needs ptrace or the frida entitlement)
  dap            test/dap-node (VS Code's DebugClient against the adapter)
  package        archive what the e2e stage needs (publish + libs + test hosts)
  unpack         restore such an archive into this checkout
  all            style configure build ctest e2e-pure e2e-surface e2e-inject dap

`package`/`unpack` exist because the CI pipeline is two stages: `build` produces
one archive per platform and the `e2e` job consumes it, so the tests run against
exactly the binaries that were built and tested, without rebuilding them.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
import tarfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Suite lists and version sets live here only. `lua-latest` is the bundled
# interpreter whose sources are identical to 5.5; it has its own directory.
ALL_VERSIONS = "lua51,lua52,lua53,lua54,lua55,luajit,lua-latest"
# `full` stops on raised errors (5.2+ only) and `mixed-vm` drives multi-ABI hosts;
# both are meaningful on this subset.
SURFACE_VERSIONS = "lua51,lua54,luajit,lua55,lua-latest"
PURE_SUITES = ("baseline", "coroutine", "core", "two-sources", "late-breakpoint", "vm-source")
SURFACE_SUITES = ("full", "mixed-vm")
PRIVILEGED_SUITES = ("attach", "inject-config")

PRESET_PLATFORM = {
    "darwin-arm64-release": "darwin-arm64",
    "darwin-x64-release": "darwin-x64",
    "linux-x64-release": "linux-x64",
    "linux-arm64-release": "linux-arm64",
    "win32-x64-release": "win32-x64",
}


def host_preset():
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")
    if system == "Darwin":
        return "darwin-arm64-release" if arm else "darwin-x64-release"
    if system == "Linux":
        return "linux-arm64-release" if arm else "linux-x64-release"
    if system == "Windows":
        return "win32-x64-release"
    raise SystemExit("tools/ci.py: unsupported host %s/%s" % (system, machine))


class Layout:
    """Everything a command needs, derived from the preset name."""

    def __init__(self, preset, runtime, adapter, test_bin_dir):
        self.preset = preset
        self.platform = PRESET_PLATFORM.get(preset)
        if not self.platform:
            raise SystemExit("tools/ci.py: unknown preset '%s'" % preset)
        self.runtime = runtime or self.platform
        exe = ".exe" if self.platform.startswith("win32") else ""
        self.adapter = adapter or os.path.join("publish", "bin", "lua-debug" + exe)
        self.build_dir = os.path.join("build", "cmake", preset)
        self.test_bin_dir = test_bin_dir or os.path.join(self.build_dir, "tests", "bin")
        self.runtime_root = os.path.join("publish", "runtime", self.runtime)


def run(command, cwd=REPO_ROOT):
    print("+ " + " ".join(command), flush=True)
    return subprocess.call(command, cwd=cwd)


def cmake(*args):
    return ["cmake", *args]


def step_style(_layout, _args):
    return run([sys.executable, os.path.join("tools", "format.py"), "--check"])


def step_configure(layout, args):
    command = cmake("--preset", layout.preset)
    if args.dep_cache:
        # Forwarded to CMake because the dependency cache is a configure-time
        # choice (it also decides where Boost is extracted and built).
        command.append("-DLUA_DEBUG_DEP_CACHE=" + os.path.abspath(args.dep_cache))
    return run(command)


def step_build(layout, _args):
    return run(cmake("--build", "--preset", layout.preset))


def step_ctest(layout, _args):
    return run(["ctest", "--preset", layout.preset, "--output-on-failure"])


def e2e(layout, suite, versions, timeout):
    return run([
        sys.executable, os.path.join("test", "e2e", "run_e2e.py"),
        "--adapter", layout.adapter,
        "--runtime-root", layout.runtime_root,
        "--versions", versions,
        "--suite", suite,
        "--timeout", str(timeout),
        "--test-bin-dir", layout.test_bin_dir,
    ])


def run_suites(layout, suites, versions, timeout):
    for suite in suites:
        status = e2e(layout, suite, versions, timeout)
        if status != 0:
            return status
    return 0


def step_e2e_pure(layout, _args):
    return run_suites(layout, PURE_SUITES, ALL_VERSIONS, 90)


def step_e2e_surface(layout, _args):
    return run_suites(layout, SURFACE_SUITES, SURFACE_VERSIONS, 120)


def step_e2e_inject(layout, _args):
    return e2e(layout, "launch-inject", "lua54", 90)


def step_e2e_privileged(layout, _args):
    return run_suites(layout, PRIVILEGED_SUITES, "lua54", 90)


def step_dap(_layout, _args):
    status = run(["npm", "install", "--prefix", os.path.join("test", "dap-node")])
    if status != 0:
        return status
    return run(["npm", "test", "--prefix", os.path.join("test", "dap-node")])


def step_package(layout, args):
    """Archive what the e2e stage needs, for the build -> e2e hand-off."""
    if not args.artifact:
        print("tools/ci.py: package needs --artifact <file.tar.gz>", file=sys.stderr)
        return 2
    members = [
        "publish",
        os.path.join(layout.build_dir, "lib"),
        layout.test_bin_dir,
    ]
    missing = [m for m in members if not os.path.exists(os.path.join(REPO_ROOT, m))]
    if missing:
        print("tools/ci.py: nothing to package, missing %s" % ", ".join(missing), file=sys.stderr)
        return 2
    os.makedirs(os.path.dirname(os.path.abspath(args.artifact)) or ".", exist_ok=True)
    with tarfile.open(args.artifact, "w:gz") as archive:
        for member in members:
            archive.add(os.path.join(REPO_ROOT, member), arcname=member)
    size = os.path.getsize(args.artifact) / (1024 * 1024)
    print("packaged %s (%.1f MB)" % (args.artifact, size))
    return 0


def step_unpack(_layout, args):
    if not args.artifact or not os.path.exists(args.artifact):
        print("tools/ci.py: unpack needs an existing --artifact", file=sys.stderr)
        return 2
    with tarfile.open(args.artifact, "r:gz") as archive:
        archive.extractall(REPO_ROOT)
    print("unpacked %s" % args.artifact)
    return 0


COMMANDS = {
    "style": step_style,
    "configure": step_configure,
    "build": step_build,
    "ctest": step_ctest,
    "e2e-pure": step_e2e_pure,
    "e2e-surface": step_e2e_surface,
    "e2e-inject": step_e2e_inject,
    "e2e-privileged": step_e2e_privileged,
    "dap": step_dap,
    "package": step_package,
    "unpack": step_unpack,
}

ALL = [
    "style", "configure", "build", "ctest",
    "e2e-pure", "e2e-surface", "e2e-inject", "dap",
]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", default=os.environ.get("LUA_DEBUG_CI_PRESET") or host_preset())
    parser.add_argument("--runtime", help="runtime directory name (default: derived from the preset)")
    parser.add_argument("--adapter", help="adapter executable (default: publish/bin/lua-debug)")
    parser.add_argument("--test-bin-dir", help="native test binaries (default: build/cmake/<preset>/tests/bin)")
    parser.add_argument("--artifact", help="archive path for `package` and `unpack`")
    parser.add_argument("--dep-cache",
                        help="LUA_DEBUG_DEP_CACHE (default: the preset's 3rd/_deps); "
                             "the Docker flow points this at a volume")
    parser.add_argument("commands", nargs="+", choices=sorted(COMMANDS) + ["all"],
                        help="pipeline steps, in order")
    args = parser.parse_args(argv)

    if "all" in args.commands:
        # `all` is the local one-stop run; explicit commands can still be mixed in.
        order = ALL
    else:
        order = args.commands

    layout = Layout(args.preset, args.runtime, args.adapter, args.test_bin_dir)
    print("tools/ci.py: preset=%s platform=%s runtime=%s"
          % (layout.preset, layout.platform, layout.runtime), flush=True)

    for name in order:
        print("\n=== %s ===" % name, flush=True)
        status = COMMANDS[name](layout, args)
        if status != 0:
            print("tools/ci.py: '%s' failed (exit %s)" % (name, status), file=sys.stderr)
            return status
    return 0


if __name__ == "__main__":
    sys.exit(main())
