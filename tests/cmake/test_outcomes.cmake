cmake_minimum_required(VERSION 3.28)

# Guards the outcome rule in docs/development/graphics-test-matrix.md: a test
# that can report SKIP must also name what PASS looks like, so an environment
# that lacks a capability can never be counted as a pass by an exit code alone.
#
# Inputs: TERREATE_CTEST_COMMAND, TERREATE_BUILD_DIR.
# Rules, applied to every registered test:
#   1. A test without PASS_REGULAR_EXPRESSION must be listed in
#      TERREATE_EXIT_CODE_ONLY_TESTS below (CPU-only or CMake-driven tests whose
#      exit code is the whole result).
#   2. A test with SKIP_REGULAR_EXPRESSION or SKIP_RETURN_CODE must have a
#      PASS_REGULAR_EXPRESSION.

foreach(_required IN ITEMS TERREATE_CTEST_COMMAND TERREATE_BUILD_DIR)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "test_outcomes: ${_required} is required")
  endif()
endforeach()

set(TERREATE_EXIT_CODE_ONLY_TESTS
  smoke
  core.error_result
  core.diagnostics
  graphics.diagnostics
  cmake.component_matrix)

execute_process(
  COMMAND "${TERREATE_CTEST_COMMAND}" --test-dir "${TERREATE_BUILD_DIR}" --show-only=json-v1
  RESULT_VARIABLE _status
  OUTPUT_VARIABLE _json
  ERROR_VARIABLE _error)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "test_outcomes: ctest --show-only failed: ${_error}")
endif()

string(JSON _test_count LENGTH "${_json}" tests)
if(_test_count EQUAL 0)
  message(FATAL_ERROR "test_outcomes: no tests were listed")
endif()

set(_violations "")
math(EXPR _last "${_test_count} - 1")
foreach(_index RANGE ${_last})
  string(JSON _name GET "${_json}" tests ${_index} name)
  set(_has_pass FALSE)
  set(_can_skip FALSE)
  string(JSON _property_count ERROR_VARIABLE _no_properties
         LENGTH "${_json}" tests ${_index} properties)
  if(NOT _no_properties STREQUAL "NOTFOUND")
    set(_property_count 0)
  endif()
  if(_property_count GREATER 0)
    math(EXPR _last_property "${_property_count} - 1")
    foreach(_property RANGE ${_last_property})
      string(JSON _property_name GET "${_json}" tests ${_index} properties ${_property} name)
      if(_property_name STREQUAL "PASS_REGULAR_EXPRESSION")
        set(_has_pass TRUE)
      elseif(_property_name STREQUAL "SKIP_REGULAR_EXPRESSION"
             OR _property_name STREQUAL "SKIP_RETURN_CODE")
        set(_can_skip TRUE)
      endif()
    endforeach()
  endif()

  if(NOT _has_pass AND NOT _name IN_LIST TERREATE_EXIT_CODE_ONLY_TESTS)
    string(APPEND _violations
      "  ${_name}: no PASS_REGULAR_EXPRESSION and not listed as exit-code-only\n")
  endif()
  if(_can_skip AND NOT _has_pass)
    string(APPEND _violations
      "  ${_name}: can report SKIP but has no PASS_REGULAR_EXPRESSION\n")
  endif()
endforeach()

if(NOT "${_violations}" STREQUAL "")
  message(FATAL_ERROR "test_outcomes: outcome rule violated:\n${_violations}")
endif()
message(STATUS "test_outcomes: ${_test_count} tests follow the outcome rule")
message("cmake.test_outcomes: outcome-rule=PASS")
