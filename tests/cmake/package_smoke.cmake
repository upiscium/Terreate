cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED TERREATE_SOURCE_DIR OR "${TERREATE_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_SOURCE_DIR is required")
endif()
if(NOT DEFINED TERREATE_PACKAGE_BINARY_ROOT OR
   "${TERREATE_PACKAGE_BINARY_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_PACKAGE_BINARY_ROOT is required")
endif()

get_filename_component(_terreate_source_dir "${TERREATE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_terreate_package_root
  "${TERREATE_PACKAGE_BINARY_ROOT}" ABSOLUTE)
file(MAKE_DIRECTORY "${_terreate_package_root}")

function(terreate_assert_install_boundary prefix name)
  foreach(_public_header IN ITEMS
      "include/terreate/core/error.hpp"
      "include/terreate/core/result.hpp")
    if(NOT EXISTS "${prefix}/${_public_header}")
      message(FATAL_ERROR
        "${name} install is missing public Core header: ${_public_header}")
    endif()
  endforeach()

  file(GLOB_RECURSE _installed_paths LIST_DIRECTORIES true
    "${prefix}/*")
  foreach(_installed_path IN LISTS _installed_paths)
    file(RELATIVE_PATH _installed_relative "${prefix}" "${_installed_path}")
    if("${_installed_relative}" MATCHES "(^|/)(private|detail)(/|$)")
      message(FATAL_ERROR
        "${name} install leaked a private/detail path: ${_installed_relative}")
    endif()
    if("${_installed_relative}" MATCHES
         "\\.(h|hh|hpp|hxx|c|cc|cpp|cxx)$" AND
       NOT "${_installed_relative}" MATCHES
         "^include/terreate/core/(error|result)\\.hpp$")
      message(FATAL_ERROR
        "${name} install unexpectedly contains a source/header file: "
        "${_installed_relative}")
    endif()
  endforeach()

  foreach(_forbidden_path IN ITEMS
      "include/error.hpp"
      "include/result.hpp"
      "include/terreate.hpp"
      "include/terreate/error.hpp"
      "include/terreate/result.hpp"
      "include/project/lib.hpp"
      "include/terreate/core.hpp"
      "include/terreate/platform.hpp"
      "include/terreate/graphics.hpp")
    if(EXISTS "${prefix}/${_forbidden_path}")
      message(FATAL_ERROR
        "${name} install contains forbidden compatibility/API header: "
        "${_forbidden_path}")
    endif()
  endforeach()
endfunction()

function(terreate_package_configure_and_install name platform graphics)
  set(_fixture_build "${_terreate_package_root}/${name}/fixture-build")
  set(_prefix "${_terreate_package_root}/${name}/prefix")
  file(REMOVE_RECURSE "${_fixture_build}" "${_prefix}")
  file(MAKE_DIRECTORY "${_terreate_package_root}/${name}")

  set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${_terreate_source_dir}/tests/cmake/package-project"
    -B "${_fixture_build}")
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
    "-DCMAKE_INSTALL_PREFIX:PATH=${_prefix}"
    -DTERREATE_BUILD_CORE=ON
    "-DTERREATE_BUILD_PLATFORM=${platform}"
    "-DTERREATE_BUILD_GRAPHICS=${graphics}")

  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "${name} package fixture configuration failed\n"
      "${_configure_output}\n${_configure_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_fixture_build}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
      "${name} package fixture build failed\n"
      "${_build_output}\n${_build_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${_fixture_build}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
  if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
      "${name} package fixture install failed\n"
      "${_install_output}\n${_install_error}")
  endif()

  terreate_assert_install_boundary("${_prefix}" "${name}")

  set(${name}_PREFIX "${_prefix}" PARENT_SCOPE)
endfunction()

