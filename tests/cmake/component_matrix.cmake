cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED TERREATE_SOURCE_DIR OR "${TERREATE_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_SOURCE_DIR is required")
endif()
if(NOT DEFINED TERREATE_MATRIX_BINARY_ROOT OR
   "${TERREATE_MATRIX_BINARY_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_MATRIX_BINARY_ROOT is required")
endif()

get_filename_component(_terreate_source_dir "${TERREATE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_terreate_matrix_root
  "${TERREATE_MATRIX_BINARY_ROOT}" ABSOLUTE)
file(MAKE_DIRECTORY "${_terreate_matrix_root}")

function(terreate_matrix_case name component core platform graphics)
  set(_binary_dir "${_terreate_matrix_root}/${name}")
  set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${_terreate_source_dir}/tests/cmake/matrix-project"
    -B "${_binary_dir}")
  if(DEFINED TERREATE_CMAKE_GENERATOR AND
     NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
    list(APPEND _configure_command -G "${TERREATE_CMAKE_GENERATOR}")
  endif()
  if(DEFINED TERREATE_CXX_COMPILER AND
     NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
    list(APPEND _configure_command
      "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
  endif()
  list(APPEND _configure_command
    "-DTERREATE_SOURCE_DIR:PATH=${_terreate_source_dir}"
    "-DTERREATE_MATRIX_COMPONENT=${component}"
    "-DTERREATE_EXPECT_CORE=${core}"
    "-DTERREATE_EXPECT_PLATFORM=${platform}"
    "-DTERREATE_EXPECT_GRAPHICS=${graphics}"
    "-DTERREATE_BUILD_CORE=${core}"
    "-DTERREATE_BUILD_PLATFORM=${platform}"
    "-DTERREATE_BUILD_GRAPHICS=${graphics}")

  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "${name} configuration failed (${_configure_result})\n"
      "${_configure_output}\n${_configure_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_binary_dir}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
      "${name} build failed (${_build_result})\n"
      "${_build_output}\n${_build_error}")
  endif()
endfunction()

terreate_matrix_case(core_only Core ON OFF OFF)
terreate_matrix_case(platform_only Platform ON ON OFF)
terreate_matrix_case(graphics_only Graphics ON OFF ON)

# Reconfiguring an existing tree with changed BUILD_* options must update the
# cached component selection without stale compatibility state.
terreate_matrix_case(reconfigure Core ON OFF OFF)
terreate_matrix_case(reconfigure Platform ON ON OFF)

set(_contradictory_binary "${_terreate_matrix_root}/contradictory")
set(_contradictory_command
  "${CMAKE_COMMAND}"
  -S "${_terreate_source_dir}/tests/cmake/matrix-project"
  -B "${_contradictory_binary}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _contradictory_command -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _contradictory_command
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _contradictory_command
  "-DTERREATE_SOURCE_DIR:PATH=${_terreate_source_dir}"
  -DTERREATE_MATRIX_COMPONENT=Core
  -DTERREATE_BUILD_CORE=OFF
  -DTERREATE_BUILD_PLATFORM=ON
  -DTERREATE_BUILD_GRAPHICS=OFF)
execute_process(
  COMMAND ${_contradictory_command}
  RESULT_VARIABLE _contradictory_result
  OUTPUT_VARIABLE _contradictory_output
  ERROR_VARIABLE _contradictory_error)
if(_contradictory_result EQUAL 0)
  message(FATAL_ERROR
    "contradictory Core/Platform options unexpectedly configured")
endif()
set(_contradictory_diagnostics
  "${_contradictory_output}\n${_contradictory_error}")
if(NOT _contradictory_diagnostics MATCHES
   "[Cc]ore.*require|require.*[Cc]ore")
  message(FATAL_ERROR
    "contradictory options did not report the Core dependency:\n"
    "${_contradictory_diagnostics}")
endif()
