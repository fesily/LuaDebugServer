# Companion build: the second architecture of a multi-architecture
# distribution.
#
#   macOS arm64 distribution -> companion darwin-x64 (runtime, bridges,
#                               interpreters, launcher, inject client)
#   Windows distribution      -> companion win32-x64 (launcher, runtime,
#                               interpreters, bridges)
#
# The companion is a separate CMake build tree with its own binary directory: no
# objects, generated headers or LuaJIT build tools are shared between
# architectures. It does not touch the publish directory (`LUA_DEBUG_PACKAGING`
# is off); the parent assembles the final tree, so a stale or half-written
# companion build can never delete or overwrite published files.

include_guard(GLOBAL)

if(NOT LUA_DEBUG_COMPANION_PLATFORM OR NOT LUA_DEBUG_BUILD_COMPANION OR NOT LUA_DEBUG_PACKAGING)
    return()
endif()

set(_companion_bin "${CMAKE_BINARY_DIR}/companion")

set(_companion_args
    -S "${CMAKE_SOURCE_DIR}"
    -B "${_companion_bin}"
    -G "${CMAKE_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DLUA_DEBUG_DIST_PLATFORM=${LUA_DEBUG_DIST_PLATFORM}"
    "-DLUA_DEBUG_PLATFORM=${LUA_DEBUG_COMPANION_PLATFORM}"
    "-DLUA_DEBUG_PACKAGING=OFF"
    "-DLUA_DEBUG_BUILD_COMPANION=OFF"
    "-DLUA_DEBUG_BUILD_TESTS=OFF"
    "-DLUA_DEBUG_DEP_CACHE=${LUA_DEBUG_DEP_CACHE}"
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")

# The publish platform name is not the compiler's architecture spelling.
_lua_debug_split_platform("${LUA_DEBUG_COMPANION_PLATFORM}" _companion_os _companion_arch)

# A toolchain file, not bare -D flags: it also states the target processor, which
# third-party code (Boost.Context's assembly, for one) keys its sources on.
set(_companion_toolchain "${CMAKE_BINARY_DIR}/companion_toolchain.cmake")
set(LUA_DEBUG_COMPANION_OS "${_companion_os}")
set(LUA_DEBUG_COMPANION_ARCH "${_companion_arch}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/CompanionToolchain.cmake" "${_companion_toolchain}" @ONLY)
list(APPEND _companion_args
    "-DCMAKE_TOOLCHAIN_FILE=${_companion_toolchain}")
if(WIN32)
    list(APPEND _companion_args
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()

add_custom_target(companion_build ALL
    COMMAND ${CMAKE_COMMAND} ${_companion_args}
    COMMAND ${CMAKE_COMMAND} --build "${_companion_bin}" --config "$<CONFIG>"
    COMMENT "companion build (${LUA_DEBUG_COMPANION_PLATFORM})"
    VERBATIM)

# --- assembly ---------------------------------------------------------------
#
# Everything the companion produced is copied into the publish tree here, once,
# by the parent build.
set(_companion_publish_commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_companion_bin}/bin/lua-debug-injector-x64"
        "${LUA_DEBUG_PUBLISH_DIR}/bin/lua-debug-injector-x64")

if(WIN32)
    set(_companion_runtime_name "luadebug_runtime.dll")
    set(_companion_bridge_suffix "dll")
else()
    set(_companion_runtime_name "libluadebug_runtime.dylib")
    set(_companion_bridge_suffix "so")
endif()

set(_companion_tail
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_companion_bin}/lib/${_companion_runtime_name}"
        "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}/${_companion_runtime_name}")
# LuaJIT is not in LUA_DEBUG_LUA_VERSIONS (it is not a LuaTargets entry; its
# interpreter comes from LuaJIT.cmake), but the companion builds one per
# architecture just like the plain interpreters.
foreach(_version IN LISTS LUA_DEBUG_LUA_VERSIONS ITEMS luajit)
    if(WIN32)
        list(APPEND _companion_tail
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}/${_version}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_companion_bin}/bin/${_version}/lua"
                "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}/${_version}/lua.exe")
    else()
        list(APPEND _companion_tail
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}/${_version}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_companion_bin}/bin/${_version}/lua"
                "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}/${_version}/lua")
    endif()
endforeach()

# The bridge is one file per platform/arch, not one per version.
list(APPEND _companion_tail
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_companion_bin}/lib/luadebug.${_companion_bridge_suffix}"
        "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_COMPANION_PLATFORM}/luadebug.${_companion_bridge_suffix}")

add_custom_target(companion_publish ALL
    ${_companion_publish_commands}
    ${_companion_tail}
    DEPENDS companion_build
    COMMENT "assemble companion artifacts into publish"
    VERBATIM)
