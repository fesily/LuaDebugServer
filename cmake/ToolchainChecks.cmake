# C++26 / coroutine toolchain gate.
#
# Two separate checks, both mandatory:
#
#  1. At configure time, `try_compile` compiles the probe translation unit that
#     asserts the language mode, the coroutine feature macros, <bit>/<span>/
#     <utility> facilities and instantiates the Asio/Process coroutine
#     integration. It cannot *link* Boost at configure time - Boost is built by
#     this same build tree and does not exist yet - so linking is covered by
#     step 2.
#
#  2. `luadebug_toolchain_probe` is a real target: it compiles the same probe,
#     links the pinned Boost (Asio, Process, Filesystem, System) and fmt with
#     the deployment target, architecture, PIC and CRT settings of the shipped
#     binaries, and runs the coroutine suspend/resume assertions as a CTest.
#
# A failure means the toolchain is wrong, not that the sources need a fallback:
# there is no C++17/20/23 path, no `-fno-char8_t`, and no callback I/O variant.

include_guard(GLOBAL)

set(LUA_DEBUG_CXX_STANDARD 26)
set(LUA_DEBUG_C_STANDARD 11)
set(LUA_DEBUG_TOOLCHAIN_ERROR
    "C++26 coroutine toolchain required: install/select a compiler with C++26 language mode, coroutines and Boost.Asio/Process awaitable support
  macOS: AppleClang >= 17 with a matching SDK (xcrun --find clang)
  Linux: LLVM Clang 21 (clang-21 / clang++-21)
  Windows: LLVM clang-cl 21 inside an MSVC x86/x64 developer environment, Ninja generator")

# Applies the language requirements to one of *our* targets. Third-party
# subprojects keep whatever they declare; their public headers are compiled by
# these C++26 targets, which is where the compatibility is verified.
function(lua_debug_apply_standards target)
    set_target_properties("${target}" PROPERTIES
        CXX_STANDARD "${LUA_DEBUG_CXX_STANDARD}"
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        C_STANDARD "${LUA_DEBUG_C_STANDARD}"
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
endfunction()

# Common compile settings for every target of this project.
function(lua_debug_apply_defaults target)
    lua_debug_apply_standards("${target}")
    set_target_properties("${target}" PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON)
    # Boost.Process v2 defaults to `boost::filesystem::path`; the project uses
    # `std::filesystem` (common/path_utf8.h) so there is exactly one path type.
    target_compile_definitions("${target}" PRIVATE BOOST_PROCESS_USE_STD_FS)
    if(UNIX AND NOT APPLE)
        # Strict `-std=c11` defines `__STRICT_ANSI__`, and glibc then hides the
        # POSIX/BSD declarations these sources use (sigaction, sigjmp_buf in the
        # frida headers, ...). C++ translation units get `_GNU_SOURCE` from
        # libstdc++'s own headers; C translation units do not, so the opt-in is
        # explicit here.
        target_compile_definitions("${target}" PRIVATE _DEFAULT_SOURCE)
    endif()

    if(MSVC)
        target_compile_options("${target}" PRIVATE /W3 /utf-8 /EHsc)
        # The Frida devkits use the static release CRT; match it in every
        # configuration instead of failing on _ITERATOR_DEBUG_LEVEL.
        target_compile_options("${target}" PRIVATE /MT)
    else()
        target_compile_options("${target}" PRIVATE -Wall)
    endif()
endfunction()

# Link-time trimming, for the executables that never look their own symbols up at
# runtime (the adapter and the injector helper).
#
# ld64 keeps every section of every archive member it loads: the frida-core
# devkit is one archive with ~80 MB of embedded resource blobs (agent dylibs,
# helper binaries, icons) that exist for frida's session/agent API, which the
# injector never touches. Global symbols survive for the linker, so the pieces a
# later `dlsym` needs stay reachable (`lua_*` in an interpreter, the module entry
# points) - interpreters and loadable modules deliberately do NOT get this.
function(lua_debug_apply_link_trim target)
    if(APPLE)
        target_link_options("${target}" PRIVATE "-Wl,-dead_strip" "-Wl,-dead_strip_dylibs")
        # Local symbols are only useful for a backtrace of this very binary; the
        # helper's own symbol table is ~10 MB.
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND strip -x -no_code_signature_warning "$<TARGET_FILE:${target}>"
            COMMENT "strip local symbols from ${target}"
            VERBATIM)
    elseif(NOT WIN32)
        target_compile_options("${target}" PRIVATE -ffunction-sections -fdata-sections)
        target_link_options("${target}" PRIVATE "-Wl,--gc-sections")
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND strip --strip-unneeded "$<TARGET_FILE:${target}>"
            COMMENT "strip ${target}"
            VERBATIM)
    endif()
