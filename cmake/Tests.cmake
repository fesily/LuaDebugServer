# Native tests and their CTest registrations.
#
# Every target the previous recipe built is reproduced with the same sources and
# the same runtime contract, and each one is registered by hand: a test that is
# missing its binary or its fixture is a failure, never a silent skip.
#
# Tests that exercise the target-side code link the bridge *sources* (core plus
# the profile backend they target) instead of a per-version bridge library, which
# is what the unified bridge made possible.
#
# Invocation contracts (previously implicit, now written down):
#   test_session_hub      <runtime library path>
#   test_two_bridges      <runtime library> <shim a> <shim b>
#   test_two_lua_vms      <bridge> [--serve <address> <script a> <script b>]
#   test_mixed_vm         <bridge> <lib a> <profile a> <lib b> <profile b> [A|B] [join]
#   test_multi_group_target <lib a> [<lib b>] --dir <dir> [--load-second]
#   testwaitdll           <launcher> <shared lua dso> <bootstrap json or "-">
#   test_load             <library> [<library> ...]

include_guard(GLOBAL)

set(LUA_DEBUG_TEST_BIN_DIR "${CMAKE_BINARY_DIR}/tests/bin")
file(MAKE_DIRECTORY "${LUA_DEBUG_TEST_BIN_DIR}")

lua_debug_third_party_includes(_third_party)

set(_lua54 "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/lua54")
# A test that embeds a Lua core must see the same configuration defines as that
# core: `luaconf.h` keys several API details on them.
function(_lua_debug_test_defines version out_var)
    _lua_debug_version_defines("${version}" defines)
    set(${out_var} ${defines} PARENT_SCOPE)
endfunction()

foreach(_test_version IN LISTS LUA_DEBUG_LUA_VERSIONS)
    _lua_debug_test_defines("${_test_version}" "_lua_${_test_version}_defines")
endforeach()

set(_luadebug_src "${LUA_DEBUG_SOURCE_DIR}/src/luadebug")
set(_debugger_src "${LUA_DEBUG_SOURCE_DIR}/src/debugger")

function(_lua_debug_test_target name)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;DEFINES;LINKS" ${ARGN})
    lua_debug_add_executable("${name}"
        SOURCES ${ARG_SOURCES}
        INCLUDES ${ARG_INCLUDES}
        DEFINES ${ARG_DEFINES}
        LINKS ${ARG_LINKS})
    set_target_properties("${name}" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${LUA_DEBUG_TEST_BIN_DIR}")
    lua_debug_apply_triple("${name}")
endfunction()

function(_lua_debug_add_test name)
    cmake_parse_arguments(ARG "" "" "COMMAND;PROPERTIES" ${ARGN})
    add_test(NAME "${name}" COMMAND ${ARG_COMMAND} WORKING_DIRECTORY "${LUA_DEBUG_TEST_BIN_DIR}")
    if(ARG_PROPERTIES)
        set_tests_properties("${name}" PROPERTIES ${ARG_PROPERTIES})
    endif()
endfunction()

# A test that hosts the bridge must export the Lua API it embeds: the bridge
# resolves `lua_*` from the host process, exactly like a real embedder.
function(_lua_debug_test_hosts_lua name)
    set_target_properties("${name}" PROPERTIES
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET default)
    if(UNIX AND NOT APPLE)
        # The bridge resolves the host's `lua_*` through a handle for this
        # executable, which on ELF only works when the symbols are exported
        # (`--export-dynamic`). Without it every embedded-Lua test fails with "cannot
        # resolve the test module" and the Linux suite proves nothing.
        set_target_properties("${name}" PROPERTIES ENABLE_EXPORTS ON)
    endif()
    target_link_libraries("${name}" PRIVATE
        luadebug_bridge_core "luadebug_backend_${ARGN}")
endfunction()

# --- stand-in bridges and shared interpreters --------------------------------
foreach(_name a b)
    add_library("test_bridge_shim_${_name}" SHARED
        "${LUA_DEBUG_SOURCE_DIR}/test/bridge_shim.cpp")
    lua_debug_apply_defaults("test_bridge_shim_${_name}")
    target_include_directories("test_bridge_shim_${_name}" PRIVATE "${_debugger_src}")
    set_target_properties("test_bridge_shim_${_name}" PROPERTIES
        PREFIX ""
        SUFFIX "${LUA_DEBUG_MODULE_SUFFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${LUA_DEBUG_TEST_BIN_DIR}")
    if(APPLE)
        target_link_options("test_bridge_shim_${_name}" PRIVATE "-undefined" "dynamic_lookup")
    endif()
    lua_debug_apply_triple("test_bridge_shim_${_name}")
endforeach()

foreach(_version IN LISTS LUA_DEBUG_LUA_VERSIONS)
    lua_debug_add_test_lua("${_version}")
endforeach()

# The heterogeneous-group host needs a *real* LuaJIT as a shared library: it is
# its own ABI group, so a stand-in cannot prove the refusal. The published
# interpreter is already a shared library, so it only has to be renamed into the
# test directory.
add_custom_target(test_lua_luajit ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:luajit-5.1>"
        "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_luajit${LUA_DEBUG_MODULE_SUFFIX}"
    DEPENDS luajit-5.1
    COMMENT "stage test_lua_luajit"
    VERBATIM)

# --- target-side unit tests --------------------------------------------------
_lua_debug_test_target(test_target_protected
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/target_protected_test.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${_luadebug_src}" "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})
_lua_debug_test_hosts_lua(test_target_protected lua54)

