# API reference generation (#360). Documentation tooling only: nothing here is
# exported or installed, and no Terreate target links to or includes anything
# from it, so a consumer of the package never receives a docs dependency.
find_program(TERREATE_DOXYGEN_EXECUTABLE doxygen)
if(NOT TERREATE_DOXYGEN_EXECUTABLE)
  message(FATAL_ERROR
    "TERREATE_BUILD_DOCS=ON requires doxygen, which was not found. Enter the "
    "Nix dev shell, install doxygen, or set TERREATE_BUILD_DOCS=OFF")
endif()
find_program(TERREATE_SPHINX_EXECUTABLE sphinx-build)
if(NOT TERREATE_SPHINX_EXECUTABLE)
  message(FATAL_ERROR
    "TERREATE_BUILD_DOCS=ON requires sphinx-build (with myst-parser and breathe), which "
    "was not found. Enter the Nix dev shell, install them, or set TERREATE_BUILD_DOCS=OFF")
endif()

# The public include trees that exist in this checkout; src/ is never an input.
set(_terreate_doxygen_inputs "")
foreach(_tree IN ITEMS core/include modules/asset/include modules/platform/include modules/graphics/include
        integrations/platform-graphics/include integrations/asset-graphics/include
        adapters/asset-image-stb/include)
  if(EXISTS "${PROJECT_SOURCE_DIR}/${_tree}")
    list(APPEND _terreate_doxygen_inputs "\"${PROJECT_SOURCE_DIR}/${_tree}\"")
  endif()
endforeach()
if(NOT _terreate_doxygen_inputs)
  message(FATAL_ERROR "TERREATE_BUILD_DOCS=ON found no public include tree to document")
endif()
list(JOIN _terreate_doxygen_inputs " " TERREATE_DOXYGEN_INPUT)
set(TERREATE_DOXYGEN_STRIP "${_terreate_doxygen_inputs}")
set(TERREATE_DOXYGEN_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/doxygen")
file(MAKE_DIRECTORY "${TERREATE_DOXYGEN_OUTPUT}")

set(_strict_log "${CMAKE_CURRENT_BINARY_DIR}/doxygen-strict.log")
set(_count_log "${CMAKE_CURRENT_BINARY_DIR}/doxygen-undocumented.log")

# Strict pass: reference errors fail the run.
set(TERREATE_DOXYGEN_WARN_UNDOCUMENTED NO)
set(TERREATE_DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
set(TERREATE_DOXYGEN_WARN_LOG "${_strict_log}")
configure_file(Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)

# Counting pass: reports undocumented members to a log, never fails.
set(TERREATE_DOXYGEN_WARN_UNDOCUMENTED YES)
set(TERREATE_DOXYGEN_WARN_AS_ERROR NO)
set(TERREATE_DOXYGEN_WARN_LOG "${_count_log}")
set(TERREATE_DOXYGEN_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/doxygen-count")
configure_file(Doxyfile.in "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.count" @ONLY)

add_custom_target(terreate_docs
  COMMAND "${CMAKE_COMMAND}"
    -DTERREATE_DOXYGEN_EXECUTABLE=${TERREATE_DOXYGEN_EXECUTABLE}
    -DTERREATE_DOXYFILE_STRICT=${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
    -DTERREATE_DOXYFILE_COUNT=${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.count
    -DTERREATE_DOXYGEN_STRICT_LOG=${_strict_log}
    -DTERREATE_DOXYGEN_COUNT_LOG=${_count_log}
    -DTERREATE_DOXYGEN_XML_INDEX=${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml/index.xml
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_doxygen.cmake
  COMMENT "Generating the API reference (HTML in ${CMAKE_CURRENT_BINARY_DIR}/doxygen/html)"
  VERBATIM)

add_test(NAME docs.doxygen
  COMMAND "${CMAKE_COMMAND}"
    -DTERREATE_DOXYGEN_EXECUTABLE=${TERREATE_DOXYGEN_EXECUTABLE}
    -DTERREATE_DOXYFILE_STRICT=${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
    -DTERREATE_DOXYFILE_COUNT=${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.count
    -DTERREATE_DOXYGEN_STRICT_LOG=${_strict_log}
    -DTERREATE_DOXYGEN_COUNT_LOG=${_count_log}
    -DTERREATE_DOXYGEN_XML_INDEX=${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml/index.xml
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_doxygen.cmake)
set_tests_properties(docs.doxygen PROPERTIES
  PASS_REGULAR_EXPRESSION "docs\\.doxygen: reference=PASS"
  FAIL_REGULAR_EXPRESSION "docs\\.doxygen: error"
  FIXTURES_SETUP terreate_doxygen_xml)

# Prose pages (#361): Markdown through MyST; the API pages read the Doxygen XML.
set(_sphinx_args
  -DTERREATE_SPHINX_EXECUTABLE=${TERREATE_SPHINX_EXECUTABLE}
  -DTERREATE_SPHINX_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}
  -DTERREATE_SPHINX_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/sphinx
  -DTERREATE_DOXYGEN_XML_DIR=${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml
  -DTERREATE_VERSION=${PROJECT_VERSION})
add_custom_target(terreate_docs_sphinx
  COMMAND "${CMAKE_COMMAND}" ${_sphinx_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_sphinx.cmake
  COMMENT "Building the documentation pages (HTML in ${CMAKE_CURRENT_BINARY_DIR}/sphinx/html)"
  VERBATIM)
add_dependencies(terreate_docs_sphinx terreate_docs)
add_test(NAME docs.sphinx
  COMMAND "${CMAKE_COMMAND}" ${_sphinx_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_sphinx.cmake)
set_tests_properties(docs.sphinx PROPERTIES
  PASS_REGULAR_EXPRESSION "docs\\.sphinx: pages=PASS"
  FAIL_REGULAR_EXPRESSION "docs\\.sphinx: error"
  FIXTURES_REQUIRED terreate_doxygen_xml)
