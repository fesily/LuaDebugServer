# Frida devkits: frida-gum (launcher hooks) and frida-core (process control and
# injection).
#
# Both come from the same pinned release, are unpacked into
# `3rd/frida_<component>/<os>-<arch>` (the layout the previous script used, so an
# already populated tree keeps working offline) and are linked statically: the
# shipped binaries must not require Frida shared objects on the target machine.
#
# A devkit directory is only accepted when it carries the pinned version stamp and
# every expected artifact, so a stale header from an older release can never be
# mistaken for a complete download.

include_guard(GLOBAL)

set(LUA_DEBUG_FRIDA_VERSION "17.25.2")

# SHA256 of every devkit archive this build can use. An archive that is not listed
# here is refused instead of being fetched unverified.
set(LUA_DEBUG_FRIDA_SHA256
    "frida-gum-devkit-${LUA_DEBUG_FRIDA_VERSION}-macos-arm64.tar.xz=bceae50800ad0badf68992f65f9ea0a8ce7cb70d41be63a6bbe67b46a9670119"
    "frida-gum-devkit-${LUA_DEBUG_FRIDA_VERSION}-macos-x86_64.tar.xz=c1141c056a4db2a27b48464b1685a1ce8fc288bf3c0454461d315938480d2d1a"
    "frida-gum-devkit-${LUA_DEBUG_FRIDA_VERSION}-linux-x86_64.tar.xz=8855031067f6b0f6a5e38bf9357639ee7e29dcaead606c9ec6f2703a246fc737"
    "frida-gum-devkit-${LUA_DEBUG_FRIDA_VERSION}-linux-arm64.tar.xz=a2c00b433feb066060dd87c6fa86cbfa50eeac660f3c889af32177b6daa1f40f"
    "frida-gum-devkit-${LUA_DEBUG_FRIDA_VERSION}-windows-x86.tar.xz=f04ba3f2f049853f649877a91f288141e331e989a6f7009e046d4bc949967e61"
    "frida-gum-devkit-${LUA_DEBUG_FRIDA_VERSION}-windows-x86_64.tar.xz=958b4e83dc5ae034f87a425be6c60f89f2d84743f8b276dc2aeb6584934df7e1"
    "frida-core-devkit-${LUA_DEBUG_FRIDA_VERSION}-macos-arm64.tar.xz=b91a1dc1a59857d0d23cab91792455122187765bea0d0bd72f48355651666353"
    "frida-core-devkit-${LUA_DEBUG_FRIDA_VERSION}-macos-x86_64.tar.xz=da0b4500447d9a0ea4cf68c6818d64ff24430421196f99807360a42e41acdeb6"
    "frida-core-devkit-${LUA_DEBUG_FRIDA_VERSION}-linux-x86_64.tar.xz=139e903abe87117477cd888f8b6417ce2be2ec98d124981ac30d158b906ca3b6"
    "frida-core-devkit-${LUA_DEBUG_FRIDA_VERSION}-linux-arm64.tar.xz=e72e266df7f3cf44929ede370fb0d07feee216e691cf58dd0ef1ac1a7569735e"
    "frida-core-devkit-${LUA_DEBUG_FRIDA_VERSION}-windows-x86.tar.xz=885e4e215fea382c480f34b1cad3992f01e1264d46cb58147560b2586b7e4864"
    "frida-core-devkit-${LUA_DEBUG_FRIDA_VERSION}-windows-x86_64.tar.xz=d59b41466826e537a9f51211584017d05ba335288c61b204146d419a45c8c44f"
    CACHE STRING "Pinned SHA256 per Frida devkit archive")