_lua_debug_test_target(test_target_value_store
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/target_value_store_test.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${_luadebug_src}" "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})
_lua_debug_test_hosts_lua(test_target_value_store lua54)

_lua_debug_test_target(test_target_evaluator
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/target_evaluator_test.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${_luadebug_src}" "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})
_lua_debug_test_hosts_lua(test_target_evaluator lua54)

_lua_debug_test_target(test_proto_view
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/proto_view_test.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${_luadebug_src}" "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})
_lua_debug_test_hosts_lua(test_proto_view lua54)

foreach(_version IN ITEMS lua53 lua54 lua55)
    _lua_debug_test_target("test_disasm_${_version}"
        SOURCES
            "${LUA_DEBUG_SOURCE_DIR}/test/disasm_test.cpp"
            $<TARGET_OBJECTS:lua_${_version}_core>
        INCLUDES "${_luadebug_src}" "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${_version}" ${_third_party}
        DEFINES ${_lua_lua${_version}_defines})
    _lua_debug_test_hosts_lua("test_disasm_${_version}" "${_version}")
endforeach()

foreach(_version IN ITEMS lua51 lua52 lua53 lua54 lua55)
    _lua_debug_test_target("test_proto_view_${_version}"
        SOURCES
            "${LUA_DEBUG_SOURCE_DIR}/test/proto_view_test.cpp"
            $<TARGET_OBJECTS:lua_${_version}_core>
        INCLUDES "${_luadebug_src}" "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/${_version}" ${_third_party}
        DEFINES ${_lua_lua${_version}_defines})
    _lua_debug_test_hosts_lua("test_proto_view_${_version}" "${_version}")
endforeach()

# --- worker / session execution ----------------------------------------------
foreach(_name worker_session session_exec)
    _lua_debug_test_target("test_${_name}"
        SOURCES
            "${LUA_DEBUG_SOURCE_DIR}/test/${_name}_test.cpp"
            $<TARGET_OBJECTS:lua_lua54_core>
        INCLUDES "${_luadebug_src}" "${_debugger_src}" "${_lua54}" ${_third_party}
        DEFINES ${_lua_lua54_defines})
    _lua_debug_test_hosts_lua("test_${_name}" lua54)
