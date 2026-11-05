# Strip the no-export host and run its table-only binding from the stripped copy.
#
# The point of the copy is the file's symbol table: after `strip` there is nothing
# left to look a name up in, so a binding that still works can only have used the
# addresses it was given.

if(NOT HOST OR NOT BRIDGE OR NOT PROFILE OR NOT WORK)
    message(FATAL_ERROR "strip_and_run: HOST, BRIDGE, PROFILE and WORK are required")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
get_filename_component(_name "${HOST}" NAME)
set(_copy "${WORK}/${_name}")
file(COPY "${HOST}" DESTINATION "${WORK}")

if(WIN32)
    # An executable has no exported symbol table to strip on Windows; the export
    # list already keeps the Lua API out of it.
else()
    find_program(_strip strip REQUIRED)
    execute_process(COMMAND "${_strip}" -x "${_copy}" RESULT_VARIABLE _strip_status)
    if(NOT _strip_status EQUAL 0)
        message(FATAL_ERROR "strip failed (${_strip_status})")
    endif()

    find_program(_nm nm REQUIRED)
    # `-gU` lists external symbols; an undefined import (`U`) is not a symbol table
    # entry the debugger could look up, so only a *defined* one would defeat the
    # proof. A failing `nm` must not turn the assertions into no-ops.
    execute_process(COMMAND "${_nm}" -gU "${_copy}"
        OUTPUT_VARIABLE _symbols
        RESULT_VARIABLE _nm_status
        ERROR_VARIABLE _nm_error)
    if(NOT _nm_status EQUAL 0)
        message(FATAL_ERROR "nm failed (${_nm_status}): ${_nm_error}")
    endif()
    foreach(_name lua_gettop luaL_newstate)
        foreach(_spelling "_${_name}" "${_name}")
            if(_symbols MATCHES "[^U] ${_spelling}\n")
                message(FATAL_ERROR "the stripped copy still defines ${_name} in its symbol table")
            endif()
        endforeach()
    endforeach()
endif()

execute_process(
    COMMAND "${_copy}" bind "${BRIDGE}" "${PROFILE}"
    RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "the stripped host failed to bind from its own table (${_status})")
endif()
message(STATUS "stripped host bound its interpreter from an address-only table")
