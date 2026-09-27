cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED TERREATE_SOURCE_DIR OR "${TERREATE_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_SOURCE_DIR is required")
endif()
if(NOT DEFINED TERREATE_MATRIX_BINARY_ROOT OR
   "${TERREATE_MATRIX_BINARY_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_MATRIX_BINARY_ROOT is required")
endif()

get_filename_component(_terreate_source_dir "${TERREATE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_terreate_matrix_binary_root
  "${TERREATE_MATRIX_BINARY_ROOT}" ABSOLUTE)
# The caller supplies a parent for generated state.  Recreate only this fixed
# child so an accidental non-empty parent cannot be deleted.
set(_terreate_matrix_root
  "${_terreate_matrix_binary_root}/generated")
file(REMOVE_RECURSE "${_terreate_matrix_root}")
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

# Configure the real source tree as a top-level Core-only project while CMake
# is instructed to make Vulkan discovery unavailable.  This is distinct from
# the add_subdirectory matrix below: it proves the public top-level boundary
# does not require Vulkan-Hpp or a Vulkan package when Graphics is OFF.
set(_direct_binary_dir "${_terreate_matrix_root}/direct_core_only")
set(_direct_configure_command
  "${CMAKE_COMMAND}"
  -S "${_terreate_source_dir}"
  -B "${_direct_binary_dir}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _direct_configure_command -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _direct_configure_command
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _direct_configure_command
  -DTERREATE_BUILD_CORE=ON
  -DTERREATE_BUILD_PLATFORM=OFF
  -DTERREATE_BUILD_GRAPHICS=OFF
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
execute_process(
  COMMAND ${_direct_configure_command}
  RESULT_VARIABLE _direct_configure_result
  OUTPUT_VARIABLE _direct_configure_output
  ERROR_VARIABLE _direct_configure_error)
if(NOT _direct_configure_result EQUAL 0)
  message(FATAL_ERROR
    "direct Core-only source configuration failed (${_direct_configure_result})\n"
    "${_direct_configure_output}\n${_direct_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_direct_binary_dir}"
  RESULT_VARIABLE _direct_build_result
  OUTPUT_VARIABLE _direct_build_output
  ERROR_VARIABLE _direct_build_error)
if(NOT _direct_build_result EQUAL 0)
  message(FATAL_ERROR
    "direct Core-only source build failed (${_direct_build_result})\n"
    "${_direct_build_output}\n${_direct_build_error}")
endif()

terreate_matrix_case(core_only Core ON OFF OFF)
terreate_matrix_case(platform_only Platform ON ON OFF)
terreate_matrix_case(graphics_only Graphics ON OFF ON)

# Source-subdirectory consumers must validate the effective Vulkan include
# order.  The partial directory intentionally appears before a complete
# transitive Headers directory; textual per-directory checks would select the
# later directory and defer the failure until a consumer compile.
if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
   "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "" OR
   NOT EXISTS "$ENV{VULKAN_HEADERS_INCLUDE}/vulkan/vulkan_raii.hpp")
  message(FATAL_ERROR
    "source partial-shadow fixture requires the pinned Vulkan-Hpp headers")
endif()
set(_source_partial_vulkan_root
  "${_terreate_matrix_root}/source-partial-shadow-vulkan")
set(_source_partial_vulkan_include
  "${_source_partial_vulkan_root}/partial/include")
set(_source_partial_vulkan_config_dir
  "${_source_partial_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_source_partial_vulkan_include}/vulkan"
  "${_source_partial_vulkan_config_dir}")
file(WRITE "${_source_partial_vulkan_include}/vulkan/vulkan_core.h" [=[
#pragma once

#include <cstdint>

#define VK_API_VERSION_1_3 4202496U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT 0x00000001U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT 0x00000002U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT 0x00000004U

using VkDebugUtilsMessageTypeFlagsEXT = std::uint32_t;
]=])
file(WRITE "${_source_partial_vulkan_include}/vulkan/vulkan.hpp" [=[
#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace vk {

enum class DebugUtilsMessageTypeFlagBitsEXT : std::uint32_t {
  eGeneral = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
  eValidation = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
  ePerformance = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
};

} // namespace vk
]=])
file(WRITE "${_source_partial_vulkan_include}/vulkan/vulkan_raii.hpp" [=[
#pragma once

#include <vulkan/vulkan.hpp>

namespace vk::raii {

class Context {};
class Instance {};
class DebugUtilsMessengerEXT {};

} // namespace vk::raii
]=])
file(WRITE "${_source_partial_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Headers PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"$ENV{VULKAN_HEADERS_INCLUDE}\")\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_source_partial_vulkan_include}\")\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_LINK_LIBRARIES\n"
  "  Vulkan::Headers)\n")
file(WRITE "${_source_partial_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_source_partial_binary_dir
  "${_terreate_matrix_root}/source-partial-shadow")
set(_source_partial_cmake_command
  "${CMAKE_COMMAND}"
  -S "${_terreate_source_dir}/tests/cmake/matrix-project"
  -B "${_source_partial_binary_dir}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _source_partial_cmake_command -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _source_partial_cmake_command
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _source_partial_cmake_command
  "-DTERREATE_SOURCE_DIR:PATH=${_terreate_source_dir}"
  -DTERREATE_MATRIX_COMPONENT=Graphics
  -DTERREATE_EXPECT_CORE=ON
  -DTERREATE_EXPECT_PLATFORM=OFF
  -DTERREATE_EXPECT_GRAPHICS=ON
  -DTERREATE_BUILD_CORE=ON
  -DTERREATE_BUILD_PLATFORM=OFF
  -DTERREATE_BUILD_GRAPHICS=ON
  "-DVulkan_DIR:PATH=${_source_partial_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE
  -DCMAKE_FIND_PACKAGE_NO_MODULE=TRUE)
set(_source_partial_configure
  "${CMAKE_COMMAND}" -E env --unset=VULKAN_HEADERS_INCLUDE)
list(APPEND _source_partial_configure ${_source_partial_cmake_command})
execute_process(
  COMMAND ${_source_partial_configure}
  RESULT_VARIABLE _source_partial_result
  OUTPUT_VARIABLE _source_partial_output
  ERROR_VARIABLE _source_partial_error)
if(_source_partial_result EQUAL 0)
  message(FATAL_ERROR
    "source-subdirectory partial Vulkan include order was accepted\n"
    "${_source_partial_output}\n${_source_partial_error}")
endif()
string(TOLOWER "${_source_partial_output}\n${_source_partial_error}"
  _source_partial_diagnostics)
if(NOT _source_partial_diagnostics MATCHES
     "effective|compile|partial|declaration|usage closure")
  message(FATAL_ERROR
    "source-subdirectory partial Vulkan rejection was not actionable\n"
    "${_source_partial_output}\n${_source_partial_error}")
endif()

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