terreate_package_configure_and_install(all_components ON ON)
set(_all_prefix "${all_components_PREFIX}")
set(_all_consumer "${_terreate_package_root}/all_components/consumer")
file(MAKE_DIRECTORY "${_all_consumer}")
file(WRITE "${_all_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreatePackageConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core Graphics OPTIONAL_COMPONENTS Future)
if(NOT DEFINED Terreate_Future_FOUND OR Terreate_Future_FOUND)
  message(FATAL_ERROR
    "optional unknown component did not set Terreate_Future_FOUND=FALSE")
endif()
foreach(_component IN ITEMS Core Platform Graphics)
  if(NOT TARGET Terreate::${_component})
    message(FATAL_ERROR
      "installed package did not export Terreate::${_component}")
  endif()
endforeach()
get_target_property(_installed_platform_links Terreate::Platform
  INTERFACE_LINK_LIBRARIES)
if(NOT "${_installed_platform_links}" MATCHES "Core")
  message(FATAL_ERROR
    "installed Platform target lost its public Core dependency: "
    "${_installed_platform_links}")
endif()
get_target_property(_installed_graphics_links Terreate::Graphics
  INTERFACE_LINK_LIBRARIES)
if(NOT "${_installed_graphics_links}" MATCHES "Core")
  message(FATAL_ERROR
    "installed Graphics target lost its public Core dependency: "
    "${_installed_graphics_links}")
endif()
if("${_installed_graphics_links}" MATCHES "Platform")
  message(FATAL_ERROR
    "installed Graphics target acquired an invalid Platform dependency: "
    "${_installed_graphics_links}")
endif()
if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR
   NOT "${Terreate_NOT_FOUND_MESSAGE}" MATCHES
      "does not provide requested component")
  message(FATAL_ERROR
    "optional unknown component did not set Terreate_NOT_FOUND_MESSAGE")
endif()
add_executable(core_package_consumer core_main.cpp)
target_link_libraries(core_package_consumer PRIVATE Terreate::Core)
add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE Terreate::Graphics)
set_target_properties(core_package_consumer package_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_all_consumer}/core_main.cpp" [=[
#include <terreate/core/result.hpp>

#include <system_error>

int main() {
  terreate::Result<int> result = 7;
  return terreate::unwrap(result) == 7 ? 0 : 1;
}
]=])
file(WRITE "${_all_consumer}/main.cpp" [=[
int main() { return 0; }
]=])

set(_all_consumer_build "${_terreate_package_root}/all_components/consumer-build")
set(_consumer_configure
  "${CMAKE_COMMAND}"
  -S "${_all_consumer}"
  -B "${_all_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _consumer_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _consumer_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _consumer_configure "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_consumer_configure}
  RESULT_VARIABLE _consumer_configure_result
  OUTPUT_VARIABLE _consumer_configure_output
  ERROR_VARIABLE _consumer_configure_error)
if(NOT _consumer_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed package consumer configuration failed\n"
    "${_consumer_configure_output}\n${_consumer_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_all_consumer_build}"
  RESULT_VARIABLE _consumer_build_result
  OUTPUT_VARIABLE _consumer_build_output
  ERROR_VARIABLE _consumer_build_error)
if(NOT _consumer_build_result EQUAL 0)
  message(FATAL_ERROR
    "installed package consumer build failed\n"
    "${_consumer_build_output}\n${_consumer_build_error}")
endif()

terreate_package_configure_and_install(core_only OFF OFF)
set(_core_prefix "${core_only_PREFIX}")

set(_core_consumer "${_terreate_package_root}/core_only/core-consumer")
file(MAKE_DIRECTORY "${_core_consumer}")
file(WRITE "${_core_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateCoreOnlyConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
if(TARGET Terreate::Platform OR TARGET Terreate::Graphics)
  message(FATAL_ERROR "Core-only package exported an optional component")
endif()
add_executable(core_only_consumer main.cpp)
target_link_libraries(core_only_consumer PRIVATE Terreate::Core)
set_target_properties(core_only_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_core_consumer}/main.cpp" [=[
#include <terreate/core/result.hpp>

int main() {
  terreate::Result<int> result = 23;
  return terreate::unwrap(result) == 23 ? 0 : 1;
}
]=])

set(_core_consumer_build
  "${_terreate_package_root}/core_only/core-consumer-build")
set(_core_consumer_configure
  "${CMAKE_COMMAND}"
  -S "${_core_consumer}"
  -B "${_core_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _core_consumer_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _core_consumer_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _core_consumer_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_core_prefix}")
