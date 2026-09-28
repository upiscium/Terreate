cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED TERREATE_SOURCE_DIR OR "${TERREATE_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_SOURCE_DIR is required")
endif()
if(NOT DEFINED TERREATE_PACKAGE_BINARY_ROOT OR
   "${TERREATE_PACKAGE_BINARY_ROOT}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_PACKAGE_BINARY_ROOT is required")
endif()

get_filename_component(_terreate_source_dir "${TERREATE_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_terreate_package_binary_root
  "${TERREATE_PACKAGE_BINARY_ROOT}" ABSOLUTE)
# The caller supplies a parent for generated state.  Recreate only this fixed
# child so an accidental non-empty parent cannot be deleted.
set(_terreate_package_root
  "${_terreate_package_binary_root}/generated")
file(REMOVE_RECURSE "${_terreate_package_root}")
file(MAKE_DIRECTORY "${_terreate_package_root}")

function(terreate_assert_install_boundary prefix name)
  foreach(_public_header IN ITEMS
      "include/terreate/core/error.hpp"
      "include/terreate/core/result.hpp"
      "include/terreate/core/diagnostics.hpp")
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
          "^include/terreate/(core/(diagnostics|error|result)|graphics/instance)\\.hpp$")
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
  set(${name}_BUILD_CONFIG_DIR
    "${_fixture_build}/terreate" PARENT_SCOPE)
endfunction()

terreate_package_configure_and_install(all_components ON ON)
set(_all_prefix "${all_components_PREFIX}")
if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
   "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  message(FATAL_ERROR
    "build-tree package regression requires an explicit Vulkan-Hpp root")
endif()
set(_build_config_file
  "${all_components_BUILD_CONFIG_DIR}/TerreateConfig.cmake")
if(NOT EXISTS "${_build_config_file}")
  message(FATAL_ERROR "build-tree package config is missing")
endif()
file(READ "${_build_config_file}" _build_config_contents)
string(FIND "${_build_config_contents}" "$ENV{VULKAN_HEADERS_INCLUDE}"
  _build_hpp_root_position)
if(_build_hpp_root_position EQUAL -1)
  message(FATAL_ERROR
    "build-tree package config omitted the explicit Vulkan-Hpp root")
endif()
file(GLOB_RECURSE _installed_config_candidates
  "${_all_prefix}/*/TerreateConfig.cmake")
if(NOT _installed_config_candidates)
  message(FATAL_ERROR "installed package config is missing")
endif()
list(GET _installed_config_candidates 0 _installed_config_file)
file(READ "${_installed_config_file}" _installed_config_contents)
string(FIND "${_installed_config_contents}" "$ENV{VULKAN_HEADERS_INCLUDE}"
  _installed_hpp_root_position)
if(NOT _installed_hpp_root_position EQUAL -1)
  message(FATAL_ERROR
    "installed package config leaked the build-tree Vulkan-Hpp root")
endif()
string(FIND "${_installed_config_contents}" "${_terreate_source_dir}"
  _installed_source_root_position)
if(NOT _installed_source_root_position EQUAL -1)
  message(FATAL_ERROR
    "installed package config leaked the Terreate source root")
endif()
if(NOT EXISTS "${_all_prefix}/include/terreate/graphics/instance.hpp")
  message(FATAL_ERROR "Graphics install is missing its public instance header")
endif()
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
foreach(_component IN ITEMS Core Graphics)
  if(NOT TARGET Terreate::${_component})
    message(FATAL_ERROR
      "explicit Core+Graphics package request did not import Terreate::${_component}")
  endif()
endforeach()
if(TARGET Terreate::Platform)
  message(FATAL_ERROR
    "explicit Core+Graphics package request imported unrequested Platform")
endif()
if(NOT Vulkan_FOUND OR NOT TARGET Vulkan::Vulkan)
  message(FATAL_ERROR
    "all-components installed package did not discover Vulkan::Vulkan")
endif()
get_target_property(_installed_graphics_links Terreate::Graphics
  INTERFACE_LINK_LIBRARIES)
if(NOT "${_installed_graphics_links}" MATCHES "Core")
  message(FATAL_ERROR
    "installed Graphics target lost its public Core dependency: "
    "${_installed_graphics_links}")
endif()
if(NOT "${_installed_graphics_links}" MATCHES "Vulkan::Vulkan")
  message(FATAL_ERROR
    "installed Graphics target lost its public Vulkan dependency: "
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
#include <terreate/core/diagnostics.hpp>
#include <terreate/core/result.hpp>

#include <system_error>

int main() {
  terreate::Result<int> result = 7;
  terreate::DiagnosticEvent event{
      .categories = {"package-specific"},
      .message = "package smoke"};
  terreate::DiagnosticSinkView sink;
  sink.emit(event);
  return terreate::unwrap(result) == 7 && event.message == "package smoke" &&
                 event.categories.size() == 1 && event.categories.front() == "package-specific"
             ? 0
             : 1;
}
]=])
file(WRITE "${_all_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

static_assert(VK_API_VERSION_1_3 != 0);
static_assert(sizeof(vk::raii::Context) > 0);

int main() {
  const auto capabilities = terreate::graphics::queryInstanceCapabilities();
  if (!capabilities) {
    return 1;
  }

  terreate::graphics::InstanceDescription description;
  description.api_version = capabilities->loader_api_version;
  const auto plan = terreate::graphics::resolveInstance(description, *capabilities);
  if (!plan) {
    return 1;
  }

  const auto instance = terreate::graphics::createInstance(*plan);
  return instance && instance->valid() ? 0 : 1;
}
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
execute_process(
  COMMAND "${_all_consumer_build}/package_consumer"
  RESULT_VARIABLE _consumer_run_result
  OUTPUT_VARIABLE _consumer_run_output
  ERROR_VARIABLE _consumer_run_error)
if(NOT _consumer_run_result EQUAL 0)
  message(FATAL_ERROR
    "installed package consumer runtime failed\n"
    "${_consumer_run_output}\n${_consumer_run_error}")
endif()

# The build-tree package must remain usable when Vulkan::Vulkan contributes
# only the loader.  Its explicit Vulkan-Hpp BUILD_INTERFACE root is the only
# header path available to this consumer.
set(_build_tree_loader_vulkan_root
  "${_terreate_package_root}/build-tree-loader-vulkan")
set(_build_tree_loader_vulkan_config_dir
  "${_build_tree_loader_vulkan_root}/lib/cmake/Vulkan")
find_library(_build_tree_loader_vulkan_library NAMES vulkan)
if(NOT _build_tree_loader_vulkan_library)
  message(FATAL_ERROR
    "build-tree loader-only Vulkan fixture could not locate a Vulkan loader")
endif()
file(MAKE_DIRECTORY "${_build_tree_loader_vulkan_config_dir}")
file(WRITE "${_build_tree_loader_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan UNKNOWN IMPORTED)\n"
  "set_target_properties(Vulkan::Vulkan PROPERTIES\n"
  "  IMPORTED_LOCATION \"${_build_tree_loader_vulkan_library}\")\n")
file(WRITE
  "${_build_tree_loader_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

# The build-tree package config has a different public-header root from the
# installed config.  Consume it through Terreate_DIR so this check cannot fall
# back to the installed package while compiling the public Graphics header.
set(_build_tree_consumer
  "${_terreate_package_root}/all_components/build-tree-consumer")
file(MAKE_DIRECTORY "${_build_tree_consumer}")
file(WRITE "${_build_tree_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateBuildTreeConsumer LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS Graphics)
if(NOT TARGET Terreate::Core OR NOT TARGET Terreate::Graphics)
  message(FATAL_ERROR
    "build-tree package did not import the Graphics dependency closure")
endif()
if(TARGET Terreate::Platform)
  message(FATAL_ERROR
    "build-tree Graphics request imported unrequested Platform")
endif()
get_target_property(_build_tree_graphics_includes Terreate::Graphics
  INTERFACE_INCLUDE_DIRECTORIES)
list(FIND _build_tree_graphics_includes "$ENV{VULKAN_HEADERS_INCLUDE}"
  _build_tree_hpp_include_index)
if(_build_tree_hpp_include_index EQUAL -1)
  message(FATAL_ERROR
    "build-tree Graphics target omitted its explicit Vulkan-Hpp BUILD_INTERFACE")
endif()
if(TARGET Vulkan::Headers)
  message(FATAL_ERROR
    "loader-only build-tree Vulkan fixture unexpectedly provided Vulkan::Headers")
endif()
add_executable(build_tree_consumer main.cpp)
target_link_libraries(build_tree_consumer PRIVATE Terreate::Graphics)
set_target_properties(build_tree_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_build_tree_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>

int main() {
  terreate::graphics::InstanceDescription description;
  return description.debug_utils == terreate::graphics::DebugUtilsMode::disabled
             ? 0
             : 1;
}
]=])

set(_build_tree_consumer_build
  "${_terreate_package_root}/all_components/build-tree-consumer-build")
file(REMOVE_RECURSE "${_build_tree_consumer_build}")
set(_build_tree_configure
  "${CMAKE_COMMAND}"
  -S "${_build_tree_consumer}"
  -B "${_build_tree_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _build_tree_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _build_tree_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _build_tree_configure
  "-DTerreate_DIR:PATH=${all_components_BUILD_CONFIG_DIR}"
  "-DVulkan_DIR:PATH=${_build_tree_loader_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE
  -DCMAKE_FIND_PACKAGE_NO_MODULE=TRUE)
execute_process(
  COMMAND ${_build_tree_configure}
  RESULT_VARIABLE _build_tree_configure_result
  OUTPUT_VARIABLE _build_tree_configure_output
  ERROR_VARIABLE _build_tree_configure_error)
if(NOT _build_tree_configure_result EQUAL 0)
  message(FATAL_ERROR
    "build-tree package consumer configuration failed\n"
    "${_build_tree_configure_output}\n${_build_tree_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_build_tree_consumer_build}"
  RESULT_VARIABLE _build_tree_build_result
  OUTPUT_VARIABLE _build_tree_build_output
  ERROR_VARIABLE _build_tree_build_error)
if(NOT _build_tree_build_result EQUAL 0)
  message(FATAL_ERROR
    "build-tree package consumer build failed\n"
    "${_build_tree_build_output}\n${_build_tree_build_error}")
endif()

# With no explicit component list, an all-components installation imports all
# built targets and their exact dependency closure.  Keep this assertion
# separate from the explicit Core+Graphics request above so importing
# unrequested Platform cannot hide a package-config regression.
set(_all_default_consumer
  "${_terreate_package_root}/all_components/default-consumer")
file(MAKE_DIRECTORY "${_all_default_consumer}")
file(WRITE "${_all_default_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateAllComponentsPackageConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED)
foreach(_component IN ITEMS Core Platform Graphics)
  if(NOT TARGET Terreate::${_component})
    message(FATAL_ERROR
      "all-components package did not import Terreate::${_component}")
  endif()
endforeach()
if(NOT Vulkan_FOUND OR NOT TARGET Vulkan::Vulkan)
  message(FATAL_ERROR
    "all-components package did not discover Vulkan::Vulkan")
endif()
]=])

