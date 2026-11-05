# Publishing: manifest, extension surface, runtime tree and the vcredist note.
#
# The publish directory is assembled only by the top-level build tree; companion
# build trees pass LUA_DEBUG_PACKAGING=OFF and the parent copies their artifacts
# (see Companion.cmake). Nothing here runs `npm`/`vsce`: packaging the VSIX is a
# separate, explicit step.

include_guard(GLOBAL)

if(NOT LUA_DEBUG_PACKAGING)
    return()
endif()

set(LUA_DEBUG_PYTHON "" CACHE FILEPATH "Python 3 interpreter used for the packaging steps")
if(NOT LUA_DEBUG_PYTHON)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(LUA_DEBUG_PYTHON "${Python3_EXECUTABLE}")
endif()

set(_manifest "${LUA_DEBUG_GENERATED_DIR}/package.json")

add_custom_command(
    OUTPUT "${_manifest}"
    COMMAND "${LUA_DEBUG_PYTHON}" "${LUA_DEBUG_SOURCE_DIR}/tools/gen_package_json.py"
        --platform "${LUA_DEBUG_DIST_PLATFORM}"
        --output "${_manifest}"
    DEPENDS
        "${LUA_DEBUG_SOURCE_DIR}/tools/gen_package_json.py"
    COMMENT "generate package.json for ${LUA_DEBUG_DIST_PLATFORM}"
    VERBATIM)

add_custom_target(package_manifest DEPENDS "${_manifest}")

add_custom_target(publish_extension ALL
    COMMAND "${LUA_DEBUG_PYTHON}" "${LUA_DEBUG_SOURCE_DIR}/tools/assemble_extension.py"
        --source-root "${LUA_DEBUG_SOURCE_DIR}"
        --publish-dir "${LUA_DEBUG_PUBLISH_DIR}"
        --manifest "${_manifest}"
        --platform "${LUA_DEBUG_DIST_PLATFORM}"
    DEPENDS package_manifest
    COMMENT "assemble publish (extension, manifest, LICENSE, README)"
    VERBATIM)

# Everything the distribution ships. The per-target publish steps are POST_BUILD
# commands on the targets themselves, so depending on them here is enough.
set(_publish_dependencies
    publish_extension
    lua-debug
    luadebug_runtime
    launcher)
foreach(_version IN LISTS LUA_DEBUG_LUA_VERSIONS)
    list(APPEND _publish_dependencies "lua_${_version}")
endforeach()
# One bridge per platform/arch serves every Lua profile.
list(APPEND _publish_dependencies luadebug luajit_publish)
if(APPLE)
    list(APPEND _publish_dependencies injector_helper)
endif()
if(LUA_DEBUG_COMPANION_PLATFORM AND LUA_DEBUG_BUILD_COMPANION)
    list(APPEND _publish_dependencies companion_publish)
endif()

add_custom_target(publish ALL DEPENDS ${_publish_dependencies})

# --- Windows runtime binaries ------------------------------------------------
#
# Everything is built with the static release CRT (/MT), including the Frida
# devkits it links against, so the distribution has no VC++ redistributable
# dependency. The old build copied the runtime DLLs because part of the tree was
# /MD; that is no longer the case, and shipping an unused vcredist directory
# would misrepresent what the extension needs.
if(WIN32)
    message(STATUS "lua-debug: static CRT (/MT) everywhere; no vcredist directory is published")
endif()
