# Linux verification image: the toolchain the presets ask for, and nothing else.
#
# Ubuntu 26.04 LTS ships clang 21 (the presets pin `clang-21`) and CMake 4.2.3,
# which satisfies the project's `cmake_minimum_required(4.2...4.4)` floor, so the
# container uses the distro toolchain instead of downloading one. The versions are
# asserted below: if a future base image moves them, the build fails here rather
# than three minutes later inside CMake.
#
# One image serves both architectures; `docker-compose.yaml` picks the platform.

FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-21 \
        cmake \
        curl \
        git \
        ninja-build \
        python3 \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# `git` is not optional: the vendored LuaJIT CMake runs
# `git show -s --format=%ct > luajit_relver.txt` unconditionally, so the working
# copy must be a git repository (the entrypoint creates a stub when the mount is a
# plain directory).
RUN set -eux; \
    clang-21 --version; \
    cmake --version; \
    ninja --version; \
    python3 --version; \
    cmake --version | head -1 | awk '{ if ($3 < "4.2") { print "CMake " $3 " is below the 4.2 floor"; exit 1 } }'

# The source is mounted read-only at /repo and copied into /work by the
# entrypoint; /deps is the dependency cache (a volume, so a rebuild does not
# re-download ~120 MB of pinned archives).
WORKDIR /work

ENTRYPOINT ["/bin/bash"]