set(_all_default_consumer_build
  "${_terreate_package_root}/all_components/default-consumer-build")
set(_all_default_configure
  "${CMAKE_COMMAND}"
  -S "${_all_default_consumer}"
  -B "${_all_default_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _all_default_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _all_default_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _all_default_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_all_default_configure}
  RESULT_VARIABLE _all_default_configure_result
  OUTPUT_VARIABLE _all_default_configure_output
  ERROR_VARIABLE _all_default_configure_error)
if(NOT _all_default_configure_result EQUAL 0)
  message(FATAL_ERROR
    "all-components default package consumer configuration failed\n"
    "${_all_default_configure_output}\n${_all_default_configure_error}")
endif()

# A dependency package is allowed to define PACKAGE_PREFIX_DIR for itself, but
# it must not redirect Terreate's own public-header probe.  This fixture
# emulates the CMake 3.28/3.29 behavior where a nested package config writes
# that variable in the caller's scope.
if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
   "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "" OR
   NOT EXISTS "$ENV{VULKAN_HEADERS_INCLUDE}/vulkan/vulkan_raii.hpp")
  message(FATAL_ERROR
    "prefix-overwrite Vulkan fixture requires the pinned Vulkan-Hpp headers")
endif()
set(_prefix_overwrite_vulkan_root
  "${_terreate_package_root}/prefix-overwrite-vulkan")
set(_prefix_overwrite_vulkan_config_dir
  "${_prefix_overwrite_vulkan_root}/lib/cmake/Vulkan")
set(_prefix_overwrite_vulkan_fake_prefix
  "${_prefix_overwrite_vulkan_root}/fake-prefix")
file(MAKE_DIRECTORY "${_prefix_overwrite_vulkan_config_dir}")
file(WRITE "${_prefix_overwrite_vulkan_config_dir}/VulkanConfig.cmake"
  "set(PACKAGE_PREFIX_DIR \"${_prefix_overwrite_vulkan_fake_prefix}\")\n"
  "set(PACKAGE_PREFIX_DIR \"${_prefix_overwrite_vulkan_fake_prefix}\" PARENT_SCOPE)\n"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"$ENV{VULKAN_HEADERS_INCLUDE}\")\n")
file(WRITE "${_prefix_overwrite_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_prefix_overwrite_consumer
  "${_terreate_package_root}/all_components/prefix-overwrite-consumer")
file(MAKE_DIRECTORY "${_prefix_overwrite_consumer}")
file(WRITE "${_prefix_overwrite_consumer}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\n"
  "project(TerreatePrefixOverwriteConsumer LANGUAGES CXX)\n"
  "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
  "set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)\n"
  "find_package(Terreate REQUIRED COMPONENTS Core Graphics)\n"
  "if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR\n"
  "   NOT Terreate_Graphics_FOUND OR\n"
  "   NOT TARGET Terreate::Core OR NOT TARGET Terreate::Graphics)\n"
  "  message(FATAL_ERROR\n"
  "    \"dependency prefix overwrite made Terreate Graphics unavailable\")\n"
  "endif()\n")

set(_prefix_overwrite_consumer_build
  "${_terreate_package_root}/all_components/prefix-overwrite-consumer-build")
file(REMOVE_RECURSE "${_prefix_overwrite_consumer_build}")
set(_prefix_overwrite_configure
  "${CMAKE_COMMAND}"
  -S "${_prefix_overwrite_consumer}"
  -B "${_prefix_overwrite_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _prefix_overwrite_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _prefix_overwrite_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _prefix_overwrite_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_prefix_overwrite_vulkan_config_dir}")
execute_process(
  COMMAND ${_prefix_overwrite_configure}
  RESULT_VARIABLE _prefix_overwrite_result
  OUTPUT_VARIABLE _prefix_overwrite_output
  ERROR_VARIABLE _prefix_overwrite_error)
if(NOT _prefix_overwrite_result EQUAL 0)
  message(FATAL_ERROR
    "Terreate prefix-overwrite consumer configuration failed\n"
    "${_prefix_overwrite_output}\n${_prefix_overwrite_error}")
endif()

set(_optional_graphics_consumer
  "${_terreate_package_root}/all_components/optional-graphics-consumer")
file(MAKE_DIRECTORY "${_optional_graphics_consumer}")
file(WRITE "${_optional_graphics_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateOptionalGraphicsWithoutVulkan LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Core OPTIONAL_COMPONENTS Graphics)
if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR
   NOT TARGET Terreate::Core)
  message(FATAL_ERROR
    "required Core was rejected while optional Graphics lacked Vulkan")