endforeach()

# --- hub / master (the runtime implementation is linked in, the ABI is still
# --- loaded from the built library) ------------------------------------------
set(_runtime_impl_sources
    "${_debugger_src}/common/capabilities.cpp"
    "${_debugger_src}/common/json.cpp"
    "${_debugger_src}/common/log.cpp"
    "${_debugger_src}/common/protocol.cpp"
    "${_debugger_src}/runtime/runtime.cpp"
    "${_debugger_src}/session/master.cpp"
    "${_debugger_src}/session/session_hub.cpp"
    "${_debugger_src}/transport/channel_socket.cpp"
    "${_debugger_src}/transport/channel_stdio.cpp"
    "${_debugger_src}/transport/reactor.cpp")

foreach(_name master session_hub)
    if(_name STREQUAL "master")
        set(_extra_sources "${LUA_DEBUG_SOURCE_DIR}/test/master_test.cpp")
    else()
        set(_extra_sources "${LUA_DEBUG_SOURCE_DIR}/test/session_hub_test.cpp")
        file(GLOB _host_shims CONFIGURE_DEPENDS "${_debugger_src}/host/*.cpp")
        list(APPEND _extra_sources ${_host_shims})
    endif()
    _lua_debug_test_target("test_${_name}"
        SOURCES ${_extra_sources} ${_runtime_impl_sources}
        INCLUDES "${_debugger_src}" ${_third_party}
        LINKS Boost::asio Boost::process Boost::unordered Boost::system fmt::fmt)
    if(APPLE)
        target_link_options("test_${_name}" PRIVATE "-undefined" "dynamic_lookup")
    endif()
endforeach()

_lua_debug_test_target(test_two_bridges
    SOURCES "${LUA_DEBUG_SOURCE_DIR}/test/two_bridge_host.cpp"
    INCLUDES "${_debugger_src}")

# Two states of one ABI group: the host embeds Lua 5.4 and enters the bridge
# twice through the same profile entry, so it must export the Lua API.
_lua_debug_test_target(test_two_lua_vms
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/two_lua_vms.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})
set_target_properties(test_two_lua_vms PROPERTIES ENABLE_EXPORTS ON)
if(APPLE)
    target_link_libraries(test_two_lua_vms PRIVATE m)
elseif(UNIX)
    target_link_libraries(test_two_lua_vms PRIVATE m dl)
endif()

# Two real interpreters of different ABIs loaded side by side.
_lua_debug_test_target(test_mixed_vm
    SOURCES "${LUA_DEBUG_SOURCE_DIR}/test/mixed_vm_host.cpp"
    INCLUDES "${_luadebug_src}")
if(UNIX)
    target_link_libraries(test_mixed_vm PRIVATE dl)
endif()
if(APPLE)
    target_link_libraries(test_mixed_vm PRIVATE m)
endif()

_lua_debug_test_target(test_multi_group_target
    SOURCES "${LUA_DEBUG_SOURCE_DIR}/test/multi_group_target.cpp")
if(UNIX)
    target_link_libraries(test_multi_group_target PRIVATE dl)
endif()
if(APPLE)
    target_link_libraries(test_multi_group_target PRIVATE m)
endif()

# --- bootstrap, helpers, gum -------------------------------------------------
_lua_debug_test_target(test_bootstrap_config
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/bootstrap_config_test.cpp"
        "${_debugger_src}/common/bootstrap.cpp"
        "${_debugger_src}/common/vm_source.cpp"
        "${_debugger_src}/common/vm_version.cpp"
        "${_debugger_src}/common/path_utf8.cpp"
    INCLUDES "${_debugger_src}" "${LUA_DEBUG_SOURCE_DIR}/src" ${_third_party})

# The thunk test drives a real `lua_State`'s allocations through the generated
# allocator thunk: the synthetic argument checks cannot observe a misaligned call
# into the C allocator, which is what broke every hook-installing test on x86_64.
_lua_debug_test_target(test_thunk
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/thunk.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${_luadebug_src}" "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})

