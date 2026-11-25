cmake_minimum_required(VERSION 3.28)

# Builds the Sphinx pages with every warning an error (a broken link, a missing
# reference target, an unknown directive, a breathe symbol that does not exist) and
# prints one marker line. Inputs: TERREATE_SPHINX_EXECUTABLE, TERREATE_SPHINX_SOURCE,
# TERREATE_SPHINX_OUTPUT, TERREATE_DOXYGEN_XML_DIR, TERREATE_VERSION.
foreach(_required IN ITEMS TERREATE_SPHINX_EXECUTABLE TERREATE_SPHINX_SOURCE
        TERREATE_SPHINX_OUTPUT TERREATE_DOXYGEN_XML_DIR TERREATE_VERSION)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "docs.sphinx: ${_required} is required")
  endif()
endforeach()
if(NOT EXISTS "${TERREATE_DOXYGEN_XML_DIR}/index.xml")
  message(FATAL_ERROR "docs.sphinx: error: the Doxygen XML is missing (${TERREATE_DOXYGEN_XML_DIR})")
endif()

file(REMOVE_RECURSE "${TERREATE_SPHINX_OUTPUT}")
set(ENV{TERREATE_DOXYGEN_XML} "${TERREATE_DOXYGEN_XML_DIR}")
set(ENV{TERREATE_VERSION} "${TERREATE_VERSION}")
execute_process(
  COMMAND "${TERREATE_SPHINX_EXECUTABLE}" -W --keep-going -q -b html
          -d "${TERREATE_SPHINX_OUTPUT}/.doctrees"
          "${TERREATE_SPHINX_SOURCE}" "${TERREATE_SPHINX_OUTPUT}/html"
  RESULT_VARIABLE _status
  OUTPUT_VARIABLE _output
  ERROR_VARIABLE _error)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "docs.sphinx: error: sphinx-build failed (status ${_status}):\n${_output}${_error}")
endif()
if(NOT EXISTS "${TERREATE_SPHINX_OUTPUT}/html/index.html")
  message(FATAL_ERROR "docs.sphinx: error: no pages were generated")
endif()
message("docs.sphinx: pages=PASS")