endif()
if(Terreate_Graphics_FOUND OR TARGET Terreate::Graphics)
  message(FATAL_ERROR
    "optional Graphics was imported despite unavailable Vulkan")
endif()
if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR
   NOT "${Terreate_NOT_FOUND_MESSAGE}" MATCHES "Vulkan")
  message(FATAL_ERROR
    "optional Graphics status did not retain the Vulkan diagnostic")
endif()
add_executable(optional_graphics_consumer main.cpp)
target_link_libraries(optional_graphics_consumer PRIVATE Terreate::Core)
set_target_properties(optional_graphics_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_optional_graphics_consumer}/main.cpp" [=[
#include <terreate/core/result.hpp>

int main() {
  terreate::Result<int> result = 31;
  return terreate::unwrap(result) == 31 ? 0 : 1;
}
]=])

set(_optional_graphics_consumer_build
  "${_terreate_package_root}/all_components/optional-graphics-consumer-build")
set(_optional_graphics_configure
  "${CMAKE_COMMAND}"
  -S "${_optional_graphics_consumer}"
  -B "${_optional_graphics_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _optional_graphics_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _optional_graphics_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _optional_graphics_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
execute_process(
  COMMAND ${_optional_graphics_configure}
  RESULT_VARIABLE _optional_graphics_configure_result
  OUTPUT_VARIABLE _optional_graphics_configure_output
  ERROR_VARIABLE _optional_graphics_configure_error)
if(NOT _optional_graphics_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed optional Graphics consumer configuration failed\n"
    "${_optional_graphics_configure_output}\n"
    "${_optional_graphics_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_optional_graphics_consumer_build}"
  RESULT_VARIABLE _optional_graphics_build_result
  OUTPUT_VARIABLE _optional_graphics_build_output
  ERROR_VARIABLE _optional_graphics_build_error)
if(NOT _optional_graphics_build_result EQUAL 0)
  message(FATAL_ERROR
    "installed optional Graphics consumer build failed\n"
    "${_optional_graphics_build_output}\n"
    "${_optional_graphics_build_error}")
endif()

set(_required_graphics_consumer
  "${_terreate_package_root}/all_components/required-graphics-without-vulkan")
file(MAKE_DIRECTORY "${_required_graphics_consumer}")
file(WRITE "${_required_graphics_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateRequiredGraphicsWithoutVulkan LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Graphics)
]=])

set(_required_graphics_consumer_build
  "${_terreate_package_root}/all_components/required-graphics-without-vulkan-build")
