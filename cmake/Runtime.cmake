# The shared runtime library: one session hub (SessionHub + Master) per
# platform/architecture.
#
# Every per-version bridge and the adapter executable load this single library
# through the runtime ABI (src/debugger/runtime/abi.h); it is never copied into a
# version directory and never linked statically into a bridge.

include_guard(GLOBAL)

set(_runtime_sources
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/capabilities.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/json.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/log.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/common/protocol.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/runtime/runtime.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/session/master.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/session/session_hub.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/transport/channel_socket.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/transport/channel_stdio.cpp
    ${LUA_DEBUG_SOURCE_DIR}/src/debugger/transport/reactor.cpp)

lua_debug_third_party_includes(_third_party)

lua_debug_add_library(luadebug_runtime
    SOURCES ${_runtime_sources}
    INCLUDES
        ${LUA_DEBUG_SOURCE_DIR}/src/debugger
        ${_third_party}
    OUTPUT_NAME luadebug_runtime)

target_link_libraries(luadebug_runtime PRIVATE
    Boost::asio Boost::process Boost::unordered Boost::system
    fmt::fmt)

if(APPLE)
    # The runtime library is loaded with RTLD_NOW by bridges and by the adapter;
    # symbols it resolves from the loader are intentional.
    target_link_options(luadebug_runtime PRIVATE "-undefined" "dynamic_lookup")
    set_target_properties(luadebug_runtime PROPERTIES
        INSTALL_NAME_DIR "@rpath"
        MACOSX_RPATH ON)
    set_target_properties(luadebug_runtime PROPERTIES SUFFIX ".dylib")
endif()
lua_debug_apply_triple(luadebug_runtime)

lua_debug_publish(luadebug_runtime "runtime/${LUA_DEBUG_RUNTIME_PLATFORM}")

# The adapter links the runtime sources' *headers* only; it loads the library at
# run time. Nothing else is needed here.
