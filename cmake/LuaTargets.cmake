# Bundled PUC Lua interpreters.
#
# Each version produces:
#   * `lua_<ver>_core` - OBJECT library of the interpreter core (no lua.c),
#   * POSIX: `lua_<ver>` - the `lua` executable published as
#     `runtime/<platform>/<ver>/lua`. It exports the Lua API, because a bridge
#     resolves `lua_*` from the host process through the flat namespace.
#   * Windows: `lua_<ver>_dll` - `<ver>.dll`, plus `lua_<ver>` linking it.
#
# The core sources are the upstream directory contents, which is exactly what the
# previous recipe compiled (`*.c` minus `lua.c`/`luac.c`); the exclusions are
# explicit rather than a glob over this project's own tree.

include_guard(GLOBAL)

set(LUA_DEBUG_LUA_VERSIONS lua51 lua52 lua53 lua54 lua55 lua-latest)

# Per-version define set, mirroring the previous recipe.
function(_lua_debug_version_defines version out_var)
    set(defines "")
    if(version STREQUAL "lua-latest")
        list(APPEND defines LUA_VERSION_LATEST)
    endif()
    if(version STREQUAL "lua51" OR version STREQUAL "lua52")
        if(NOT WIN32)
            list(APPEND defines _XOPEN_SOURCE=600)
        endif()
    endif()
    if(APPLE)
        list(APPEND defines LUA_USE_MACOSX)
        if(version STREQUAL "lua51")
            list(APPEND defines LUA_USE_DLOPEN)
        endif()
    elseif(UNIX)
        list(APPEND defines LUA_USE_LINUX)
    endif()
    if(WIN32)
        list(APPEND defines _CRT_SECURE_NO_WARNINGS)
    endif()
    set(${out_var} ${defines} PARENT_SCOPE)
endfunction()

function(_lua_debug_lua_core_sources version out_var)
    set(dir "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${version}")
    file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.c")
    list(FILTER sources EXCLUDE REGEX "/(lua|luac)\\.c$")
    list(SORT sources)
    set(${out_var} ${sources} PARENT_SCOPE)
endfunction()

# Builds the core object library for one version: `lua_<ver>_core`.
function(lua_debug_add_lua_core version)
    _lua_debug_lua_core_sources("${version}" sources)
    _lua_debug_version_defines("${version}" defines)
    set(dir "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${version}")

    add_library("lua_${version}_core" OBJECT ${sources})
    lua_debug_apply_standards("lua_${version}_core")
    set_target_properties("lua_${version}_core" PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        # The interpreter is the host of a bridge: its Lua API must stay visible.
        C_VISIBILITY_PRESET default)
    target_include_directories("lua_${version}_core" PRIVATE
        "${dir}"
        "${LUA_DEBUG_THIRD_PARTY_DIR}/lua")
    target_compile_definitions("lua_${version}_core" PRIVATE ${defines})
    lua_debug_apply_triple("lua_${version}_core")
endfunction()