_lua_debug_test_target(testwaitdll
    SOURCES "${LUA_DEBUG_SOURCE_DIR}/test/waitdll.cpp"
    INCLUDES "${LUA_DEBUG_THIRD_PARTY_DIR}/lua/lua51")
target_link_libraries(testwaitdll PRIVATE dl)

lua_debug_add_gumpp()
_lua_debug_test_target(test_frida
    SOURCES "${LUA_DEBUG_SOURCE_DIR}/test/interceptor.cpp"
    INCLUDES "${LUA_DEBUG_THIRD_PARTY_DIR}/frida_gum/gumpp"
    LINKS gumpp)
file(GLOB _symbol_sources CONFIGURE_DEPENDS "${LUA_DEBUG_SOURCE_DIR}/test/symbol/*.c")
_lua_debug_test_target(test_symbol
    SOURCES ${_symbol_sources}
    INCLUDES "${LUA_DEBUG_FRIDA_GUM_DIR}" "${LUA_DEBUG_THIRD_PARTY_DIR}/frida_gum/gumpp"
    LINKS gumpp)

# Loader check host: `test_load <library> ...` dlopens each library.
_lua_debug_test_target(test_load
    SOURCES "${LUA_DEBUG_SOURCE_DIR}/test/load/test.cpp")
target_link_libraries(test_load PRIVATE dl)

# --- optional VM source ------------------------------------------------------
# The pure rules (pattern syntax, ABI identification) need no target process.
_lua_debug_test_target(test_vm_source
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/vm_source_test.cpp"
        "${LUA_DEBUG_SOURCE_DIR}/src/launcher/resolver/gum_pattern.cpp"
        "${_debugger_src}/common/vm_version.cpp"
        "${_debugger_src}/common/vm_source.cpp"
        "${_debugger_src}/common/path_utf8.cpp"
        "${LUA_DEBUG_SOURCE_DIR}/src/luadebug/vm/mapping.cpp"
    INCLUDES "${_debugger_src}" "${LUA_DEBUG_SOURCE_DIR}/src/launcher"
        "${LUA_DEBUG_SOURCE_DIR}/src/luadebug" "${LUA_DEBUG_SOURCE_DIR}/src" ${_third_party})

# A host that exports nothing. The Lua API is linked into this executable but is
# absent from its exported symbol table, so `dlsym` cannot find it and a stripped
# copy has no symbol table at all - the exact shape an external symbol table is
# for. It never links the bridge: it dlopens the built one, like a real host.
_lua_debug_test_target(test_noexport_host
    SOURCES
        "${LUA_DEBUG_SOURCE_DIR}/test/noexport_host.cpp"
        $<TARGET_OBJECTS:lua_lua54_core>
    INCLUDES "${LUA_DEBUG_SOURCE_DIR}/src/luadebug" "${_lua54}" ${_third_party}
    DEFINES ${_lua_lua54_defines})
set_target_properties(test_noexport_host PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)
if(APPLE)
    set(_noexport_list "${CMAKE_CURRENT_BINARY_DIR}/noexport.exports")
    file(WRITE "${_noexport_list}" "_main\n")
    target_link_options(test_noexport_host PRIVATE
        "-Wl,-exported_symbols_list,${_noexport_list}")
elseif(UNIX)
    set(_noexport_map "${CMAKE_CURRENT_BINARY_DIR}/noexport.map")
    file(WRITE "${_noexport_map}" "{ global: main; local: *; };\n")
    target_link_options(test_noexport_host PRIVATE "-Wl,--version-script=${_noexport_map}")
endif()
target_link_libraries(test_noexport_host PRIVATE ${CMAKE_DL_LIBS})