set(_required_graphics_configure
  "${CMAKE_COMMAND}"
  -S "${_required_graphics_consumer}"
  -B "${_required_graphics_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _required_graphics_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _required_graphics_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _required_graphics_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
execute_process(
  COMMAND ${_required_graphics_configure}
  RESULT_VARIABLE _required_graphics_configure_result
  OUTPUT_VARIABLE _required_graphics_configure_output
  ERROR_VARIABLE _required_graphics_configure_error)
if(_required_graphics_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed required Graphics consumer unexpectedly configured without Vulkan\n"
    "${_required_graphics_configure_output}\n"
    "${_required_graphics_configure_error}")
endif()
string(TOLOWER
  "${_required_graphics_configure_output}\n${_required_graphics_configure_error}"
  _required_graphics_diagnostics)
if(NOT _required_graphics_diagnostics MATCHES "vulkan")
  message(FATAL_ERROR
    "required Graphics failure did not identify Vulkan discovery as the cause\n"
    "${_required_graphics_configure_output}\n"
    "${_required_graphics_configure_error}")
endif()

# A valid Vulkan package may publish Vulkan-Hpp through a separate Headers
# target.  Keep the loader target itself headerless so this fixture proves the
# package check follows Vulkan::Vulkan's linked usage closure.
set(_linked_headers_vulkan_root "${_terreate_package_root}/linked-headers-vulkan")
set(_linked_headers_vulkan_loader_include
  "${_linked_headers_vulkan_root}/loader/include")
set(_linked_headers_vulkan_include
  "${_linked_headers_vulkan_root}/headers/include")
set(_linked_headers_vulkan_config_dir
  "${_linked_headers_vulkan_root}/lib/cmake/Vulkan")
find_library(_linked_headers_vulkan_library NAMES vulkan)
if(NOT _linked_headers_vulkan_library)
  message(FATAL_ERROR
    "linked Vulkan::Headers fixture could not locate a Vulkan loader library")
endif()
file(MAKE_DIRECTORY "${_linked_headers_vulkan_loader_include}"
  "${_linked_headers_vulkan_include}/vulkan"
  "${_linked_headers_vulkan_config_dir}")
file(WRITE "${_linked_headers_vulkan_include}/vulkan/vulkan_core.h" [=[
#pragma once

#include <cstdint>

#define VK_API_VERSION_1_0 4194304U
#define VK_API_VERSION_1_3 4202496U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT 0x00000001U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT 0x00000002U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT 0x00000004U

using VkDebugUtilsMessageTypeFlagsEXT = std::uint32_t;
]=])
file(WRITE "${_linked_headers_vulkan_include}/vulkan/vulkan.hpp" [=[
#pragma once

#include <cstdint>
#include <system_error>

#include <vulkan/vulkan_core.h>

namespace vk {

enum class DebugUtilsMessageTypeFlagBitsEXT : std::uint32_t {
  eGeneral = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
  eValidation = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
  ePerformance = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
};

using DebugUtilsMessageTypeFlagsEXT = std::uint32_t;

struct Instance {};

class SystemError : public std::system_error {
public:
  using std::system_error::system_error;
};

} // namespace vk
]=])
file(WRITE "${_linked_headers_vulkan_include}/vulkan/vulkan_raii.hpp" [=[
#pragma once

#include <vulkan/vulkan.hpp>

namespace vk::raii {

class Context {};
class Instance {};
class DebugUtilsMessengerEXT {};

} // namespace vk::raii
]=])
file(WRITE "${_linked_headers_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Headers PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_linked_headers_vulkan_include}\")\n"
  "add_library(Vulkan::Vulkan UNKNOWN IMPORTED)\n"
  "set_target_properties(Vulkan::Vulkan PROPERTIES\n"
  "  IMPORTED_LOCATION \"${_linked_headers_vulkan_library}\"\n"
  "  INTERFACE_INCLUDE_DIRECTORIES \"${_linked_headers_vulkan_loader_include}\")\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_LINK_LIBRARIES\n"
  "  Vulkan::Headers)\n")
file(WRITE "${_linked_headers_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_linked_headers_consumer
  "${_terreate_package_root}/all_components/linked-headers-consumer")
file(MAKE_DIRECTORY "${_linked_headers_consumer}")
file(WRITE "${_linked_headers_consumer}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\n"
  "project(TerreateLinkedVulkanHeaders LANGUAGES CXX)\n"
  "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
  "find_package(Terreate REQUIRED COMPONENTS Core OPTIONAL_COMPONENTS Graphics)\n"
  "if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR\n"
  "   NOT Terreate_Graphics_FOUND OR\n"
  "   NOT TARGET Terreate::Core OR NOT TARGET Terreate::Graphics)\n"
  "  message(FATAL_ERROR\n"
  "    \"linked Vulkan::Headers fixture did not enable Graphics\")\n"
  "endif()\n"
  "if(NOT TARGET Vulkan::Vulkan OR NOT TARGET Vulkan::Headers)\n"
  "  message(FATAL_ERROR\n"
  "    \"linked Vulkan::Headers fixture did not provide both Vulkan targets\")\n"
  "endif()\n"
  "get_target_property(_linked_graphics_links Terreate::Graphics\n"
  "  INTERFACE_LINK_LIBRARIES)\n"
  "if(NOT \"\${_linked_graphics_links}\" MATCHES \"Vulkan::Vulkan\")\n"
  "  message(FATAL_ERROR\n"
  "    \"installed Graphics target lost its Vulkan::Vulkan dependency\")\n"
  "endif()\n"
  "get_target_property(_linked_core_links Terreate::Core\n"
  "  INTERFACE_LINK_LIBRARIES)\n"
  "if(\"\${_linked_core_links}\" MATCHES \"Vulkan\")\n"
  "  message(FATAL_ERROR\n"
  "    \"Core acquired an unintended Vulkan dependency\")\n"
  "endif()\n"
  "add_executable(linked_headers_consumer main.cpp)\n"
  "target_link_libraries(linked_headers_consumer PRIVATE Terreate::Graphics)\n"
  "set_target_properties(linked_headers_consumer PROPERTIES\n"
  "  CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)\n")
file(WRITE "${_linked_headers_consumer}/main.cpp"
  "#include <terreate/graphics/instance.hpp>\n"
  "#include <vulkan/vulkan.hpp>\n"
  "#include <vulkan/vulkan_core.h>\n"
  "#include <vulkan/vulkan_raii.hpp>\n"
  "#include <cstdint>\n"
  "static_assert(VK_API_VERSION_1_3 != 0);\n"
  "static_assert(sizeof(vk::raii::Context) > 0);\n"
  "int main() {\n"
  "  const auto capabilities =\n"
  "      terreate::graphics::queryInstanceCapabilities();\n"
  "  return capabilities.has_value() ? 0 : 1;\n"
  "}\n")

set(_linked_headers_consumer_build
  "${_terreate_package_root}/all_components/linked-headers-consumer-build")
file(REMOVE_RECURSE "${_linked_headers_consumer_build}")
set(_linked_headers_configure
  "${CMAKE_COMMAND}"
  -S "${_linked_headers_consumer}"
  -B "${_linked_headers_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _linked_headers_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _linked_headers_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _linked_headers_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_linked_headers_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
execute_process(
  COMMAND ${_linked_headers_configure}
  RESULT_VARIABLE _linked_headers_configure_result
  OUTPUT_VARIABLE _linked_headers_configure_output
  ERROR_VARIABLE _linked_headers_configure_error)
if(NOT _linked_headers_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed linked Vulkan::Headers consumer configuration failed\n"
    "${_linked_headers_configure_output}\n"
    "${_linked_headers_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_linked_headers_consumer_build}"
  RESULT_VARIABLE _linked_headers_build_result
  OUTPUT_VARIABLE _linked_headers_build_output
  ERROR_VARIABLE _linked_headers_build_error)
if(NOT _linked_headers_build_result EQUAL 0)
  message(FATAL_ERROR
    "installed linked Vulkan::Headers consumer build failed\n"
    "${_linked_headers_build_output}\n"
    "${_linked_headers_build_error}")
endif()

# A partial include directory earlier in Vulkan::Vulkan's effective include
# order must shadow the valid transitive Headers directory and reject optional
# Graphics.  A per-directory textual check would incorrectly select the later
# complete directory and accept this package.
set(_partial_shadow_vulkan_root
  "${_terreate_package_root}/partial-shadow-vulkan")
set(_partial_shadow_vulkan_include
  "${_partial_shadow_vulkan_root}/partial/include")
set(_partial_shadow_vulkan_config_dir
  "${_partial_shadow_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_partial_shadow_vulkan_include}/vulkan"
  "${_partial_shadow_vulkan_config_dir}")
file(WRITE "${_partial_shadow_vulkan_include}/vulkan/vulkan_core.h" [=[
#pragma once

#include <cstdint>

#define VK_API_VERSION_1_3 4202496U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT 0x00000001U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT 0x00000002U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT 0x00000004U

using VkDebugUtilsMessageTypeFlagsEXT = std::uint32_t;
]=])
file(WRITE "${_partial_shadow_vulkan_include}/vulkan/vulkan.hpp" [=[
#pragma once

#include <vulkan/vulkan_core.h>

namespace vk {

enum class DebugUtilsMessageTypeFlagBitsEXT : std::uint32_t {
  eGeneral = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
  eValidation = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
  ePerformance = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
};

} // namespace vk
]=])
file(WRITE "${_partial_shadow_vulkan_include}/vulkan/vulkan_raii.hpp" [=[
#pragma once

#include <vulkan/vulkan.hpp>

namespace vk::raii {

class Context {};
class Instance {};
class DebugUtilsMessengerEXT {};

} // namespace vk::raii
]=])
file(WRITE "${_partial_shadow_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Headers PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_linked_headers_vulkan_include}\")\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_partial_shadow_vulkan_include}\")\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_LINK_LIBRARIES\n"
  "  Vulkan::Headers)\n")
file(WRITE "${_partial_shadow_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_partial_shadow_consumer
  "${_terreate_package_root}/all_components/partial-shadow-consumer")
file(MAKE_DIRECTORY "${_partial_shadow_consumer}")
file(WRITE "${_partial_shadow_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreatePartialShadowVulkan LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS Core OPTIONAL_COMPONENTS Graphics)
if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR
   NOT TARGET Terreate::Core)
  message(FATAL_ERROR
    "required Core was rejected by the partial Vulkan shadow package")
endif()
if(Terreate_Graphics_FOUND OR TARGET Terreate::Graphics)
  message(FATAL_ERROR
    "optional Graphics accepted an earlier partial Vulkan include directory")
endif()
if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR
   NOT "${Terreate_NOT_FOUND_MESSAGE}" MATCHES "compilable|declaration|usage closure")
  message(FATAL_ERROR
    "partial Vulkan rejection did not preserve the compile-probe diagnostic")
endif()
add_executable(partial_shadow_core_consumer main.cpp)
target_link_libraries(partial_shadow_core_consumer PRIVATE Terreate::Core)
set_target_properties(partial_shadow_core_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_partial_shadow_consumer}/main.cpp" [=[
#include <terreate/core/result.hpp>

int main() {
  terreate::Result<int> result = 43;
  return terreate::unwrap(result) == 43 ? 0 : 1;
}
]=])

set(_partial_shadow_consumer_build
  "${_terreate_package_root}/all_components/partial-shadow-consumer-build")
file(REMOVE_RECURSE "${_partial_shadow_consumer_build}")
set(_partial_shadow_configure
  "${CMAKE_COMMAND}"
  -S "${_partial_shadow_consumer}"
  -B "${_partial_shadow_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _partial_shadow_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _partial_shadow_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _partial_shadow_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_partial_shadow_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE
  -DCMAKE_FIND_PACKAGE_NO_MODULE=TRUE)
execute_process(
  COMMAND ${_partial_shadow_configure}
  RESULT_VARIABLE _partial_shadow_configure_result
  OUTPUT_VARIABLE _partial_shadow_configure_output
  ERROR_VARIABLE _partial_shadow_configure_error)
if(NOT _partial_shadow_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed partial-shadow optional Graphics configuration failed\n"
    "${_partial_shadow_configure_output}\n"
    "${_partial_shadow_configure_error}")
endif()
if(EXISTS "${_partial_shadow_consumer_build}/CMakeFiles/TerreateVulkanHeadersProbe")
  message(FATAL_ERROR
    "partial-shadow Vulkan compile probe leaked its temporary directory")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_partial_shadow_consumer_build}"
  RESULT_VARIABLE _partial_shadow_build_result
  OUTPUT_VARIABLE _partial_shadow_build_output
  ERROR_VARIABLE _partial_shadow_build_error)
if(NOT _partial_shadow_build_result EQUAL 0)
  message(FATAL_ERROR
    "installed partial-shadow Core consumer build failed\n"
    "${_partial_shadow_build_output}\n"
    "${_partial_shadow_build_error}")
endif()

