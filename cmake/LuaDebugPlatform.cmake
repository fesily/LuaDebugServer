# Platform identity for the lua-debug build.
#
# Two names are kept apart on purpose:
#   * LUA_DEBUG_DIST_PLATFORM - the distribution package this build tree
#     contributes to (`darwin-arm64`, `win32-x64`, `linux-arm64`, ...).
#   * LUA_DEBUG_PLATFORM - the platform this particular build tree emits
#     binaries for.
# They differ whenever a distribution carries more than one architecture:
# `win32-x64` ships an x86 adapter plus an x64 launcher/runtime, and
# `darwin-arm64` ships an arm64 adapter plus an x64 launcher/runtime. The
# companion build (see Companion.cmake) is configured with the second value
# only.
#
# This file is included before project(), because the deployment target and the
# target triple have to be known by the first compiler check.

function(_lua_debug_detect_host_platform out)
    if(APPLE)
        if(CMAKE_OSX_ARCHITECTURES)
            set(arch "${CMAKE_OSX_ARCHITECTURES}")
        else()
            execute_process(COMMAND uname -m OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()
        if(arch STREQUAL "arm64")
            set(platform darwin-arm64)
        else()
            set(platform darwin-x64)
        endif()
    elseif(WIN32)
        if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64" OR "$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "ARM64")
            set(platform win32-arm64)
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(platform win32-x64)
        else()
            set(platform win32-ia32)
        endif()
    else()
        execute_process(COMMAND uname -m OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(arch STREQUAL "aarch64" OR arch STREQUAL "arm64")
            set(platform linux-arm64)
        else()
            set(platform linux-x64)
        endif()
    endif()
    set(${out} "${platform}" PARENT_SCOPE)
endfunction()

# `darwin-arm64` and friends are the only spellings the publish tree uses; the
# canonical architecture names below are what the compiler and the Frida devkit
# names are keyed by.
function(_lua_debug_split_platform platform os_var arch_var)
    string(REGEX MATCH "^([^-]+)-(.+)$" _ "${platform}")
    set(os "${CMAKE_MATCH_1}")
    set(arch "${CMAKE_MATCH_2}")
    if(arch STREQUAL "x64")
        set(compiler_arch "x86_64")
    elseif(arch STREQUAL "ia32")
        set(compiler_arch "x86")
    else()
        set(compiler_arch "${arch}")
    endif()
    set(${os_var} "${os}" PARENT_SCOPE)
    set(${arch_var} "${compiler_arch}" PARENT_SCOPE)
endfunction()

if(NOT LUA_DEBUG_DIST_PLATFORM)
    _lua_debug_detect_host_platform(LUA_DEBUG_DIST_PLATFORM)
endif()

if(NOT LUA_DEBUG_PLATFORM)
    set(LUA_DEBUG_PLATFORM "${LUA_DEBUG_DIST_PLATFORM}")
endif()

_lua_debug_split_platform("${LUA_DEBUG_PLATFORM}" LUA_DEBUG_OS LUA_DEBUG_ARCH)
_lua_debug_split_platform("${LUA_DEBUG_DIST_PLATFORM}" LUA_DEBUG_DIST_OS LUA_DEBUG_DIST_ARCH)

set(LUA_DEBUG_RUNTIME_PLATFORM "${LUA_DEBUG_PLATFORM}")

# The companion architecture this build tree assembles into the same publish
# directory, or empty when the distribution is single-arch.
if(LUA_DEBUG_PLATFORM STREQUAL LUA_DEBUG_DIST_PLATFORM)
    if(LUA_DEBUG_DIST_PLATFORM STREQUAL "darwin-arm64")
        set(LUA_DEBUG_COMPANION_PLATFORM "darwin-x64")
    elseif(LUA_DEBUG_DIST_PLATFORM STREQUAL "win32-x64")
        # The Windows distribution leads with the x86 adapter; the x64 pieces
        # (launcher, runtime, interpreters, bridges) come from the companion.
        set(LUA_DEBUG_COMPANION_PLATFORM "win32-x64")
    endif()
endif()

# macOS deployment targets. The C++26 language mode does not change the minimum
# system the shipped binaries must run on; `<filesystem>` sets it: the project
# uses `std::filesystem` everywhere (one path type, no Boost.Filesystem), which
# macOS introduced in 10.15. arm64 starts at 11.0 anyway.
if(APPLE)
    if(LUA_DEBUG_ARCH STREQUAL "arm64")
        set(LUA_DEBUG_MACOS_DEPLOYMENT_TARGET "11.0")
    else()
        set(LUA_DEBUG_MACOS_DEPLOYMENT_TARGET "10.15")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${LUA_DEBUG_MACOS_DEPLOYMENT_TARGET}")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "${LUA_DEBUG_ARCH}")
    endif()
endif()

set(LUA_DEBUG_TARGET_TRIPLE "")
if(APPLE)
    set(LUA_DEBUG_TARGET_TRIPLE "${LUA_DEBUG_ARCH}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# frida devkit directories are keyed by `<os>-<arch>` with the compiler's
# spelling of the architecture.
set(LUA_DEBUG_FRIDA_PREFIX "")
if(LUA_DEBUG_OS STREQUAL "darwin")
    set(LUA_DEBUG_FRIDA_PREFIX "macos")
elseif(LUA_DEBUG_OS STREQUAL "win32")
    set(LUA_DEBUG_FRIDA_PREFIX "windows")
else()
    set(LUA_DEBUG_FRIDA_PREFIX "${LUA_DEBUG_OS}")
endif()
set(LUA_DEBUG_FRIDA_TARGET "${LUA_DEBUG_FRIDA_PREFIX}-${LUA_DEBUG_ARCH}")

message(STATUS "lua-debug: dist platform ${LUA_DEBUG_DIST_PLATFORM}, building ${LUA_DEBUG_PLATFORM} (os ${LUA_DEBUG_OS}, arch ${LUA_DEBUG_ARCH})")
if(LUA_DEBUG_COMPANION_PLATFORM)
    message(STATUS "lua-debug: companion platform ${LUA_DEBUG_COMPANION_PLATFORM}")
endif()
