# LuaJIT: the plain v2.1 sources from `fesily/luajit`, built with the CMake
# project that lives on that fork's `dontstarve` branch.
#
# The split is deliberate:
#   * `3rd/lua/luajit` is the v2.1 branch - what this debugger targets and what
#     ships;
#   * `cmake/luajit/CMakeLists.txt` is the vendored build script from the
#     `dontstarve` branch (see its header for provenance and the three
#     deviations), so the DynASM/buildvm machinery is maintained upstream instead
#     of being re-implemented here.
#
# What this module adds on top:
#   * the options this project's bridges need (Lua 5.2 compatibility, nummode 2),
#   * deterministic output names/locations, so the publish tree keeps the
#     `<version>/lua` + `<version>/luajit.dll` layout,
#   * one include interface (`luajit_headers`) for the bridge and the launcher,
#     covering the per-variant *generated* `luajit.h`,
#   * the ld64 workaround described below.

include_guard(GLOBAL)

set(LUA_DEBUG_LUAJIT_DIR "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/luajit")
set(LUA_DEBUG_LUAJIT_SRC "${LUA_DEBUG_LUAJIT_DIR}/src")

if(NOT EXISTS "${LUA_DEBUG_LUAJIT_SRC}/lj_obj.h")
    message(FATAL_ERROR
        "lua-debug: the LuaJIT submodule at ${LUA_DEBUG_LUAJIT_DIR} is missing or "
        "empty; run `git submodule update --init 3rd/lua/luajit` (branch v2.1).")
endif()

# Options the vendored script reads. Defaults, not FORCE: a packaging build can
# still override them from the command line.
set(LUAJIT_DIR "${LUA_DEBUG_LUAJIT_DIR}")
set(LUAJIT_ENABLE_LUA52COMPAT ON CACHE BOOL "Enable Lua 5.2 compatibility")
set(LUAJIT_NUMMODE "2" CACHE STRING "LuaJIT number mode")
set(WITH_SHARED_LUA ON CACHE BOOL "Build the shared LuaJIT library")
set(WITH_AMALG ON CACHE BOOL "Build LuaJIT from the amalgamated source")
set(LUAJIT_TEST_TAILCALL OFF CACHE BOOL "Upstream native tail-call tests")
set(LUAJIT_TEST_STACK_OOM OFF CACHE BOOL "Upstream stack OOM tests")
set(LUAJIT_TEST_ETW OFF CACHE BOOL "Upstream Windows ETW tests")
# The arena-GC variant needs LUAJIT_ENABLE_GCARENA, which the v2.1 sources do not
# implement (it is a dontstarve-branch feature).
set(LUAJIT_BUILD_ARENAGC OFF CACHE BOOL "Build the arena-GC variant (dontstarve only)")

# Out-of-tree build directory: the submodule is shared between build trees, its
# own in-tree build artifacts must never be produced from here.
#
# EXCLUDE_FROM_ALL keeps the upstream project's extra targets (the shared library
# when not needed, host tools beyond the default variant) out of `all`; the
# interpreter is still built through its explicit dependencies below.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/luajit" "${CMAKE_BINARY_DIR}/lua/luajit" EXCLUDE_FROM_ALL)

set(LUA_DEBUG_LUAJIT_VARIANT_DIR "${CMAKE_BINARY_DIR}/lua/luajit/variants/default")

if(NOT TARGET luajit OR NOT TARGET luajit-5.1)
    message(FATAL_ERROR
        "lua-debug: the LuaJIT CMake project did not provide the expected "
        "`luajit` (interpreter) and `luajit-5.1` (library) targets")
endif()

# --- published layout --------------------------------------------------------
#
# POSIX: the interpreter is published as `runtime/<platform>/luajit/lua` and must
# export the Lua API, because the bridge resolves `lua_*` from the host process.
# Windows: the interpreter links `luajit.dll` (the bridge delay-loads
# `LUA_DLL_VERSION=luajit`), so the library keeps that name too.
set_target_properties(luajit PROPERTIES
    OUTPUT_NAME lua
    RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_BIN_DIR}/luajit")

set_target_properties(luajit-5.1 PROPERTIES
    OUTPUT_NAME luajit
    RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}/luajit"
    LIBRARY_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}/luajit")

if(APPLE)
    # ld64's stub optimization (implicitly enabled by the -O level CMake puts on
    # the link line) produces a binary this hand-written VM cannot run: string
    # interning then reads a stale table. Upstream links without it.
    target_link_options(luajit PRIVATE "-Wl,-O0")
    # Same reason as the plain interpreters (cmake/LuaTargets.cmake): the
    # launcher/bridge are C++ DSOs mapped into this process, and frida's injector
    # does not run their dependency initializers, so libc++ has to be loaded by
    # the host already. Also gives the published interpreter the LuaJIT dynamic
    # loader path in `jet` code a working libc++.
    # See cmake/LuaTargets.cmake: `-lc++` alone would be dropped as unused.
    target_link_options(luajit PRIVATE "-Wl,-needed-lc++")
endif()

if(UNIX AND NOT APPLE)
    # The vendored build compiles C as strict `-std=c11`, and glibc hides the POSIX
    # declarations behind feature macros in that mode: `luajit.c` then fails on
    # `struct sigaction`/`sigemptyset`. `_DEFAULT_SOURCE` is glibc's own opt-in for
    # the POSIX + BSD surface the interpreter uses (the same reason `-std=gnu11`
    # would work, without loosening the standard for the whole file).
    target_compile_definitions(luajit PRIVATE _DEFAULT_SOURCE)
    target_compile_definitions(luajit-5.1 PRIVATE _DEFAULT_SOURCE)
endif()

# --- headers for the bridge and the launcher ---------------------------------
#
# `<lua.hpp>` includes the *generated* `luajit.h` (version macros, including
# LUAJIT_VERSION, which selects the JIT code paths in compat/jit and
# compat/internal.h), so the per-variant directory has to be on the include path
# and the build has to wait for it.
add_library(luajit_headers INTERFACE)
target_include_directories(luajit_headers INTERFACE
    "${LUA_DEBUG_LUAJIT_SRC}"
    "${LUA_DEBUG_LUAJIT_VARIANT_DIR}")
add_dependencies(luajit_headers luajit-5.1)

set(LUA_DEBUG_LUAJIT_INCLUDE_DIRS
    "${LUA_DEBUG_LUAJIT_SRC}"
    "${LUA_DEBUG_LUAJIT_VARIANT_DIR}")

# --- publish -----------------------------------------------------------------
#
# The target lives in the subdirectory, so no POST_BUILD rule can be attached to
# it here; a dedicated target copies the artifacts instead. It exists in every
# build tree - the subdirectory is EXCLUDE_FROM_ALL, so something has to depend on
# `luajit` for it to be built at all - and only performs the copy when this tree
# assembles the publish directory.
set(_luajit_publish_commands "")
if(LUA_DEBUG_PACKAGING)
    set(_luajit_publish_dir
        "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_RUNTIME_PLATFORM}/luajit")
    set(_luajit_publish_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_luajit_publish_dir}")
    if(WIN32)
        list(APPEND _luajit_publish_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:luajit>" "${_luajit_publish_dir}/lua.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:luajit-5.1>" "${_luajit_publish_dir}/luajit.dll")
    else()
        list(APPEND _luajit_publish_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:luajit>" "${_luajit_publish_dir}/lua")
    endif()
endif()

add_custom_target(luajit_publish ALL
    ${_luajit_publish_commands}
    DEPENDS luajit
    COMMENT "luajit interpreter"
    VERBATIM)
