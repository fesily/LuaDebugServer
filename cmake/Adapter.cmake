# The adapter executable (`publish/bin/lua-debug`).
#
# It owns the DAP connection to VS Code, launches or attaches to the debuggee and
# proxies DAP frames to the target-side session. It never links Lua: the only
# process that runs Lua is the debuggee.
#
# macOS injects through a separate per-architecture helper
# (`bin/lua-debug-injector-<arch>`), so an arm64 adapter can still attach to an
# x86_64 process; Linux and Windows link frida-core into the adapter itself
# (Linux preloads the launcher, Windows needs frida's gated spawn).

include_guard(GLOBAL)

file(GLOB _adapter_sources CONFIGURE_DEPENDS
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/frontend/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/host/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/debugger/transport/*.cpp")
list(SORT _adapter_sources)

lua_debug_third_party_includes(_third_party)

set(_adapter_sources_extra "")
set(_adapter_links
    Boost::asio Boost::process Boost::unordered Boost::system
    fmt::fmt)

if(NOT APPLE)
    lua_debug_add_frida_core(_frida_core_target)
    list(APPEND _adapter_sources_extra "${LUA_DEBUG_SOURCE_DIR}/src/injector/frida_inject.cpp")
    list(APPEND _adapter_links ${_frida_core_target})
endif()

if(WIN32)
    list(APPEND _adapter_sources_extra "${LUA_DEBUG_SOURCE_DIR}/assets/windows/lua-debug.rc")
endif()

lua_debug_add_executable(lua-debug
    SOURCES ${_adapter_sources} ${_adapter_sources_extra}
    INCLUDES
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger
        ${LUA_DEBUG_SOURCE_DIR}/src
        ${_third_party}
    LINKS ${_adapter_links})


if(WIN32)
    target_compile_definitions(lua-debug PRIVATE _CRT_SECURE_NO_WARNINGS)
    # The .rc file references the icon by relative path.
    target_include_directories(lua-debug PRIVATE "${LUA_DEBUG_SOURCE_DIR}/assets/windows")
elseif(APPLE)
    # Nothing beyond libc++: frida-core is not part of the adapter on macOS.
else()
    # frida-core is linked in-process and needs the executable's symbols exported.
    target_link_options(lua-debug PRIVATE "-rdynamic" "-static-libstdc++" "-static-libgcc")
endif()
lua_debug_apply_triple(lua-debug)
lua_debug_apply_link_trim(lua-debug)

if(LUA_DEBUG_PACKAGING)
    if(WIN32)
        lua_debug_publish(lua-debug "bin")
    else()
        lua_debug_publish(lua-debug "bin")
    endif()
endif()

# --- macOS injector helper ---------------------------------------------------
#
# One helper per target architecture (`bin/lua-debug-injector-<arch>`): the
# adapter runs `/usr/bin/arch -x86_64` for the Intel one under Rosetta, so both
# are needed in an arm64 distribution. The helper hosts the frida backend; the
# lldb backend drives the system lldb and needs no helper.
if(APPLE)
    lua_debug_add_frida_core(_frida_core_target)
    get_property(_cli_arch GLOBAL PROPERTY LUA_DEBUG_FRIDA_CLIENT_ARCH)
    set(_suffix "${LUA_DEBUG_ARCH}")
    if(_suffix STREQUAL "x86_64")
        set(_suffix "x64")
    endif()

    lua_debug_add_executable("injector_helper"
        SOURCES
            ${LUA_DEBUG_SOURCE_DIR}/src/injector/main.cpp
            ${LUA_DEBUG_SOURCE_DIR}/src/injector/frida_inject.cpp
        INCLUDES
            ${LUA_DEBUG_SOURCE_DIR}/src/injector
        LINKS ${_frida_core_target}
        OUTPUT_NAME "lua-debug-injector-${_suffix}")
    lua_debug_apply_triple(injector_helper)
    lua_debug_apply_link_trim(injector_helper)
    if(LUA_DEBUG_PACKAGING)
        lua_debug_publish(injector_helper "bin")
    endif()
endif()