function(terreate_expect_installed_graphics_rejection name vulkan_config_dir)
  set(_consumer
    "${_terreate_package_root}/all_components/${name}-consumer")
  file(MAKE_DIRECTORY "${_consumer}")
  file(WRITE "${_consumer}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.28)\n"
    "project(Terreate${name} LANGUAGES CXX)\n"
    "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
    "set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)\n"
    "find_package(Terreate REQUIRED COMPONENTS Graphics)\n")

  set(_consumer_build
    "${_terreate_package_root}/all_components/${name}-consumer-build")
  file(REMOVE_RECURSE "${_consumer_build}")
  set(_configure
    "${CMAKE_COMMAND}"
    -S "${_consumer}"
    -B "${_consumer_build}")
  if(DEFINED TERREATE_CMAKE_GENERATOR AND
     NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
    list(APPEND _configure -G "${TERREATE_CMAKE_GENERATOR}")
  endif()
  if(DEFINED TERREATE_CXX_COMPILER AND
     NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
    list(APPEND _configure
      "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
  endif()
  list(APPEND _configure
    "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
    "-DVulkan_DIR:PATH=${vulkan_config_dir}"
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
  execute_process(
    COMMAND ${_configure}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(_configure_result EQUAL 0)
    message(FATAL_ERROR
      "installed ${name} partial Vulkan package unexpectedly accepted Graphics\n"
      "${_configure_output}\n${_configure_error}")
  endif()
  string(TOLOWER "${_configure_output}\n${_configure_error}" _diagnostics)
  if(NOT _diagnostics MATCHES
       "vulkan/vulkan_raii|partial|decoy")
    message(FATAL_ERROR
      "installed ${name} rejection did not identify the incomplete Vulkan "
      "declarations\n${_configure_output}\n${_configure_error}")
  endif()
endfunction()

# A complete Vulkan::Vulkan closure must not hide an earlier, partial legacy
# Vulkan_INCLUDE_DIR.  The installed Graphics target publishes that legacy
# path directly, before Vulkan::Vulkan's transitive includes; the config probe
# must therefore reject this conflicting package instead of accepting a
# consumer that would select the stale headers.
set(_legacy_partial_shadow_vulkan_root
  "${_terreate_package_root}/legacy-partial-shadow-vulkan")
set(_legacy_partial_shadow_vulkan_config_dir
  "${_legacy_partial_shadow_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_legacy_partial_shadow_vulkan_config_dir}")
file(WRITE "${_legacy_partial_shadow_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "set(Vulkan_INCLUDE_DIR \"${_partial_shadow_vulkan_include}\")\n"
  "set(Vulkan_INCLUDE_DIRS \"${_partial_shadow_vulkan_include}\")\n"
  "set(Vulkan_LIBRARY \"${_linked_headers_vulkan_library}\")\n"
  "add_library(Vulkan::Vulkan UNKNOWN IMPORTED)\n"
  "set_target_properties(Vulkan::Vulkan PROPERTIES\n"
  "  IMPORTED_LOCATION \"${_linked_headers_vulkan_library}\"\n"
  "  INTERFACE_INCLUDE_DIRECTORIES \"${_linked_headers_vulkan_include}\")\n")
file(WRITE
  "${_legacy_partial_shadow_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")
terreate_expect_installed_graphics_rejection(
  legacy-partial-shadow "${_legacy_partial_shadow_vulkan_config_dir}")

function(terreate_expect_installed_graphics_acceptance name vulkan_config_dir)
  set(_consumer
    "${_terreate_package_root}/all_components/${name}-consumer")
  file(MAKE_DIRECTORY "${_consumer}")
  file(WRITE "${_consumer}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.28)\n"
    "project(Terreate${name} LANGUAGES CXX)\n"
    "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
    "set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)\n"
    "find_package(Terreate REQUIRED COMPONENTS Graphics)\n"
    "if(NOT TARGET Terreate::Graphics)\n"
    "  message(FATAL_ERROR \"accepted Vulkan package omitted Graphics\")\n"
    "endif()\n")

  set(_consumer_build
    "${_terreate_package_root}/all_components/${name}-consumer-build")
  file(REMOVE_RECURSE "${_consumer_build}")
  set(_configure
    "${CMAKE_COMMAND}"
    -S "${_consumer}"
    -B "${_consumer_build}")
  if(DEFINED TERREATE_CMAKE_GENERATOR AND
     NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
    list(APPEND _configure -G "${TERREATE_CMAKE_GENERATOR}")
  endif()
  if(DEFINED TERREATE_CXX_COMPILER AND
     NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
    list(APPEND _configure
      "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
  endif()
  list(APPEND _configure
    "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
    "-DVulkan_DIR:PATH=${vulkan_config_dir}"
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
  execute_process(
    COMMAND ${_configure}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "installed ${name} Vulkan package was rejected unexpectedly\n"
      "${_configure_output}\n${_configure_error}")
  endif()
endfunction()

function(terreate_expect_installed_graphics_legacy_include
    name vulkan_config_dir expected_include_dir)
  set(_consumer
    "${_terreate_package_root}/all_components/${name}-consumer")
  file(MAKE_DIRECTORY "${_consumer}")
  file(WRITE "${_consumer}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.28)\n"
    "project(Terreate${name} LANGUAGES CXX)\n"
    "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
    "set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)\n"
    "find_package(Terreate REQUIRED COMPONENTS Graphics)\n"
    "if(NOT TARGET Terreate::Graphics)\n"
    "  message(FATAL_ERROR \"legacy Vulkan include fixture omitted Graphics\")\n"
    "endif()\n"
    "get_target_property(_legacy_graphics_includes Terreate::Graphics\n"
    "  INTERFACE_INCLUDE_DIRECTORIES)\n"
    "get_target_property(_legacy_graphics_system_includes Terreate::Graphics\n"
    "  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)\n"
    "set(_legacy_graphics_all_includes\n"
    "  \"\${_legacy_graphics_includes};\${_legacy_graphics_system_includes}\")\n"
    "list(FIND _legacy_graphics_all_includes \"${expected_include_dir}\"\n"
    "  _legacy_graphics_include_index)\n"
    "if(_legacy_graphics_include_index EQUAL -1)\n"
    "  message(FATAL_ERROR \"validated legacy Vulkan include path was not propagated to Graphics: "
    "\${_legacy_graphics_all_includes}\")\n"
    "endif()\n"
    "add_executable(legacy_vulkan_include_consumer main.cpp)\n"
    "target_link_libraries(legacy_vulkan_include_consumer PRIVATE Terreate::Graphics)\n"
    "set_target_properties(legacy_vulkan_include_consumer PROPERTIES\n"
    "  CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)\n")
  file(WRITE "${_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <vulkan/vulkan_raii.hpp>

static_assert(sizeof(vk::raii::Context) > 0);

int main() { return 0; }
]=])

  set(_consumer_build
    "${_terreate_package_root}/all_components/${name}-consumer-build")
  file(REMOVE_RECURSE "${_consumer_build}")
  set(_configure
    "${CMAKE_COMMAND}"
    -S "${_consumer}"
    -B "${_consumer_build}")
  if(DEFINED TERREATE_CMAKE_GENERATOR AND
     NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
    list(APPEND _configure -G "${TERREATE_CMAKE_GENERATOR}")
  endif()
  if(DEFINED TERREATE_CXX_COMPILER AND
     NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
    list(APPEND _configure
      "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
  endif()
  list(APPEND _configure
    "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
    "-DVulkan_DIR:PATH=${vulkan_config_dir}"
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE
    -DCMAKE_FIND_PACKAGE_NO_MODULE=TRUE)
  execute_process(
    COMMAND ${_configure}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "installed ${name} legacy Vulkan include consumer configuration failed\n"
      "${_configure_output}\n${_configure_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
      "installed ${name} legacy Vulkan include consumer build failed\n"
      "${_build_output}\n${_build_error}")
  endif()
endfunction()

# FindVulkan-compatible legacy variables can describe a complete Vulkan-Hpp
# tree even when Vulkan::Vulkan publishes only the loader.  The installed
# package must either reject that package or propagate the validated path to
# the exported Graphics consumer; exercise the latter contract here.
set(_legacy_variable_vulkan_root
  "${_terreate_package_root}/legacy-variable-vulkan")
set(_legacy_variable_vulkan_config_dir
  "${_legacy_variable_vulkan_root}/lib/cmake/Vulkan")
find_library(_legacy_variable_vulkan_library NAMES vulkan)
if(NOT _legacy_variable_vulkan_library)
  message(FATAL_ERROR
    "legacy Vulkan variable fixture could not locate a Vulkan loader library")
endif()
file(MAKE_DIRECTORY "${_legacy_variable_vulkan_config_dir}")
file(WRITE "${_legacy_variable_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "set(Vulkan_INCLUDE_DIR \"$ENV{VULKAN_HEADERS_INCLUDE}\")\n"
  "set(Vulkan_INCLUDE_DIRS \"$ENV{VULKAN_HEADERS_INCLUDE}\")\n"
  "set(Vulkan_LIBRARY \"${_legacy_variable_vulkan_library}\")\n"
  "add_library(Vulkan::Vulkan UNKNOWN IMPORTED)\n"
  "set_target_properties(Vulkan::Vulkan PROPERTIES\n"
  "  IMPORTED_LOCATION \"${_legacy_variable_vulkan_library}\")\n")
file(WRITE "${_legacy_variable_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")
terreate_expect_installed_graphics_legacy_include(
  legacy-variable "${_legacy_variable_vulkan_config_dir}"
  "$ENV{VULKAN_HEADERS_INCLUDE}")

# Each of these package-local Vulkan fixtures is intentionally incomplete.
# They all declare a Vulkan 1.3 version and a Vulkan::Vulkan target so a
# version-only or loader-only check cannot accept them.
set(_missing_core_vulkan_root
  "${_terreate_package_root}/missing-core-vulkan")
set(_missing_core_vulkan_include "${_missing_core_vulkan_root}/include")
set(_missing_core_vulkan_config_dir
  "${_missing_core_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_missing_core_vulkan_include}/vulkan"
  "${_missing_core_vulkan_config_dir}")
file(WRITE "${_missing_core_vulkan_include}/vulkan/vulkan.hpp"
  "// Hpp decoy without the required core header.\n")
file(WRITE "${_missing_core_vulkan_include}/vulkan/vulkan_raii.hpp"
  "// RAII decoy without the required core header.\n")
file(WRITE "${_missing_core_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_missing_core_vulkan_include}\")\n")
file(WRITE "${_missing_core_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")
terreate_expect_installed_graphics_rejection(
  missing-core "${_missing_core_vulkan_config_dir}")

set(_missing_hpp_vulkan_root
  "${_terreate_package_root}/missing-hpp-vulkan")
set(_missing_hpp_vulkan_include "${_missing_hpp_vulkan_root}/include")
set(_missing_hpp_vulkan_config_dir
  "${_missing_hpp_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_missing_hpp_vulkan_include}/vulkan"
  "${_missing_hpp_vulkan_config_dir}")
file(WRITE "${_missing_hpp_vulkan_include}/vulkan/vulkan_core.h" [=[
#define VK_API_VERSION_1_3 4202496U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT 0x00000001U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT 0x00000002U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT 0x00000004U
using VkDebugUtilsMessageTypeFlagsEXT = unsigned int;
]=])
file(WRITE "${_missing_hpp_vulkan_include}/vulkan/vulkan_raii.hpp"
  "class Context {}; class Instance {}; class DebugUtilsMessengerEXT {};\n")
file(WRITE "${_missing_hpp_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_missing_hpp_vulkan_include}\")\n")
file(WRITE "${_missing_hpp_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")
terreate_expect_installed_graphics_rejection(
  missing-hpp "${_missing_hpp_vulkan_config_dir}")

set(_missing_device_vulkan_root
  "${_terreate_package_root}/missing-device-address-binding-vulkan")
set(_missing_device_vulkan_include "${_missing_device_vulkan_root}/include")
set(_missing_device_vulkan_config_dir
  "${_missing_device_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_missing_device_vulkan_include}/vulkan"
  "${_missing_device_vulkan_config_dir}")
file(WRITE "${_missing_device_vulkan_include}/vulkan/vulkan_core.h" [=[
#define VK_API_VERSION_1_0 4194304U
#define VK_API_VERSION_1_3 4202496U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT 0x00000001U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT 0x00000002U
#define VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT 0x00000004U
using VkDebugUtilsMessageTypeFlagsEXT = unsigned int;
]=])
file(WRITE "${_missing_device_vulkan_include}/vulkan/vulkan.hpp" [=[
#pragma once

#include <vulkan/vulkan_core.h>

namespace vk {

enum class DebugUtilsMessageTypeFlagBitsEXT : unsigned int {
  eGeneral = 1,
  eValidation = 2,
  ePerformance = 4,
};

struct Instance {};

} // namespace vk
]=])
file(WRITE "${_missing_device_vulkan_include}/vulkan/vulkan_raii.hpp"
  "#pragma once\n"
  "#include <vulkan/vulkan.hpp>\n"
  "namespace vk::raii {\n"
  "class Context {}; class Instance {}; class DebugUtilsMessengerEXT {};\n"
  "}\n")
file(WRITE "${_missing_device_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_missing_device_vulkan_include}\")\n")
file(WRITE "${_missing_device_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")
terreate_expect_installed_graphics_acceptance(
  missing-device-address-binding "${_missing_device_vulkan_config_dir}")

# A Vulkan loader package can be present without the Vulkan-Hpp RAII header
# required by Graphics' public API.  Exercise that distinction with a
# package-local Vulkan config so the installed Terreate config cannot fall
# through to an unrelated host include directory.
set(_headerless_vulkan_root "${_terreate_package_root}/headerless-vulkan")
set(_headerless_vulkan_include "${_headerless_vulkan_root}/include")
set(_headerless_vulkan_decoy_include
  "${_headerless_vulkan_root}/decoy-headers/include")
set(_headerless_vulkan_config_dir
  "${_headerless_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_headerless_vulkan_include}"
  "${_headerless_vulkan_decoy_include}/vulkan"
  "${_headerless_vulkan_config_dir}")
file(WRITE "${_headerless_vulkan_decoy_include}/vulkan/vulkan_raii.hpp"
  "// This header is attached only to Vulkan::Headers and package variables.\n")
file(WRITE "${_headerless_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "set(Vulkan_INCLUDE_DIRS \"${_headerless_vulkan_decoy_include}\")\n"
  "set(Vulkan_INCLUDE_DIR \"${_headerless_vulkan_decoy_include}\")\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_headerless_vulkan_include}\")\n"
  "add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Headers PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_headerless_vulkan_decoy_include}\")\n")
file(WRITE "${_headerless_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_headerless_optional_consumer
  "${_terreate_package_root}/all_components/optional-graphics-without-raii")
file(MAKE_DIRECTORY "${_headerless_optional_consumer}")
file(WRITE "${_headerless_optional_consumer}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\n"
  "project(TerreateOptionalGraphicsWithoutRaii LANGUAGES CXX)\n"
  "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
  "find_package(Terreate REQUIRED COMPONENTS Core OPTIONAL_COMPONENTS Graphics)\n"
  "if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR\n"
  "   NOT TARGET Terreate::Core)\n"
  "  message(FATAL_ERROR \"required Core was rejected while Vulkan-Hpp was absent\")\n"
  "endif()\n"
  "get_target_property(_headerless_core_links Terreate::Core\n"
  "  INTERFACE_LINK_LIBRARIES)\n"
  "if(\"\${_headerless_core_links}\" MATCHES \"Vulkan\")\n"
  "  message(FATAL_ERROR\n"
  "    \"Core acquired an unintended Vulkan dependency from optional Graphics\")\n"
  "endif()\n"
  "if(Terreate_Graphics_FOUND OR TARGET Terreate::Graphics)\n"
  "  message(FATAL_ERROR\n"
  "    \"optional Graphics was imported without vulkan/vulkan_raii.hpp\")\n"
  "endif()\n"
  "if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR\n"
  "   NOT \"\${Terreate_NOT_FOUND_MESSAGE}\" MATCHES \"vulkan/vulkan_raii\")\n"
  "  message(FATAL_ERROR\n"
  "    \"optional Graphics status did not name the missing Vulkan-Hpp header\")\n"
  "endif()\n"
  "add_executable(headerless_optional_consumer main.cpp)\n"
  "target_link_libraries(headerless_optional_consumer PRIVATE Terreate::Core)\n"
  "set_target_properties(headerless_optional_consumer PROPERTIES\n"
  "  CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)\n")
file(WRITE "${_headerless_optional_consumer}/main.cpp"
  "#include <terreate/core/result.hpp>\n"
  "int main() {\n"
  "  terreate::Result<int> result = 37;\n"
  "  return terreate::unwrap(result) == 37 ? 0 : 1;\n"
  "}\n")

set(_headerless_optional_consumer_build
  "${_terreate_package_root}/all_components/optional-graphics-without-raii-build")
file(REMOVE_RECURSE "${_headerless_optional_consumer_build}")
set(_headerless_optional_configure
  "${CMAKE_COMMAND}"
  -S "${_headerless_optional_consumer}"
  -B "${_headerless_optional_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _headerless_optional_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _headerless_optional_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _headerless_optional_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_headerless_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
execute_process(
  COMMAND ${_headerless_optional_configure}
  RESULT_VARIABLE _headerless_optional_configure_result
  OUTPUT_VARIABLE _headerless_optional_configure_output
  ERROR_VARIABLE _headerless_optional_configure_error)
if(NOT _headerless_optional_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed optional Graphics-without-Raii consumer configuration failed\n"
    "${_headerless_optional_configure_output}\n"
    "${_headerless_optional_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_headerless_optional_consumer_build}"
  RESULT_VARIABLE _headerless_optional_build_result
  OUTPUT_VARIABLE _headerless_optional_build_output
  ERROR_VARIABLE _headerless_optional_build_error)
if(NOT _headerless_optional_build_result EQUAL 0)
  message(FATAL_ERROR
    "installed optional Graphics-without-Raii consumer build failed\n"
    "${_headerless_optional_build_output}\n"
    "${_headerless_optional_build_error}")
endif()

set(_headerless_required_consumer
  "${_terreate_package_root}/all_components/required-graphics-without-raii")
file(MAKE_DIRECTORY "${_headerless_required_consumer}")
file(WRITE "${_headerless_required_consumer}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\n"
  "project(TerreateRequiredGraphicsWithoutRaii LANGUAGES CXX)\n"
  "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
  "find_package(Terreate REQUIRED COMPONENTS Graphics)\n")

set(_headerless_required_consumer_build
  "${_terreate_package_root}/all_components/required-graphics-without-raii-build")
file(REMOVE_RECURSE "${_headerless_required_consumer_build}")
set(_headerless_required_configure
  "${CMAKE_COMMAND}"
  -S "${_headerless_required_consumer}"
  -B "${_headerless_required_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _headerless_required_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _headerless_required_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _headerless_required_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_headerless_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
execute_process(
  COMMAND ${_headerless_required_configure}
  RESULT_VARIABLE _headerless_required_configure_result
  OUTPUT_VARIABLE _headerless_required_configure_output
  ERROR_VARIABLE _headerless_required_configure_error)
if(_headerless_required_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed required Graphics consumer unexpectedly configured without "
    "vulkan/vulkan_raii.hpp\n"
    "${_headerless_required_configure_output}")
endif()
string(TOLOWER
  "${_headerless_required_configure_output}\n${_headerless_required_configure_error}"
  _headerless_required_diagnostics)
if(NOT _headerless_required_diagnostics MATCHES "vulkan/vulkan_raii")
  message(FATAL_ERROR
    "required Graphics failure did not identify the missing Vulkan-Hpp header\n"
    "${_headerless_required_configure_output}\n"
    "${_headerless_required_configure_error}")
endif()

# A loader/header package below the project's Vulkan 1.3 baseline must be
# rejected at package configuration time, even when it happens to provide the
# RAII header.  This keeps a pre-1.3 package from reaching consumer compile.
set(_pre13_vulkan_root "${_terreate_package_root}/pre13-vulkan")
set(_pre13_vulkan_include "${_pre13_vulkan_root}/include")
set(_pre13_vulkan_config_dir "${_pre13_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_pre13_vulkan_include}/vulkan"
  "${_pre13_vulkan_config_dir}")
file(WRITE "${_pre13_vulkan_include}/vulkan/vulkan_raii.hpp"
  "// The RAII header exists, but this package is Vulkan 1.2.\n")
file(WRITE "${_pre13_vulkan_include}/vulkan/vulkan_core.h"
  "#define VK_API_VERSION_1_2 1\n")
file(WRITE "${_pre13_vulkan_config_dir}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.2.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_pre13_vulkan_include}\")\n")
file(WRITE "${_pre13_vulkan_config_dir}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.2.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_pre13_consumer
  "${_terreate_package_root}/all_components/pre13-graphics-consumer")
file(MAKE_DIRECTORY "${_pre13_consumer}")
file(WRITE "${_pre13_consumer}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\n"
  "project(TerreatePre13VulkanConsumer LANGUAGES CXX)\n"
  "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
  "set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)\n"
  "find_package(Terreate REQUIRED COMPONENTS Core OPTIONAL_COMPONENTS Graphics)\n"
  "if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR\n"
  "   NOT TARGET Terreate::Core)\n"
  "  message(FATAL_ERROR\n"
  "    \"required Core was rejected while Vulkan was pre-1.3\")\n"
  "endif()\n"
  "if(Terreate_Graphics_FOUND OR TARGET Terreate::Graphics)\n"
  "  message(FATAL_ERROR\n"
  "    \"optional Graphics was imported from pre-1.3 Vulkan\")\n"
  "endif()\n"
  "if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR\n"
  "   NOT \"\${Terreate_NOT_FOUND_MESSAGE}\" MATCHES\n"
  "      \"Vulkan|1\\.3|pre-1\\.3\")\n"
  "  message(FATAL_ERROR\n"
  "    \"pre-1.3 Graphics rejection did not identify the Vulkan baseline\")\n"
  "endif()\n"
  "add_executable(pre13_core_consumer main.cpp)\n"
  "target_link_libraries(pre13_core_consumer PRIVATE Terreate::Core)\n"
  "set_target_properties(pre13_core_consumer PROPERTIES\n"
  "  CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)\n")
file(WRITE "${_pre13_consumer}/main.cpp"
  "#include <terreate/core/result.hpp>\n"
  "int main() {\n"
  "  terreate::Result<int> result = 41;\n"
  "  return terreate::unwrap(result) == 41 ? 0 : 1;\n"
  "}\n")

set(_pre13_consumer_build
  "${_terreate_package_root}/all_components/pre13-graphics-consumer-build")
file(REMOVE_RECURSE "${_pre13_consumer_build}")
set(_pre13_configure
  "${CMAKE_COMMAND}"
  -S "${_pre13_consumer}"
  -B "${_pre13_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _pre13_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _pre13_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _pre13_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_pre13_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
execute_process(
  COMMAND ${_pre13_configure}
  RESULT_VARIABLE _pre13_configure_result
  OUTPUT_VARIABLE _pre13_configure_output
  ERROR_VARIABLE _pre13_configure_error)
if(NOT _pre13_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed pre-1.3 optional Graphics consumer configuration failed\n"
    "${_pre13_configure_output}\n${_pre13_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_pre13_consumer_build}"
  RESULT_VARIABLE _pre13_build_result
  OUTPUT_VARIABLE _pre13_build_output
  ERROR_VARIABLE _pre13_build_error)
if(NOT _pre13_build_result EQUAL 0)
  message(FATAL_ERROR
    "installed pre-1.3 Core isolation consumer build failed\n"
    "${_pre13_build_output}\n${_pre13_build_error}")
endif()

set(_pre13_required_consumer
  "${_terreate_package_root}/all_components/pre13-required-graphics")
file(MAKE_DIRECTORY "${_pre13_required_consumer}")
file(WRITE "${_pre13_required_consumer}/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.28)\n"
  "project(TerreatePre13RequiredGraphics LANGUAGES CXX)\n"
  "set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)\n"
  "set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)\n"
  "find_package(Terreate REQUIRED COMPONENTS Graphics)\n")
set(_pre13_required_build
  "${_terreate_package_root}/all_components/pre13-required-graphics-build")
file(REMOVE_RECURSE "${_pre13_required_build}")
set(_pre13_required_configure
  "${CMAKE_COMMAND}"
  -S "${_pre13_required_consumer}"
  -B "${_pre13_required_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _pre13_required_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _pre13_required_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _pre13_required_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_pre13_vulkan_config_dir}"
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE)
execute_process(
  COMMAND ${_pre13_required_configure}
  RESULT_VARIABLE _pre13_required_configure_result
  OUTPUT_VARIABLE _pre13_required_configure_output
  ERROR_VARIABLE _pre13_required_configure_error)
if(_pre13_required_configure_result EQUAL 0)
  message(FATAL_ERROR
    "installed required Graphics unexpectedly accepted pre-1.3 Vulkan\n"
    "${_pre13_required_configure_output}")
endif()
string(TOLOWER
  "${_pre13_required_configure_output}\n${_pre13_required_configure_error}"
  _pre13_required_diagnostics)
if(NOT _pre13_required_diagnostics MATCHES "vulkan|1\\.3|pre-1\\.3")
  message(FATAL_ERROR
    "pre-1.3 required Graphics failure did not identify the Vulkan baseline\n"
    "${_pre13_required_configure_output}\n"
    "${_pre13_required_configure_error}")
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
#include <terreate/core/diagnostics.hpp>
#include <terreate/core/result.hpp>

int main() {
  terreate::Result<int> result = 23;
  terreate::DiagnosticEvent event{
      .severity = terreate::DiagnosticSeverity::info,
      .categories = {"package-specific"}};
  terreate::DiagnosticSinkView sink;
  sink.emit(event);
  return terreate::unwrap(result) == 23 && event.severity == terreate::DiagnosticSeverity::info &&
                 event.categories.size() == 1 && event.categories.front() == "package-specific"
             ? 0
             : 1;
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

# Exercise the other independently installable component closures as packages,
# not only as source-subdirectory configurations.  The installed exports must
# retain Core as the dependency of Platform and Graphics, while never importing
# an optional sibling merely because it was built in a different package.
function(terreate_build_installed_component_consumer name prefix component)
  set(_consumer
    "${_terreate_package_root}/${name}/consumer")
  file(MAKE_DIRECTORY "${_consumer}")

  set(_consumer_contents "")
  string(APPEND _consumer_contents
    "cmake_minimum_required(VERSION 3.28)\n"
    "project(Terreate${name}Consumer LANGUAGES CXX)\n"
    "find_package(Terreate REQUIRED COMPONENTS ${component})\n"
    "if(NOT TARGET Terreate::Core OR NOT TARGET Terreate::${component})\n"
    "  message(FATAL_ERROR \"${name} package omitted its requested target closure\")\n"
    "endif()\n")
  set(_source "")
  if("${component}" STREQUAL "Platform")
    string(APPEND _consumer_contents
      "if(TARGET Terreate::Graphics OR TARGET Vulkan::Vulkan)\n"
      "  message(FATAL_ERROR \"Platform-only package imported Graphics or Vulkan\")\n"
      "endif()\n"
      "get_target_property(_platform_links Terreate::Platform\n"
      "  INTERFACE_LINK_LIBRARIES)\n"
      "if(NOT \"\${_platform_links}\" MATCHES \"Core\" OR\n"
      "   \"\${_platform_links}\" MATCHES \"Graphics|Vulkan\")\n"
      "  message(FATAL_ERROR \"Platform package has an invalid dependency closure: \${_platform_links}\")\n"
      "endif()\n"
      "add_executable(package_component_consumer main.cpp)\n"
      "target_link_libraries(package_component_consumer PRIVATE Terreate::Platform)\n")
    string(APPEND _source "int main() { return 0; }\n")
  elseif("${component}" STREQUAL "Graphics")
    string(APPEND _consumer_contents
      "if(TARGET Terreate::Platform OR NOT TARGET Vulkan::Vulkan OR NOT Vulkan_FOUND)\n"
      "  message(FATAL_ERROR \"Graphics-only package omitted Vulkan or imported Platform\")\n"
      "endif()\n"
      "get_target_property(_graphics_links Terreate::Graphics\n"
      "  INTERFACE_LINK_LIBRARIES)\n"
      "if(NOT \"\${_graphics_links}\" MATCHES \"Core\" OR\n"
      "   NOT \"\${_graphics_links}\" MATCHES \"Vulkan::Vulkan\" OR\n"
      "   \"\${_graphics_links}\" MATCHES \"Platform\")\n"
      "  message(FATAL_ERROR \"Graphics package has an invalid dependency closure: \${_graphics_links}\")\n"
      "endif()\n"
      "add_executable(package_component_consumer main.cpp)\n"
      "target_link_libraries(package_component_consumer PRIVATE Terreate::Graphics)\n")
    string(APPEND _source
      "#include <terreate/graphics/instance.hpp>\n"
      "int main() { return 0; }\n")
  else()
    message(FATAL_ERROR "unsupported installed package component ${component}")
  endif()
  string(APPEND _consumer_contents
    "set_target_properties(package_component_consumer PROPERTIES\n"
    "  CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)\n")

  file(WRITE "${_consumer}/CMakeLists.txt" "${_consumer_contents}")
  file(WRITE "${_consumer}/main.cpp" "${_source}")

  set(_consumer_build
    "${_terreate_package_root}/${name}/consumer-build")
  file(REMOVE_RECURSE "${_consumer_build}")
  set(_configure
    "${CMAKE_COMMAND}"
    -S "${_consumer}"
    -B "${_consumer_build}")
  if(DEFINED TERREATE_CMAKE_GENERATOR AND
     NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
    list(APPEND _configure -G "${TERREATE_CMAKE_GENERATOR}")
  endif()
  if(DEFINED TERREATE_CXX_COMPILER AND
     NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
    list(APPEND _configure
      "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
  endif()
  list(APPEND _configure "-DCMAKE_PREFIX_PATH:PATH=${prefix}")
  execute_process(
    COMMAND ${_configure}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "installed ${name} ${component} consumer configuration failed\n"
      "${_configure_output}\n${_configure_error}")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
      "installed ${name} ${component} consumer build failed\n"
      "${_build_output}\n${_build_error}")
  endif()
endfunction()

terreate_package_configure_and_install(platform_only ON OFF)
terreate_build_installed_component_consumer(
  platform_only "${platform_only_PREFIX}" Platform)

terreate_package_configure_and_install(graphics_only OFF ON)
terreate_build_installed_component_consumer(
  graphics_only "${graphics_only_PREFIX}" Graphics)