# --- registrations -----------------------------------------------------------
_lua_debug_add_test(proto_view COMMAND test_proto_view)
foreach(_version IN ITEMS lua51 lua52 lua53 lua54 lua55)
    _lua_debug_add_test("proto_view_${_version}" COMMAND "test_proto_view_${_version}")
endforeach()
foreach(_version IN ITEMS lua53 lua54 lua55)
    _lua_debug_add_test("disasm_${_version}" COMMAND "test_disasm_${_version}")
endforeach()
_lua_debug_add_test(target_protected COMMAND test_target_protected)
_lua_debug_add_test(target_value_store COMMAND test_target_value_store)
_lua_debug_add_test(target_evaluator COMMAND test_target_evaluator)
_lua_debug_add_test(worker_session COMMAND test_worker_session)
_lua_debug_add_test(session_exec COMMAND test_session_exec)
_lua_debug_add_test(bootstrap_config COMMAND test_bootstrap_config)
_lua_debug_add_test(vm_source_rules COMMAND test_vm_source)
_lua_debug_add_test(thunk COMMAND test_thunk)
_lua_debug_add_test(frida_interceptor COMMAND test_frida)
_lua_debug_add_test(frida_symbol COMMAND test_symbol)

if(WIN32)
    set(_runtime_library "${LUA_DEBUG_LIB_DIR}/luadebug_runtime.dll")
elseif(APPLE)
    set(_runtime_library "${LUA_DEBUG_LIB_DIR}/libluadebug_runtime.dylib")
else()
    set(_runtime_library "${LUA_DEBUG_LIB_DIR}/libluadebug_runtime.so")
endif()
set(_bridge_library "${LUA_DEBUG_LIB_DIR}/luadebug${LUA_DEBUG_MODULE_SUFFIX}")

# The interpreter in these three is invisible to every name-based lookup.
_lua_debug_add_test(vm_source_noexport_hidden COMMAND test_noexport_host check-hidden)
_lua_debug_add_test(symbol_bind_table COMMAND test_noexport_host
    bind "${_bridge_library}" "3")
_lua_debug_add_test(symbol_bind_resolver COMMAND test_noexport_host
    resolver "${_bridge_library}" "3")
# Identity without any module: one table serves two states, a different address
# set is a different VM, and a bound state refuses to be rebound.
_lua_debug_add_test(symbol_bind_identity COMMAND test_noexport_host
    two-states "${_bridge_library}" "3")
# A refused binding names what is wrong and leaves the VM untouched.
_lua_debug_add_test(symbol_bind_failures COMMAND test_noexport_host
    failures "${_bridge_library}" "3")
# A table that satisfies the six "required" names but drops one the backend calls
# without a null check must be refused at bind time, not crash the target.
_lua_debug_add_test(symbol_bind_partial COMMAND test_noexport_host
    partial "${_bridge_library}" "3")
# The host's own address table must be a document the contract accepts.
add_test(NAME vm_source_artifact
    COMMAND ${CMAKE_COMMAND}
        "-DHOST=$<TARGET_FILE:test_noexport_host>"
        "-DCHECKER=$<TARGET_FILE:test_vm_source>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/symbol_artifact"
        -P "${LUA_DEBUG_SOURCE_DIR}/test/check_symbols_artifact.cmake")
# Same binding, from a copy with no symbol table left in the file at all.
add_test(NAME symbol_bind_stripped
    COMMAND ${CMAKE_COMMAND}
        "-DHOST=$<TARGET_FILE:test_noexport_host>"
        "-DBRIDGE=${_bridge_library}"
        "-DPROFILE=3"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/stripped_bind"
        -P "${LUA_DEBUG_SOURCE_DIR}/test/strip_and_run.cmake")
set_tests_properties(symbol_bind_table symbol_bind_resolver symbol_bind_identity
    symbol_bind_failures symbol_bind_partial symbol_bind_stripped vm_source_artifact
    PROPERTIES FIXTURES_REQUIRED built_artifacts)
