# Toolchain file for a companion build (the second architecture of a
# multi-architecture distribution).
#
# `-DCMAKE_OSX_ARCHITECTURES` alone compiles for the other architecture but leaves
# CMAKE_SYSTEM_PROCESSOR at the *host* value, which some third-party projects (for
# example Boost.Context, through its assembly sources) use to choose code paths.
# Making the build a cross build by name states the target architecture once,
# where every dependency can see it.
#
# Companion builds never assemble the publish tree (LUA_DEBUG_PACKAGING=OFF); the
# parent build copies what it needs.

# This file is a template: Companion.cmake writes an instance with the values
# below substituted. A toolchain file cannot take its configuration from -D flags,
# because CMake's own try_compile children re-include it without them.
set(LUA_DEBUG_COMPANION_OS "@LUA_DEBUG_COMPANION_OS@")
set(LUA_DEBUG_COMPANION_ARCH "@LUA_DEBUG_COMPANION_ARCH@")

if(LUA_DEBUG_COMPANION_OS STREQUAL "darwin")
    set(CMAKE_SYSTEM_NAME Darwin)
    set(CMAKE_SYSTEM_PROCESSOR "${LUA_DEBUG_COMPANION_ARCH}")
    set(CMAKE_OSX_ARCHITECTURES "${LUA_DEBUG_COMPANION_ARCH}" CACHE STRING "Companion architecture" FORCE)
    # Cross builds run their host tools through this emulator; CMake only
    # substitutes a target path into a COMMAND when the variable is defined, and
    # the LuaJIT build script invokes its host tools by target name.
    if(NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL LUA_DEBUG_COMPANION_ARCH)
        if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
            set(CMAKE_CROSSCOMPILING_EMULATOR "/usr/bin/arch;-x86_64")
        else()
            set(CMAKE_CROSSCOMPILING_EMULATOR "/usr/bin/arch;-arm64")
        endif()
    endif()
elseif(LUA_DEBUG_COMPANION_OS STREQUAL "win32")
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_SYSTEM_PROCESSOR "${LUA_DEBUG_COMPANION_ARCH}")
    set(CMAKE_C_COMPILER_TARGET "${LUA_DEBUG_COMPANION_ARCH}-pc-windows-msvc")
    set(CMAKE_CXX_COMPILER_TARGET "${LUA_DEBUG_COMPANION_ARCH}-pc-windows-msvc")
else()
    message(FATAL_ERROR "companion toolchain: unsupported companion OS '${LUA_DEBUG_COMPANION_OS}'")
endif()