execute_process(
  COMMAND ${_core_consumer_configure}
  RESULT_VARIABLE _core_consumer_configure_result
  OUTPUT_VARIABLE _core_consumer_configure_output
  ERROR_VARIABLE _core_consumer_configure_error)
if(NOT _core_consumer_configure_result EQUAL 0)
  message(FATAL_ERROR
    "Core-only package consumer configuration failed\n"
    "${_core_consumer_configure_output}\n${_core_consumer_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_core_consumer_build}"
  RESULT_VARIABLE _core_consumer_build_result
  OUTPUT_VARIABLE _core_consumer_build_output
  ERROR_VARIABLE _core_consumer_build_error)
if(NOT _core_consumer_build_result EQUAL 0)
  message(FATAL_ERROR
    "Core-only package consumer build failed\n"
    "${_core_consumer_build_output}\n${_core_consumer_build_error}")
endif()

set(_quiet_consumer "${_terreate_package_root}/core_only/quiet-consumer")
file(MAKE_DIRECTORY "${_quiet_consumer}")
file(WRITE "${_quiet_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateQuietMissingComponentConsumer LANGUAGES CXX)
find_package(Terreate QUIET COMPONENTS Graphics)
if(NOT DEFINED Terreate_FOUND OR Terreate_FOUND)
  message(FATAL_ERROR
    "non-REQUIRED missing component did not set Terreate_FOUND=FALSE")
endif()
if(NOT DEFINED Terreate_Graphics_FOUND OR Terreate_Graphics_FOUND)
  message(FATAL_ERROR
    "missing component did not set Terreate_Graphics_FOUND=FALSE")
endif()
if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR
   NOT "${Terreate_NOT_FOUND_MESSAGE}" MATCHES "disabled")
  message(FATAL_ERROR
    "missing component did not set Terreate_NOT_FOUND_MESSAGE")
endif()
]=])

set(_quiet_consumer_build
  "${_terreate_package_root}/core_only/quiet-consumer-build")
set(_quiet_configure
  "${CMAKE_COMMAND}"
  -S "${_quiet_consumer}"
  -B "${_quiet_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _quiet_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _quiet_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _quiet_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_core_prefix}")
execute_process(
  COMMAND ${_quiet_configure}
  RESULT_VARIABLE _quiet_result
  OUTPUT_VARIABLE _quiet_output
  ERROR_VARIABLE _quiet_error)
if(NOT _quiet_result EQUAL 0)
  message(FATAL_ERROR
    "quiet optional missing component configuration failed\n"
    "${_quiet_output}\n${_quiet_error}")
endif()

set(_disabled_consumer "${_terreate_package_root}/core_only/disabled-consumer")
file(MAKE_DIRECTORY "${_disabled_consumer}")
file(WRITE "${_disabled_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateDisabledComponentConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Graphics)
]=])

set(_disabled_consumer_build
  "${_terreate_package_root}/core_only/disabled-consumer-build")
set(_disabled_configure
  "${CMAKE_COMMAND}"
  -S "${_disabled_consumer}"
  -B "${_disabled_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _disabled_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _disabled_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _disabled_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_core_prefix}")
execute_process(
  COMMAND ${_disabled_configure}
  RESULT_VARIABLE _disabled_result
  OUTPUT_VARIABLE _disabled_output
  ERROR_VARIABLE _disabled_error)
if(_disabled_result EQUAL 0)
  message(FATAL_ERROR
    "find_package unexpectedly accepted disabled Graphics component")
endif()
string(TOLOWER "${_disabled_output}\n${_disabled_error}" _disabled_diagnostics)
if(NOT _disabled_diagnostics MATCHES "disabled|unavailable|not found|not.*built")
  message(FATAL_ERROR
    "disabled component diagnostic was not actionable:\n"
    "${_disabled_output}\n${_disabled_error}")
endif()
if(NOT "${_disabled_output}\n${_disabled_error}" MATCHES
   "TERREATE_BUILD_GRAPHICS=ON")
  message(FATAL_ERROR
    "disabled component diagnostic did not name the exact remediation option:\n"
    "${_disabled_output}\n${_disabled_error}")
endif()
