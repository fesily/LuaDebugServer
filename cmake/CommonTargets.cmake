# Shared target helpers.
#
# Every target of this project goes through lua_debug_apply_defaults(), so the
# language standard, visibility, PIC and the compiler warnings cannot drift
# between targets.

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/ToolchainChecks.cmake)

set(LUA_DEBUG_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
set(LUA_DEBUG_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/3rd")
set(LUA_DEBUG_BINARY_DIR "${CMAKE_BINARY_DIR}")

# Loadable modules (`luadebug`, `launcher`, test interpreters) carry no `lib`
# prefix, and use `.so` on macOS: CMake would otherwise emit a `.dylib`, which the
# extension's loader path does not look for.
if(WIN32)
    set(LUA_DEBUG_MODULE_PREFIX "")
    set(LUA_DEBUG_MODULE_SUFFIX ".dll")
else()
    set(LUA_DEBUG_MODULE_PREFIX "")
    set(LUA_DEBUG_MODULE_SUFFIX ".so")
endif()

set(LUA_DEBUG_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(LUA_DEBUG_TOOLS_DIR "${CMAKE_BINARY_DIR}/tools")
file(MAKE_DIRECTORY "${LUA_DEBUG_GENERATED_DIR}" "${LUA_DEBUG_TOOLS_DIR}")

# Third-party include directories shared by the sources that need them.
#
# `3rd` itself exposes the vendored nlohmann/json (<nlohmann/json.hpp>) and
# `3rd/lua` the Lua patch header (luai_devent.h). The platform layer (sockets,
# subprocess, formatting) is Boost and {fmt} now, so there is nothing else to
# add here.
function(lua_debug_third_party_includes out_var)
    set(${out_var}
        "${LUA_DEBUG_THIRD_PARTY_DIR}"
        "${LUA_DEBUG_THIRD_PARTY_DIR}/lua"
        PARENT_SCOPE)
endfunction()

# Runtime output locations: build artifacts stay inside the build tree, and only
# the packaging step writes into LUA_DEBUG_PUBLISH_DIR.
set(LUA_DEBUG_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(LUA_DEBUG_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${LUA_DEBUG_BIN_DIR}" "${LUA_DEBUG_LIB_DIR}")

# lua_debug_add_library(<target> SOURCES <files...> [INCLUDES <dirs...>]
#                       [DEFINES <defs...>] [LINKS <libs...>] [OUTPUT_NAME <name>]
#                       [NO_PREFIX] [NO_DEFAULT_SUFFIX])
function(lua_debug_add_library target)
    cmake_parse_arguments(ARG "" "OUTPUT_NAME;NO_PREFIX;NO_DEFAULT_SUFFIX" "SOURCES;INCLUDES;DEFINES;LINKS" ${ARGN})
    add_library("${target}" SHARED ${ARG_SOURCES})
    lua_debug_apply_defaults("${target}")
    target_include_directories("${target}" PRIVATE ${ARG_INCLUDES})
    if(ARG_DEFINES)
        target_compile_definitions("${target}" PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_LINKS)
        target_link_libraries("${target}" PRIVATE ${ARG_LINKS})
    endif()
    set_target_properties("${target}" PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_BIN_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}")
    if(ARG_OUTPUT_NAME)
        set_target_properties("${target}" PROPERTIES OUTPUT_NAME "${ARG_OUTPUT_NAME}")
    endif()
    if(ARG_NO_PREFIX)
        set_target_properties("${target}" PROPERTIES PREFIX "")
    endif()
    if(ARG_NO_DEFAULT_SUFFIX)
        set_target_properties("${target}" PROPERTIES SUFFIX "")
    endif()
endfunction()

# lua_debug_add_executable(<target> SOURCES <files...> [INCLUDES ...] [DEFINES ...] [LINKS ...] [OUTPUT_NAME ...])
function(lua_debug_add_executable target)
    cmake_parse_arguments(ARG "" "OUTPUT_NAME" "SOURCES;INCLUDES;DEFINES;LINKS" ${ARGN})
    add_executable("${target}" ${ARG_SOURCES})
    lua_debug_apply_defaults("${target}")
    target_include_directories("${target}" PRIVATE ${ARG_INCLUDES})
    if(ARG_DEFINES)
        target_compile_definitions("${target}" PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_LINKS)
        target_link_libraries("${target}" PRIVATE ${ARG_LINKS})
    endif()
    set_target_properties("${target}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_BIN_DIR}")
    if(ARG_OUTPUT_NAME)
        set_target_properties("${target}" PROPERTIES OUTPUT_NAME "${ARG_OUTPUT_NAME}")
    endif()
endfunction()

# Applies the macOS minimum system version explicitly: CMake would otherwise
# apply CMAKE_OSX_DEPLOYMENT_TARGET, but the target triple also has to reach the
# compile and link lines of targets that link prebuilt static archives (Frida).
function(lua_debug_apply_triple target)
    if(APPLE AND LUA_DEBUG_TARGET_TRIPLE)
        target_compile_options("${target}" PRIVATE -target "${LUA_DEBUG_TARGET_TRIPLE}")
        target_link_options("${target}" PRIVATE -target "${LUA_DEBUG_TARGET_TRIPLE}")
    endif()
endfunction()

# Installs a built artifact into the publish tree (assembled only by the
# top-level build tree, see Packaging.cmake).
function(lua_debug_publish target destination)
    if(NOT LUA_DEBUG_PACKAGING)
        return()
    endif()
    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LUA_DEBUG_PUBLISH_DIR}/${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${target}>" "${LUA_DEBUG_PUBLISH_DIR}/${destination}"
        COMMENT "publish ${target} -> ${destination}"
        VERBATIM)
endfunction()