# Publishes `<version>/lua` (POSIX) or `<version>/lua.exe` + `<version>.dll`.
function(lua_debug_add_interpreter version)
    _lua_debug_version_defines("${version}" defines)
    set(dir "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${version}")

    if(WIN32)
        add_library("lua_${version}_dll" SHARED $<TARGET_OBJECTS:lua_${version}_core>)
        lua_debug_apply_standards("lua_${version}_dll")
        lua_debug_apply_defaults("lua_${version}_dll")
        set_target_properties("lua_${version}_dll" PROPERTIES
            OUTPUT_NAME "${version}"
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}")
        # LUA_BUILD_AS_DLL marks the API as exported by the DLL.
        target_compile_definitions("lua_${version}_dll" PRIVATE LUA_BUILD_AS_DLL ${defines})

        lua_debug_add_executable("lua_${version}"
            SOURCES "${dir}/lua.c"
            INCLUDES "${LUA_DEBUG_THIRD_PARTY_DIR}/lua"
            DEFINES ${defines}
            LINKS "lua_${version}_dll")
        set_target_properties("lua_${version}" PROPERTIES
            OUTPUT_NAME lua
            RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_BIN_DIR}/${version}")
        lua_debug_apply_triple("lua_${version}")
        if(LUA_DEBUG_PACKAGING)
            add_custom_command(TARGET "lua_${version}" POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_RUNTIME_PLATFORM}/${version}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:lua_${version}>"
                    "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_RUNTIME_PLATFORM}/${version}/lua.exe"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:lua_${version}_dll>"
                    "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_RUNTIME_PLATFORM}/${version}/${version}.dll"
                COMMENT "publish interpreter ${version}"
                VERBATIM)
        endif()
    else()
        lua_debug_add_executable("lua_${version}"
            SOURCES "${dir}/lua.c"
            INCLUDES "${dir}" "${LUA_DEBUG_THIRD_PARTY_DIR}/lua"
            DEFINES ${defines}
            LINKS "lua_${version}_core")
        # Every interpreter is called `lua`, so they cannot share one directory.
        set_target_properties("lua_${version}" PROPERTIES
            OUTPUT_NAME lua
            RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_BIN_DIR}/${version}")
        target_link_libraries("lua_${version}" PRIVATE m)
        if(APPLE)
            # Keep the interpreter's symbols available to a loaded bridge.
            set_target_properties("lua_${version}" PROPERTIES ENABLE_EXPORTS ON)
            # The launcher and the bridge are C++ and are mapped into this
            # process (preload, or frida for attach). frida maps the image itself
            # instead of asking dyld, so the initializers of its dependencies do
            # not run: libc++ has to be loaded and initialized by the host before
            # that, or the first `operator new` in the launcher aborts. The
            # luamake link step used clang++ and pulled libc++ in the same way.
            # `-lc++` alone is dropped: nothing in this C binary references it, and
            # ld64 strips unused dylibs; `-needed-lc++` pins the load command,
            # which is the whole point.
            target_link_options("lua_${version}" PRIVATE "-Wl,-needed-lc++")
        else()
            target_link_libraries("lua_${version}" PRIVATE pthread dl)
            target_link_options("lua_${version}" PRIVATE -Wl,-E)
        endif()
        lua_debug_apply_triple("lua_${version}")
        if(LUA_DEBUG_PACKAGING)
            add_custom_command(TARGET "lua_${version}" POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_RUNTIME_PLATFORM}/${version}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:lua_${version}>"
                    "${LUA_DEBUG_PUBLISH_DIR}/runtime/${LUA_DEBUG_RUNTIME_PLATFORM}/${version}/lua"
                COMMENT "publish interpreter ${version}"
                VERBATIM)
        endif()
    endif()
endfunction()

foreach(_version IN LISTS LUA_DEBUG_LUA_VERSIONS)
    lua_debug_add_lua_core("${_version}")
    lua_debug_add_interpreter("${_version}")
endforeach()

# Test-only shared Lua per version: a bridge's Lua symbols come from the host
# process, so loading a bridge in-process requires the matching interpreter as a
# shared library. This is also what the mixed-VM host and the loader checks use.
function(lua_debug_add_test_lua version)
    _lua_debug_version_defines("${version}" defines)
    set(dir "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${version}")

    set(name "test_lua_${version}")
    add_library("${name}" SHARED $<TARGET_OBJECTS:lua_${version}_core>)
    lua_debug_apply_standards("${name}")
    lua_debug_apply_defaults("${name}")
    set_target_properties("${name}" PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
        SUFFIX "${LUA_DEBUG_MODULE_SUFFIX}"
        C_VISIBILITY_PRESET default
        LIBRARY_OUTPUT_DIRECTORY "${LUA_DEBUG_TEST_BIN_DIR}")
    target_include_directories("${name}" PRIVATE
        "${dir}"
        "${LUA_DEBUG_THIRD_PARTY_DIR}/lua")
    if(APPLE)
        target_link_libraries("${name}" PRIVATE m)
    elseif(UNIX)
        target_link_libraries("${name}" PRIVATE m dl)
    endif()
    lua_debug_apply_triple("${name}")
endfunction()
