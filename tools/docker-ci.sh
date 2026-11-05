#!/usr/bin/env bash
# Linux verification in Docker, for the host architecture or the other one.
#
#   tools/docker-ci.sh                        # arm64 on an Apple Silicon host
#   tools/docker-ci.sh --arch both
#   tools/docker-ci.sh --arch amd64 -- ctest e2e-pure
#   tools/docker-ci.sh --arch arm64 --export /tmp/linux-artifacts
#
# Commands after `--` are `tools/ci.py` commands. The default depends on whether the
# container runs natively:
#   native arch   configure build ctest e2e-pure
#   other arch    configure build              (build verification only)
# An emulated container cannot run the tests that inspect their own memory map: an
# emulator loads the guest binary as data and interprets it, so `/proc/self/maps`
# reports its text as `r--p` and the bridge's "is this address executable" guard
# correctly refuses to bind. That guard is what keeps a wrong symbol table from
# calling into data, so it stays; native x86_64 runners cover those tests
# (`build (linux-x64)` in CI). Pass commands explicitly to override.
#
# Everything runs through the same `tools/ci.py` the GitHub workflow uses, so this
# script only chooses an architecture and a container; it never re-implements a
# suite list or a timeout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "${here}")"

compose="${root}/docker/docker-compose.yaml"
arch="auto"
export_dir=""
commands=()

usage() {
    sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --arch)
        arch="${2:?--arch needs auto|arm64|amd64|both}"
        shift 2
        ;;
    --export)
        export_dir="${2:?--export needs a directory}"
        shift 2
        ;;
    -h | --help)
        usage 0
        ;;
    --)
        shift
        commands=("$@")
        break
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage 2
        ;;
    esac
done

host_arch="$(uname -m)"
if [[ "$(uname -s)" == "Darwin" ]]; then
    host_arch="$(sysctl -n hw.optional.arm64 2>/dev/null | grep -q 1 && echo arm64 || echo amd64)"
fi
[[ "${host_arch}" == "aarch64" ]] && host_arch="arm64"
[[ "${host_arch}" == "x86_64" ]] && host_arch="amd64"

case "${arch}" in
auto) arch="${host_arch}" ;;
arm64 | amd64) ;;
both) ;;
*)
    echo "--arch expects auto|arm64|amd64|both" >&2
    exit 2
    ;;
esac

explicit_commands=${#commands[@]}
if [[ ${#commands[@]} -eq 0 ]]; then
    if [[ "${arch}" == "both" || "${arch}" == "${host_arch}" ]]; then
        commands=(configure build ctest e2e-pure)
    else
        commands=(configure build)
    fi
fi

preset_for() {
    case "$1" in
    arm64) echo "linux-arm64-release" ;;
    amd64) echo "linux-x64-release" ;;
    esac
}

if ! docker compose version >/dev/null 2>&1; then
    echo "lua-debug: 'docker compose' is not available; install the compose plugin" >&2
    exit 2
fi

targets=()
[[ "${arch}" == "both" ]] && targets=(arm64 amd64) || targets=("${arch}")

status=0
for target in "${targets[@]}"; do
    preset="$(preset_for "${target}")"
    echo
    echo "=== linux/${target} (${preset}) ==="
    if [[ "${target}" != "${host_arch}" && "${explicit_commands}" -eq 0 ]]; then
        echo "    emulated: building only (the suites need a native runner; see --help)"
    fi
    # Build explicitly with `--platform`: compose derives the image's platform from
    # the build section, and getting this wrong means `run` refuses the image
    # ("does not provide the specified platform"). The build is inside the loop so
    # `--arch both` never reuses the first architecture's image.
    # BuildKit is required: the classic builder silently ignores `--platform` and
    # tags the host architecture, which `compose run` then rejects.
    DOCKER_BUILDKIT=1 docker build --platform "linux/${target}" \
        -t "lua-debug-${target}" \
        -f "${root}/docker/Dockerfile" "${root}/docker"
    image_platform="$(docker image inspect "lua-debug-${target}" --format '{{.Os}}/{{.Architecture}}' 2>/dev/null || true)"
    echo "image lua-debug-${target}: ${image_platform:-missing}"
    if ! docker compose -f "${compose}" run --rm --no-deps \
        "${target}" /repo/docker/ci-entrypoint.sh "${preset}" "${commands[@]}"; then
        echo "=== linux/${target} FAILED ===" >&2
        status=1
        continue
    fi
    if [[ -n "${export_dir}" ]]; then
        destination="${export_dir}/${preset}"
        mkdir -p "${destination}"
        # /work is a volume, so a second container still sees what the run built.
        docker compose -f "${compose}" run --rm --no-deps --entrypoint /bin/bash \
            "${target}" -c "cd /work/src && tar -cf - publish build/cmake/${preset}/tests/bin" \
            | tar -C "${destination}" -xf -
        echo "exported ${destination}"
    fi
done

exit "${status}"
