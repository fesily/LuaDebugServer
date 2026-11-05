# The target-side bridge: one shared library per platform/arch that serves every
# supported Lua through per-profile layout backends.
#
#   profile   family   compiled against
#   lua51..55 lua5x    3rd/lua/<profile>
#   luajit    luajit   3rd/lua/luajit (v2.1)
#
# Each backend is the same small set of sources compiled with `LDB_NS` (its C++
# namespace) and `LDB_PROFILE` (the profile enum), so all of them can live in one
# DSO without symbol collisions; `luaopen_luadebug_<profile>` then captures the
# calling module, which is what lets a process hosting several Lua ABIs bind the
# right one.
#
# The version-neutral half (worker, hook, target, vm, json/log) compiles with no
# Lua include path at all.

include_guard(GLOBAL)

lua_debug_third_party_includes(_third_party)

# Profiles that have a backend. `lua-latest` shares the lua55 ABI and is an
# interpreter-only entry (`debugger.lua` maps it to the lua55 backend).
set(LUA_DEBUG_BRIDGE_PROFILES lua51 lua52 lua53 lua54 lua55 luajit)

# --- version-neutral core ----------------------------------------------------
file(GLOB_RECURSE _bridge_core_sources CONFIGURE_DEPENDS
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/thunk/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/util/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/bridge/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/target/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/hook/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/worker/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/vm/*.cpp")
# The backends are compiled per profile below, never as part of the core.
list(FILTER _bridge_core_sources EXCLUDE REGEX "/src/luadebug/vm/backends/")
list(SORT _bridge_core_sources)

add_library(luadebug_bridge_core OBJECT
    ${_bridge_core_sources}
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/base64.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/json.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/vm_version.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/log.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/host/loader.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/host/platform_abi.cpp")
lua_debug_apply_defaults(luadebug_bridge_core)
target_include_directories(luadebug_bridge_core PRIVATE
    "${LUA_DEBUG_SOURCE_DIR}/src/luadebug"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger"
    ${_third_party})
target_link_libraries(luadebug_bridge_core PRIVATE
    Boost::unordered fmt::fmt)
lua_debug_apply_triple(luadebug_bridge_core)

# --- one object library per profile -----------------------------------------
function(lua_debug_add_bridge_backend profile)
    if(profile STREQUAL "luajit")
        set(family "luajit")
        set(lua_include_dirs ${LUA_DEBUG_LUAJIT_INCLUDE_DIRS})
        set(extra_dependency luajit_headers)
    else()
        set(family "lua5x")
        set(lua_include_dirs "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${profile}")
        set(extra_dependency "")
    endif()
    file(GLOB backend_sources CONFIGURE_DEPENDS
        "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/vm/backends/${family}/*.cpp")

    set(target "luadebug_backend_${profile}")
    add_library("${target}" OBJECT ${backend_sources})
    lua_debug_apply_defaults("${target}")
    target_include_directories("${target}" PRIVATE
        "${LUA_DEBUG_SOURCE_DIR}/src/luadebug"
        ${lua_include_dirs})
    target_compile_definitions("${target}" PRIVATE
        "LDB_NS=${profile}"
        "LDB_PROFILE=${profile}")
    if(extra_dependency)
        # Orders the backend after the generated LuaJIT `luajit.h`.
        add_dependencies("${target}" ${extra_dependency})
    endif()
    lua_debug_apply_triple("${target}")
endfunction()

foreach(_profile IN LISTS LUA_DEBUG_BRIDGE_PROFILES)
    lua_debug_add_bridge_backend("${_profile}")
endforeach()

# --- the single bridge library ----------------------------------------------
set(_bridge_objects $<TARGET_OBJECTS:luadebug_bridge_core>)
foreach(_profile IN LISTS LUA_DEBUG_BRIDGE_PROFILES)
    list(APPEND _bridge_objects $<TARGET_OBJECTS:luadebug_backend_${_profile}>)
endforeach()

lua_debug_add_library(luadebug
    SOURCES ${_bridge_objects}
    INCLUDES
        "${LUA_DEBUG_SOURCE_DIR}/src/luadebug"
        "${LUA_DEBUG_SOURCE_DIR}/src/debugger"
        ${_third_party}
    OUTPUT_NAME luadebug)
set_target_properties(luadebug PROPERTIES
    PREFIX "${LUA_DEBUG_MODULE_PREFIX}"
    SUFFIX "${LUA_DEBUG_MODULE_SUFFIX}")
target_link_libraries(luadebug PRIVATE
    Boost::unordered fmt::fmt)

if(WIN32)
    target_compile_definitions(luadebug PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0602)
    target_link_libraries(luadebug PRIVATE
        version ws2_32 user32 shell32 ole32 dbghelp ntdll synchronization)
elseif(APPLE)
    # Lua symbols and the runtime are provided by the host process.
    target_link_options(luadebug PRIVATE "-undefined" "dynamic_lookup")
    target_link_libraries(luadebug PRIVATE "-framework Foundation")
else()
    target_link_libraries(luadebug PRIVATE pthread)
    # Static C++ runtime: the bridge is dlopened into a process whose libstdc++
    # version is unknown.
    target_link_options(luadebug PRIVATE "-static-libstdc++" "-static-libgcc")
endif()
lua_debug_apply_triple(luadebug)

# --- publish -----------------------------------------------------------------
#
# The bridge is not per version: `runtime/<platform>/luadebug.so` next to the
# runtime library, exactly where `debugger.lua` looks for it.
lua_debug_publish(luadebug "runtime/${LUA_DEBUG_RUNTIME_PLATFORM}")
