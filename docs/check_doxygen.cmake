cmake_minimum_required(VERSION 3.28)

# Runs the strict Doxygen pass (warnings are errors) and then the counting pass
# for undocumented members, which never fails. Prints one marker line.
# Inputs: TERREATE_DOXYGEN_EXECUTABLE, TERREATE_DOXYFILE_STRICT, TERREATE_DOXYFILE_COUNT,
# TERREATE_DOXYGEN_COUNT_LOG, TERREATE_DOXYGEN_STRICT_LOG, TERREATE_DOXYGEN_XML_INDEX.

foreach(_required IN ITEMS TERREATE_DOXYGEN_EXECUTABLE TERREATE_DOXYFILE_STRICT
        TERREATE_DOXYFILE_COUNT TERREATE_DOXYGEN_COUNT_LOG TERREATE_DOXYGEN_STRICT_LOG
        TERREATE_DOXYGEN_XML_INDEX)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "docs.doxygen: ${_required} is required")
  endif()
endforeach()

file(REMOVE "${TERREATE_DOXYGEN_STRICT_LOG}" "${TERREATE_DOXYGEN_COUNT_LOG}")
execute_process(COMMAND "${TERREATE_DOXYGEN_EXECUTABLE}" "${TERREATE_DOXYFILE_STRICT}"
  RESULT_VARIABLE _strict_status ERROR_VARIABLE _strict_error)
set(_strict_warnings "")
if(EXISTS "${TERREATE_DOXYGEN_STRICT_LOG}")
  file(READ "${TERREATE_DOXYGEN_STRICT_LOG}" _strict_warnings)
endif()
if(NOT _strict_status EQUAL 0 OR NOT "${_strict_warnings}" STREQUAL "")
  message(FATAL_ERROR
    "docs.doxygen: error: doxygen reported warnings or failed (status ${_strict_status}):\n"
    "${_strict_warnings}${_strict_error}")
endif()

# A run that processed nothing (for example every input excluded) must not pass:
# the reference has to contain the component namespaces.
if(NOT EXISTS "${TERREATE_DOXYGEN_XML_INDEX}")
  message(FATAL_ERROR "docs.doxygen: error: no reference was generated")
endif()
file(READ "${TERREATE_DOXYGEN_XML_INDEX}" _index)
foreach(_namespace IN ITEMS terreate)
  if(NOT _index MATCHES "<name>${_namespace}</name>")
    message(FATAL_ERROR "docs.doxygen: error: namespace ${_namespace} is missing from the reference")
  endif()
endforeach()

execute_process(COMMAND "${TERREATE_DOXYGEN_EXECUTABLE}" "${TERREATE_DOXYFILE_COUNT}"
  RESULT_VARIABLE _count_status ERROR_VARIABLE _count_error)
if(NOT _count_status EQUAL 0)
  message(FATAL_ERROR "docs.doxygen: error: the counting pass failed: ${_count_error}")
endif()
set(_undocumented 0)
if(EXISTS "${TERREATE_DOXYGEN_COUNT_LOG}")
  file(STRINGS "${TERREATE_DOXYGEN_COUNT_LOG}" _lines REGEX "warning:")
  list(LENGTH _lines _undocumented)
endif()
message("docs.doxygen: reference=PASS warnings=0 undocumented=${_undocumented}")
