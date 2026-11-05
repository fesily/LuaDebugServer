#!/usr/bin/env bash
# Runs inside the container: prepare a working copy, then hand over to tools/ci.py.
#
# argv: <preset> <ci.py command...>
#
# The mounted checkout is read-only and is never built in place:
#   * its `3rd/_deps` holds artefacts built for the host (macOS) architecture,
#   * the submodule `.git` entries point outside this tree, so LuaJIT's
#     `git show -s --format=%ct` (used to stamp its version) cannot work.
# So the sources are copied into /work, which is a volume: the build tree survives
# between runs (incremental), while the sources are re-synced on every run so an
# edit is always picked up.
set -euo pipefail

preset="${1:?usage: ci-entrypoint.sh <preset> <command...>}"
shift

stamp="/work/.preset"
if [[ -f "${stamp}" && "$(cat "${stamp}")" != "${preset}" ]]; then
    # Different architecture/preset: the build tree is not reusable.
    echo "lua-debug docker: switching to ${preset}, discarding the previous tree"
    rm -rf /work/src
fi
mkdir -p /work/src
echo "lua-debug docker: syncing sources into /work/src"
# Drop the previous run's stub repositories first: `cp` refuses to write the host's
# submodule `.git` *file* over a `.git` directory, and the stub is recreated below.
rm -rf /work/src/3rd/lua/luajit/.git /work/src/3rd/frida_gum/gumpp/.git

# `cp -a` rather than a tar stream: under this engine's amd64 emulation, extracting
# a large tar stream into the volume fails with `ENOSYS` ("Cannot open: Function not
# implemented") for most entries, while per-entry copies work. Copying over the
# existing tree also keeps `build/` and `publish/` from previous runs, which is what
# makes a rebuild incremental.
for entry in $(ls -A /repo); do
    case "${entry}" in
    build | publish | .git) continue ;;
    ../../* | ..) continue ;;
    esac
    cp -a "/repo/${entry}" /work/src/
done
# The host's dependency cache and the submodules' dangling git pointers stay behind
# (the pointer is a relative path into the *host* repository, meaningless here).
rm -rf /work/src/3rd/_deps /work/src/3rd/_cache
rm -f /work/src/3rd/lua/luajit/.git /work/src/3rd/frida_gum/gumpp/.git
echo "${preset}" >"${stamp}"

cd /work/src

# LuaJIT's build script runs `git show -s --format=%ct` unconditionally; give the
# submodule a repository of its own. A submodule `.git` *file* (a pointer into the
# parent repository) is dangling here, so it is dropped first. `safe.directory`
# because the mounted tree may carry another owner than root.
if [[ -f 3rd/lua/luajit/.git ]]; then
    rm -f 3rd/lua/luajit/.git
fi
if [[ ! -e 3rd/lua/luajit/.git ]]; then
    git config --global --add safe.directory '*'
    git -C 3rd/lua/luajit init -q .
    git -C 3rd/lua/luajit -c user.email=ci@container -c user.name=ci \
        commit -q --allow-empty -m "container stub for luajit_relver"
fi

# Root with CAP_SYS_PTRACE already satisfies YAMA, so this is only a best effort
# for engines that drop the capability; the attach suites report a skip if they
# cannot run either way.
sysctl -w kernel.yama.ptrace_scope=0 >/dev/null 2>&1 || true

exec python3 tools/ci.py --preset "${preset}" --dep-cache /deps "$@"