_lua_debug_add_test(master COMMAND test_master)
_lua_debug_add_test(session_hub COMMAND test_session_hub "${_runtime_library}")
_lua_debug_add_test(two_bridges COMMAND test_two_bridges
    "${_runtime_library}"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_bridge_shim_a${LUA_DEBUG_MODULE_SUFFIX}"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_bridge_shim_b${LUA_DEBUG_MODULE_SUFFIX}")
_lua_debug_add_test(two_lua_vms COMMAND test_two_lua_vms "${_bridge_library}")

# Heterogeneous groups: only the selected ABI joins the session, the other one is
# refused - run in both directions.
foreach(_direction IN ITEMS A B)
    _lua_debug_add_test("mixed_vm_lua51_lua54_${_direction}" COMMAND test_mixed_vm
        "${_bridge_library}"
        "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_lua51${LUA_DEBUG_MODULE_SUFFIX}" "0"
        "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_lua54${LUA_DEBUG_MODULE_SUFFIX}" "3"
        "${_direction}")
endforeach()
_lua_debug_add_test(mixed_vm_luajit_lua54 COMMAND test_mixed_vm
    "${_bridge_library}"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_luajit${LUA_DEBUG_MODULE_SUFFIX}" "5"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_lua54${LUA_DEBUG_MODULE_SUFFIX}" "3"
    "A")
_lua_debug_add_test(mixed_vm_same_group COMMAND test_mixed_vm
    "${_bridge_library}"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_lua55${LUA_DEBUG_MODULE_SUFFIX}" "4"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_lua-latest${LUA_DEBUG_MODULE_SUFFIX}" "4"
    "B" "join")
# `test_multi_group_target` is an e2e driver host (it runs until a `stop` file
# appears), so it is built but not registered here.

_lua_debug_add_test(waitdll COMMAND testwaitdll
    "${LUA_DEBUG_LIB_DIR}/launcher${LUA_DEBUG_MODULE_SUFFIX}"
    "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_lua51${LUA_DEBUG_MODULE_SUFFIX}"
    "-")
# The test skips itself (77) where `wait_dll()` is unimplemented, e.g. Linux: the
# capability is a known gap, and reporting it as a failure would mask real ones.
set_tests_properties(waitdll PROPERTIES SKIP_RETURN_CODE 77)

# In-process load checks: the runtime library alone, then every shared
# interpreter together with the one bridge.
_lua_debug_add_test(load_runtime COMMAND test_load "${_runtime_library}")
foreach(_version IN LISTS LUA_DEBUG_LUA_VERSIONS ITEMS luajit)
    _lua_debug_add_test("load_bridge_${_version}" COMMAND test_load
        "${LUA_DEBUG_TEST_BIN_DIR}/test_lua_${_version}${LUA_DEBUG_MODULE_SUFFIX}"
        "${_bridge_library}")
endforeach()

# The tests need the artifacts they load; CTest has no build dependencies, so the
# whole test run is ordered after the build by the caller (`ctest --preset` after
# `cmake --build`). The fixture below makes the ordering explicit for the tests
# that dlopen a built library.
add_test(NAME artifacts_built COMMAND ${CMAKE_COMMAND} -E true)
set_tests_properties(artifacts_built PROPERTIES FIXTURES_SETUP built_artifacts)
set_tests_properties(
    two_bridges two_lua_vms waitdll load_runtime
    mixed_vm_lua51_lua54_A mixed_vm_lua51_lua54_B mixed_vm_luajit_lua54
    mixed_vm_same_group
    PROPERTIES FIXTURES_REQUIRED built_artifacts)
foreach(_version IN LISTS LUA_DEBUG_LUA_VERSIONS ITEMS luajit)
    set_tests_properties("load_bridge_${_version}" PROPERTIES FIXTURES_REQUIRED built_artifacts)
endforeach()
