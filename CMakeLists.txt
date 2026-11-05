# lua-debug - native debugger frontend, shared runtime and per-version bridges.
#
# Everything below is CMake/Ninja plus a Python 3 standard library for the
# extension packaging steps. There is no build-time Lua: the source tree's Lua is
# the debuggee's, never the build system's.

cmake_minimum_required(VERSION 4.2...4.4)

# The deployment target and the target triple must be known before the first
# compiler check, so platform identity is resolved before project().
include(${CMAKE_CURRENT_LIST_DIR}/cmake/LuaDebugPlatform.cmake)

project(lua-debug LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(LUA_DEBUG_BUILD_TESTS OFF CACHE BOOL "Build the native test targets and register them with CTest")
set(LUA_DEBUG_BUILD_COMPANION ON CACHE BOOL
    "Build the second architecture of a multi-arch distribution (macOS arm64 -> x64, Windows x86 -> x64)")
set(LUA_DEBUG_PACKAGING ON CACHE BOOL
    "Assemble the publish directory (off for companion build trees, which the parent assembles)")
set(LUA_DEBUG_PUBLISH_DIR "${CMAKE_SOURCE_DIR}/publish" CACHE PATH "Publish directory")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/Dependencies.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/ToolchainChecks.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/CommonTargets.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Frida.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/LuaTargets.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/LuaJIT.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Runtime.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Bridges.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Launcher.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Adapter.cmake)

if(LUA_DEBUG_BUILD_TESTS)
    enable_testing()
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/Tests.cmake)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/Packaging.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Companion.cmake)