endfunction()

# --- configure-time compile probe -------------------------------------------
set(_probe_flags
    "-DCMAKE_CXX_STANDARD=${LUA_DEBUG_CXX_STANDARD}"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF")
if(APPLE)
    list(APPEND _probe_flags "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()
# Boost/fmt headers only: the probe includes them, so their include directories
# (transitively, because Boost's targets expose one directory per library) have
# to reach the mini project. try_compile has no include option for the source
# signature, so they are passed as compiler flags.
function(_lua_debug_collect_includes target out_var)
    if(NOT TARGET "${target}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    get_target_property(includes "${target}" INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT includes)
        set(includes "")
    endif()
    # Boost exposes its per-library include directories through
    # `$<BUILD_INTERFACE:...>`, which cannot be used as a plain path.
    set(normalized "")
    foreach(entry IN LISTS includes)
        if(entry MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
            list(APPEND normalized "${CMAKE_MATCH_1}")
        elseif(entry MATCHES "\\$<")
            continue()
        else()
            list(APPEND normalized "${entry}")
        endif()
    endforeach()
    get_target_property(deps "${target}" INTERFACE_LINK_LIBRARIES)
    if(NOT deps)
        set(deps "")
    endif()
    set(collected ${normalized})
    foreach(dep IN LISTS deps)
        if(dep MATCHES "\\$<")
            continue()
        endif()
        if(NOT TARGET "${dep}")
            continue()
        endif()
        _lua_debug_collect_includes("${dep}" sub_includes)
        list(APPEND collected ${sub_includes})
    endforeach()
    if(collected)
        list(REMOVE_DUPLICATES collected)
    endif()
    set(${out_var} ${collected} PARENT_SCOPE)
endfunction()

_lua_debug_collect_includes(Boost::process _probe_boost_includes)
_lua_debug_collect_includes(fmt::fmt _probe_fmt_includes)
if(NOT _probe_boost_includes)
    message(FATAL_ERROR "${LUA_DEBUG_TOOLCHAIN_ERROR}\n\n--- the Boost/fmt include directories could not be resolved ---")
endif()

# try_compile() forwards the parent's CMAKE_CXX_FLAGS itself and a
# semicolon-separated list cannot travel in a -D value unchanged, so the include
# directories are published to a generated file the probe project includes.
set(_probe_includes_file "${CMAKE_BINARY_DIR}/_probe/probe_includes.cmake")
file(WRITE "${_probe_includes_file}"
    "set(PROBE_INCLUDE_DIRS\n${_probe_boost_includes}\n${_probe_fmt_includes}\n)\n")

try_compile(_lua_debug_cxx26_ok
    "${CMAKE_BINARY_DIR}/_probe/cxx26"
    "${CMAKE_CURRENT_LIST_DIR}/probes"
    lua_debug_cxx26_probe
    CMAKE_FLAGS ${_probe_flags} "-DPROBE_INCLUDES_FILE=${_probe_includes_file}"
    OUTPUT_VARIABLE _lua_debug_cxx26_output)

if(NOT _lua_debug_cxx26_ok)
    message(FATAL_ERROR "${LUA_DEBUG_TOOLCHAIN_ERROR}\n\n--- probe output ---\n${_lua_debug_cxx26_output}")
endif()
message(STATUS "lua-debug: C++26 coroutine toolchain accepted (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")

# --- build-time compile + link + run probe ----------------------------------
add_executable(luadebug_toolchain_probe "${CMAKE_CURRENT_LIST_DIR}/probes/cxx26_probe.cpp")
lua_debug_apply_defaults(luadebug_toolchain_probe)
target_link_libraries(luadebug_toolchain_probe PRIVATE
    Boost::asio Boost::process Boost::system fmt::fmt)
set_target_properties(luadebug_toolchain_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/bin")
if(APPLE AND LUA_DEBUG_TARGET_TRIPLE)
    target_compile_options(luadebug_toolchain_probe PRIVATE -target ${LUA_DEBUG_TARGET_TRIPLE})
    target_link_options(luadebug_toolchain_probe PRIVATE -target ${LUA_DEBUG_TARGET_TRIPLE})
endif()
add_test(NAME toolchain_probe COMMAND luadebug_toolchain_probe)
