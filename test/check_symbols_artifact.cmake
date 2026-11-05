# Round-trip the no-export host's own symbol table through the file carrier.
#
# The host computes the table from its own function pointers (the only way to know
# per-process addresses), writes it, and the contract parser reads it back. This is
# the "the target produces its own table" half of the stripped-target proof.

if(NOT HOST OR NOT CHECKER OR NOT WORK)
    message(FATAL_ERROR "check_symbols_artifact: HOST, CHECKER and WORK are required")
endif()

file(MAKE_DIRECTORY "${WORK}")
set(_artifact "${WORK}/noexport-symbols.json")

execute_process(COMMAND "${HOST}" symbols "${_artifact}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "the host could not emit its symbol table (${_status})")
endif()
if(NOT EXISTS "${_artifact}")
    message(FATAL_ERROR "the host produced no artefact")
endif()

execute_process(COMMAND "${CHECKER}" --check-symbols "${_artifact}"
    RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "the artefact is not a usable symbol source (${_status})")
endif()
message(STATUS "the host's own table is accepted as a VM source")