function(_lua_debug_frida_hash asset out_var)
    foreach(entry IN LISTS LUA_DEBUG_FRIDA_SHA256)
        if(entry MATCHES "^([^=]+)=(.+)$" AND CMAKE_MATCH_1 STREQUAL asset)
            set(${out_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "lua-debug: no pinned SHA256 for Frida asset ${asset}; add it to LUA_DEBUG_FRIDA_SHA256")
endfunction()

# lua_debug_frida_prepare(<gum|core>) -> sets LUA_DEBUG_FRIDA_<COMP>_DIR
function(lua_debug_frida_prepare component)
    string(TOUPPER "${component}" component_upper)
    set(kind "frida-${component}")
    set(target_dir "${LUA_DEBUG_THIRD_PARTY_DIR}/frida_${component}/${LUA_DEBUG_FRIDA_TARGET}")
    set(stamp "${target_dir}/.frida-version")

    if(WIN32)
        set(library "${target_dir}/frida-${component}.lib")
    else()
        set(library "${target_dir}/libfrida-${component}.a")
    endif()
    set(header "${target_dir}/frida-${component}.h")

    set(ready TRUE)
    if(NOT EXISTS "${header}" OR NOT EXISTS "${library}")
        set(ready FALSE)
    endif()
    if(EXISTS "${stamp}")
        file(READ "${stamp}" recorded_version)
        if(NOT recorded_version STREQUAL LUA_DEBUG_FRIDA_VERSION)
            set(ready FALSE)
        endif()
    else()
        set(ready FALSE)
    endif()

    if(NOT ready)
        set(asset "${kind}-devkit-${LUA_DEBUG_FRIDA_VERSION}-${LUA_DEBUG_FRIDA_TARGET}.tar.xz")
        _lua_debug_frida_hash("${asset}" asset_hash)
        message(STATUS "lua-debug: fetching ${asset}")
        # strip 0: the devkit archives hold their files at the top level.
        fetch_dep(
            NAME "${kind}"
            URL "https://github.com/frida/frida/releases/download/${LUA_DEBUG_FRIDA_VERSION}/${asset}"
            SHA256 "${asset_hash}"
            DEST_DIR "${target_dir}"
            STRIP_COMPONENTS 0
            MARKERS "frida-${component}.h")
        file(WRITE "${stamp}" "${LUA_DEBUG_FRIDA_VERSION}")
        if(NOT EXISTS "${library}")
            message(FATAL_ERROR "lua-debug: ${kind} devkit in ${target_dir} is missing ${library}")
        endif()
    else()
        message(STATUS "lua-debug: ${kind} devkit ${LUA_DEBUG_FRIDA_VERSION} at ${target_dir}")
    endif()

    set(LUA_DEBUG_FRIDA_${component_upper}_DIR "${target_dir}" PARENT_SCOPE)
endfunction()

# gumpp: the C++ wrapper compiled into the launcher (and the gum tests).
function(lua_debug_add_gumpp)
    if(TARGET gumpp)
        return()
    endif()
    lua_debug_frida_prepare(gum)
    set(gum_dir "${LUA_DEBUG_FRIDA_GUM_DIR}")
    set(gumpp_dir "${LUA_DEBUG_THIRD_PARTY_DIR}/frida_gum/gumpp")

    file(GLOB_RECURSE gumpp_sources CONFIGURE_DEPENDS "${gumpp_dir}/src/*.cpp")
    if(WIN32)
        file(GLOB_RECURSE gumpp_c_sources CONFIGURE_DEPENDS "${gumpp_dir}/src/internal/*.c")
        list(APPEND gumpp_sources ${gumpp_c_sources})
    endif()

    add_library(gumpp STATIC ${gumpp_sources})
    lua_debug_apply_standards(gumpp)
    set_target_properties(gumpp PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        ARCHIVE_OUTPUT_DIRECTORY "${LUA_DEBUG_LIB_DIR}")
    target_include_directories(gumpp PUBLIC "${gum_dir}" "${gumpp_dir}")
    target_compile_definitions(gumpp PUBLIC GUMPP_STATIC)
    if(WIN32)
        target_compile_definitions(gumpp PRIVATE HAVE_WINDOWS _CRT_SECURE_NO_WARNINGS)
        target_compile_options(gumpp PRIVATE /wd4819 /wd5051 /MT)
        target_link_libraries(gumpp PUBLIC
            kernel32 user32 gdi32 winspool comdlg32 advapi32 shell32 ole32 oleaut32 uuid odbc32 odbccp32)
        target_link_libraries(gumpp PUBLIC "${gum_dir}/frida-gum.lib")
    else()
        target_include_directories(gumpp SYSTEM PUBLIC "${gum_dir}")
        target_link_libraries(gumpp PUBLIC "${gum_dir}/libfrida-gum.a")
        # No extra Apple frameworks: the launcher is injected into arbitrary
        # processes, and frida's own loader refuses to map a library whose
        # dependencies are not already present in the target.
    endif()
    lua_debug_apply_triple(gumpp)
endfunction()

# frida-core consumer interface. The link set follows the example shipped inside
# each devkit (frida-core-example.c and, on Windows, frida-core-example.vcxproj).
function(lua_debug_add_frida_core out_target)
    lua_debug_frida_prepare(core)
    set(core_dir "${LUA_DEBUG_FRIDA_CORE_DIR}")

    if(NOT TARGET frida_core)
        add_library(frida_core INTERFACE)
        target_include_directories(frida_core SYSTEM INTERFACE "${core_dir}")
        if(WIN32)
            target_compile_definitions(frida_core INTERFACE _CRT_SECURE_NO_WARNINGS)
            target_link_libraries(frida_core INTERFACE "${core_dir}/frida-core.lib")
        else()
            target_link_libraries(frida_core INTERFACE "${core_dir}/libfrida-core.a")
        endif()
        if(APPLE)
            target_link_libraries(frida_core INTERFACE
                bsm dl resolv m
                "-framework Foundation" "-framework CoreFoundation"
                "-framework AppKit" "-framework CoreServices")
        elseif(UNIX)
            target_link_libraries(frida_core INTERFACE
                m dl atomic rt resolv pthread)
        endif()
    endif()
    set(${out_target} frida_core PARENT_SCOPE)
endfunction()
