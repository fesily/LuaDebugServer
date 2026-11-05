# Linux verification in Docker

Three files, three jobs:

| file | role |
|---|---|
| `Dockerfile` | the image: Ubuntu 26.04 LTS, `clang-21`, `cmake`, `ninja`, `python3`, `git`. The versions the presets and the CMake floor ask for, asserted at build time. |
| `docker-compose.yaml` | the runtime shape for both architectures: platform, the read-only checkout, and the per-architecture volumes. |
| `ci-entrypoint.sh` | runs *inside*: prepares `/work`, then hands over to `tools/ci.py`. |

`tools/docker-ci.sh` is the driver on the host; it builds the image explicitly
(`docker build --platform ...`, because the classic builder silently tags the host
architecture) and then runs the service.

## Usage

```sh
tools/docker-ci.sh                      # host arch: configure build ctest e2e-pure
tools/docker-ci.sh --arch amd64         # other arch: configure build (see below)
tools/docker-ci.sh --arch both
tools/docker-ci.sh --arch arm64 -- e2e-surface dap
tools/docker-ci.sh --arch arm64 --export /tmp/linux-artifacts
```

Every command is a `tools/ci.py` command, so the suite lists, version sets and
timeouts are the ones the GitHub workflow uses — this script only chooses an
architecture and a container.

## Volumes (per architecture)

| mount | contents |
|---|---|
| `/repo` | the checkout, **read-only**. Never built in place: its `3rd/_deps` holds host-architecture artefacts and the submodule `.git` pointers refer outside the tree. |
| `/work` | the working copy and its build tree. Kept between runs (incremental); the sources are re-synced on every run, so edits are picked up. |
| `/deps` | `LUA_DEBUG_DEP_CACHE`: the SHA256-pinned archives and the extracted sources, so a rebuild does not re-download ~120 MB. |

## Why the emulated architecture only builds

`--arch amd64` on an Apple Silicon host runs the container under CPU emulation.
An emulator loads the guest binary as **data** and interprets it, so
`/proc/self/maps` reports the executable's text as `r--p`. The bridge refuses to
bind a symbol table whose addresses are not in an executable mapping — that guard
is what stops a wrong table from calling into data, so it stays as it is.

The affected tests are the ones that bind the bridge through its C ABI
(`symbol_bind_*`, `two_lua_vms`, `mixed_vm_*`). They run natively in CI
(`build (linux-x64)`), and on a real x86_64 machine. On an emulated container the
default is therefore `configure build`, which still catches compile and link
regressions — the class of bug that had `linux-x64` unbuildable for a while.

Pass commands explicitly to run the suites anyway and see it for yourself:

```sh
tools/docker-ci.sh --arch amd64 -- ctest
```

## Requirements

* `docker` with the **compose** and **buildx** plugins (`docker compose version`,
  `docker buildx version`). Without buildx the classic builder ignores
  `--platform` and produces a host-architecture image that `compose run` then
  rejects.
* `cap_add: SYS_PTRACE` + `seccomp:unconfined`, already in the compose file: the
  `attach` and `inject-config` suites need to ptrace the debuggee.

## Notes

* The image runs as root, so YAMA's `ptrace_scope` does not block the suites; the
  entrypoint still tries to relax it for engines that drop the capability.
* The vendored LuaJIT CMake runs `git show -s --format=%ct` unconditionally, so
  the working copy needs to be a git repository; the entrypoint creates a stub in
  the submodule because the mounted `.git` pointer is relative to the host tree.
* The sources are copied with `cp -a` rather than a tar stream: extracting a large
  tar into the volume fails under the emulated architecture with
  `ENOSYS` (`Cannot open: Function not implemented`), while per-entry copies work.
