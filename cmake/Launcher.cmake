# The launcher: a shared library preloaded into (or injected into) the debuggee to
# bootstrap the runtime before any user code runs.
#
# It uses frida-gum to install hooks and watches; frida-core is never linked here.
# `luajit_listener.cpp` is compiled separately so a build that does not need the
# LuaJIT listener cannot accidentally pick it up twice.

include_guard(GLOBAL)

lua_debug_add_gumpp()

lua_debug_third_party_includes(_third_party)

file(GLOB _launcher_sources CONFIGURE_DEPENDS
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher/autoattach/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher/hook/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher/resolver/*.cpp"
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher/util/*.cpp")
list(FILTER _launcher_sources EXCLUDE REGEX "/luajit_listener\\.cpp$")
list(SORT _launcher_sources)

# The LuaJIT listener needs the LuaJIT headers; it is the only launcher source
# that does.
add_library(launcher_hook_luajit OBJECT
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher/hook/luajit_listener.cpp")
lua_debug_apply_defaults(launcher_hook_luajit)
target_include_directories(launcher_hook_luajit PRIVATE
    ${LUA_DEBUG_LUAJIT_INCLUDE_DIRS}
    "${LUA_DEBUG_SOURCE_DIR}/src/launcher"
    ${_third_party})
target_link_libraries(launcher_hook_luajit PRIVATE gumpp luajit_headers)
lua_debug_apply_triple(launcher_hook_luajit)

if(WIN32)
    set(_launcher_output "launcher.${LUA_DEBUG_ARCH}")
    if(LUA_DEBUG_ARCH STREQUAL "x86")
        set(_launcher_output "launcher.x86")
    elseif(LUA_DEBUG_ARCH STREQUAL "x86_64")
        set(_launcher_output "launcher.x64")
    endif()
else()
    set(_launcher_output "launcher")
endif()

lua_debug_add_library(launcher
    SOURCES
        ${_launcher_sources}
        $<TARGET_OBJECTS:launcher_hook_luajit>
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/bootstrap.cpp
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/json.cpp
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/path_utf8.cpp
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/vm_source.cpp
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/vm_version.cpp
        ${LUA_DEBUG_SOURCE_DIR}/src/luadebug/vm/mapping.cpp
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger/host/platform_abi.cpp
    INCLUDES
        ${LUA_DEBUG_SOURCE_DIR}/src/launcher
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger
        ${LUA_DEBUG_SOURCE_DIR}/src/luadebug
        ${LUA_DEBUG_SOURCE_DIR}/src
        ${LUA_DEBUG_THIRD_PARTY_DIR}/lua/lua55
        ${_third_party}
    OUTPUT_NAME "${_launcher_output}")
if(NOT WIN32)
    # A loadable module, not a dylib: the extension preloads `<root>/bin/launcher.so`.
    set_target_properties(launcher PROPERTIES
        PREFIX "${LUA_DEBUG_MODULE_PREFIX}"
        SUFFIX "${LUA_DEBUG_MODULE_SUFFIX}")
endif()

# The launcher is loaded into a process that does not export Lua; every Lua entry
# point goes through its own resolver.
target_link_libraries(launcher PRIVATE gumpp fmt::fmt)
# No Boost.Filesystem anywhere in the project: paths are `std::filesystem`
# (common/path_utf8.h). That also keeps this DSO free of Boost.Filesystem's static
# initializer (codecvt facet registration), which allocates during module init.
target_link_libraries(launcher PRIVATE Boost::unordered Boost::system)

if(WIN32)
    target_compile_definitions(launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(launcher PRIVATE ws2_32 user32 shell32 ole32 delayimp ntdll Version)
elseif(APPLE)
    target_link_options(launcher PRIVATE "-undefined" "dynamic_lookup")
else()
    # No Lua, no libstdc++ from the host: the launcher resolves everything through
    # its own loader.
    target_link_options(launcher PRIVATE "-static-libstdc++" "-static-libgcc")
endif()
lua_debug_apply_triple(launcher)

if(LUA_DEBUG_PACKAGING)
    if(APPLE AND LUA_DEBUG_COMPANION_PLATFORM AND LUA_DEBUG_BUILD_COMPANION)
        # Two slices, one published file: the arm64 build lipos its own launcher
        # together with the companion's.
        add_custom_target(launcher_fat ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${LUA_DEBUG_PUBLISH_DIR}/bin"
            COMMAND lipo -create -output
                "${LUA_DEBUG_PUBLISH_DIR}/bin/launcher.so"
                "$<TARGET_FILE:launcher>"
                "${CMAKE_BINARY_DIR}/companion/lib/launcher.so"
            DEPENDS launcher companion_build
            COMMENT "lipo launcher.so (arm64 + x64)"
            VERBATIM)
    else()
        lua_debug_publish(launcher "bin")
    endif()
endif()
