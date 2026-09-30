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
           "^include/terreate/(core/(diagnostics|error|result)|graphics/(instance|physical_device))\\.hpp$")
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
file(GLOB_RECURSE _installed_graphics_target_candidates
  "${_all_prefix}/*/TerreateGraphicsTargets.cmake")
if(NOT _installed_graphics_target_candidates)
  message(FATAL_ERROR "installed Graphics target export is missing")
endif()
list(GET _installed_graphics_target_candidates 0 _installed_graphics_targets_file)
file(READ "${_installed_graphics_targets_file}" _installed_graphics_targets_contents)
string(FIND "${_installed_graphics_targets_contents}" "${_terreate_source_dir}"
  _installed_graphics_source_root_position)
if(NOT _installed_graphics_source_root_position EQUAL -1)
  message(FATAL_ERROR
    "installed Graphics export leaked the Terreate source root")
endif()
string(FIND "${_installed_graphics_targets_contents}" "$ENV{VULKAN_HEADERS_INCLUDE}"
  _installed_graphics_hpp_root_position)
if(NOT _installed_graphics_hpp_root_position EQUAL -1)
  message(FATAL_ERROR
    "installed Graphics export leaked the build-only Vulkan-Hpp root")
endif()
if(NOT EXISTS "${_all_prefix}/include/terreate/graphics/instance.hpp")
  message(FATAL_ERROR "Graphics install is missing its public instance header")
endif()
if(NOT EXISTS "${_all_prefix}/include/terreate/graphics/physical_device.hpp")
  message(FATAL_ERROR "Graphics install is missing its public physical-device header")
endif()
foreach(_public_graphics_header IN ITEMS
    "${_all_prefix}/include/terreate/graphics/instance.hpp"
    "${_all_prefix}/include/terreate/graphics/physical_device.hpp")
  file(READ "${_public_graphics_header}" _public_graphics_header_contents)
  if(_public_graphics_header_contents MATCHES
       "PhysicalDeviceQueryAccess|namespace[ \t]+detail|friend[^\n]*(detail|Instance::Impl)")
    message(FATAL_ERROR
      "installed public Graphics header exposes a forgeable physical-device authority: "
      "${_public_graphics_header}")
  endif()
endforeach()

# Former access-hook macros must be inert: an installed consumer that defines
# every old spelling and attempts the old bridge construction must fail before
# it can produce a view.  The fixed production constructor requires the
# implementation-owned authority ABI, so this two-argument bridge has no
# matching or accessible authority path.
set(_forged_macro_bridge_consumer
  "${_terreate_package_root}/all_components/forged-macro-bridge-consumer")
file(MAKE_DIRECTORY "${_forged_macro_bridge_consumer}")
file(WRITE "${_forged_macro_bridge_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedMacroBridgeConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
add_executable(forged_macro_bridge main.cpp)
target_include_directories(forged_macro_bridge PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_macro_bridge PRIVATE
  Terreate::Core
  Vulkan::Vulkan)
set_target_properties(forged_macro_bridge PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_forged_macro_bridge_consumer}/main.cpp" [=[
#define TERREATE_GRAPHICS_INTERNAL_QUERY_ACCESS 1
#define TERREATE_GRAPHICS_QUERY_ACCESS_KEYWORD friend
#define TERREATE_GRAPHICS_QUERY_ACCESS_BRIDGE class PhysicalDeviceQueryBridge

#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include <cstdint>

namespace terreate::graphics {

class PhysicalDeviceQueryBridge {
public:
  static PhysicalDevice forge(const Instance &instance) noexcept {
    const auto native = vk::PhysicalDevice{
        reinterpret_cast<VkPhysicalDevice>(static_cast<VkInstance>(instance.nativeHandle()))};
    return PhysicalDevice{native, static_cast<const void *>(instance.plan())};
  }
};

} // namespace terreate::graphics

int main() { return 0; }
]=])
set(_forged_macro_bridge_build
  "${_terreate_package_root}/all_components/forged-macro-bridge-consumer-build")
set(_forged_macro_bridge_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_macro_bridge_consumer}"
  -B "${_forged_macro_bridge_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_macro_bridge_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_macro_bridge_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_macro_bridge_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_macro_bridge_configure}
  RESULT_VARIABLE _forged_macro_bridge_configure_result
  OUTPUT_VARIABLE _forged_macro_bridge_configure_output
  ERROR_VARIABLE _forged_macro_bridge_configure_error)
if(NOT _forged_macro_bridge_configure_result EQUAL 0)
  message(FATAL_ERROR
    "former-macro bridge consumer configuration failed\n"
    "${_forged_macro_bridge_configure_output}\n"
    "${_forged_macro_bridge_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_macro_bridge_build}"
  RESULT_VARIABLE _forged_macro_bridge_build_result
  OUTPUT_VARIABLE _forged_macro_bridge_build_output
  ERROR_VARIABLE _forged_macro_bridge_build_error)
if(_forged_macro_bridge_build_result EQUAL 0)
  message(FATAL_ERROR
    "former TERREATE_GRAPHICS access macros unexpectedly forged a view\n"
    "${_forged_macro_bridge_build_output}")
endif()
string(TOLOWER
  "${_forged_macro_bridge_build_output}\n${_forged_macro_bridge_build_error}"
  _forged_macro_bridge_diagnostics)
if(NOT _forged_macro_bridge_diagnostics MATCHES
     "no matching|inaccessible|private|protected|authority|constructible")
  message(FATAL_ERROR
    "former-macro bridge failed for an unexpected reason\n"
    "${_forged_macro_bridge_build_output}\n"
    "${_forged_macro_bridge_build_error}")
endif()

# C: Preserve the explicit Core+Graphics isolation contract: Core and Graphics
# are imported, Platform is absent, and Vulkan is resolved for Graphics.
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
# The normal installed consumer exercises the supported shared Graphics target.
# Adversarial subclass/raw construction and symbol interposition are checked in
# isolated consumers below so this package consumer remains a valid API smoke
# test.
file(WRITE "${_all_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>

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
  if (!instance || !instance->valid()) {
    return 1;
  }
  const auto devices = terreate::graphics::queryPhysicalDevices(*instance);
  if (!devices) {
    return 1;
  }
  return 0;
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

# Inspect the installed artifact before exercising shared-DSO consumers.  The
# package has exactly one selectable Graphics DSO and no Graphics archive or
# private implementation target.
file(GLOB_RECURSE _installed_graphics_dso_candidates
  "${_all_prefix}/*/libterreate_graphics.so*")
list(LENGTH _installed_graphics_dso_candidates _installed_graphics_dso_count)
if(NOT _installed_graphics_dso_count EQUAL 1)
  message(FATAL_ERROR
    "installed package did not provide exactly one Graphics DSO: "
    "${_installed_graphics_dso_candidates}")
endif()
list(GET _installed_graphics_dso_candidates 0 _installed_graphics_dso)
file(GLOB_RECURSE _installed_graphics_archives
  "${_all_prefix}/*/libterreate_graphics.a"
  "${_all_prefix}/*/libterreate_graphics_private.*")
if(_installed_graphics_archives)
  message(FATAL_ERROR
    "installed package exposed a selectable Graphics archive/private artifact: "
    "${_installed_graphics_archives}")
endif()
file(GLOB_RECURSE _installed_graphics_target_exports
  "${_all_prefix}/*/TerreateGraphicsTargets.cmake")
foreach(_graphics_target_export IN LISTS _installed_graphics_target_exports)
  file(READ "${_graphics_target_export}" _graphics_target_contents)
  if(_graphics_target_contents MATCHES
       "GraphicsPrivate|terreate_graphics_private|WHOLE_ARCHIVE|libterreate_graphics\.a")
    message(FATAL_ERROR
      "installed Graphics target export retained a private/archive selection path: "
      "${_graphics_target_export}")
  endif()
endforeach()

find_program(_terreate_readelf NAMES readelf llvm-readelf)
if(NOT _terreate_readelf)
  message(FATAL_ERROR "ELF DSO inspection requires readelf")
endif()
execute_process(
  COMMAND "${_terreate_readelf}" --dyn-syms --wide --demangle "${_installed_graphics_dso}"
  RESULT_VARIABLE _graphics_dynsym_result
  OUTPUT_VARIABLE _graphics_dynsym_output
  ERROR_VARIABLE _graphics_dynsym_error)
if(NOT _graphics_dynsym_result EQUAL 0)
  message(FATAL_ERROR
    "installed Graphics DSO dynamic-symbol inspection failed\n"
    "${_graphics_dynsym_error}")
endif()
foreach(_supported_graphics_symbol IN ITEMS
    "queryPhysicalDevices"
    "evaluatePhysicalDevice"
    "selectPhysicalDevice"
    "PhysicalDevice::valid"
    "PhysicalDevice::nativeHandle"
    "PhysicalDevice::correlatedWith"
    "createInstance"
    "queryInstanceCapabilities")
  if(NOT _graphics_dynsym_output MATCHES "${_supported_graphics_symbol}")
    message(FATAL_ERROR
      "installed Graphics DSO omitted supported export ${_supported_graphics_symbol}")
  endif()
endforeach()
foreach(_forbidden_graphics_symbol IN ITEMS
    "PhysicalDeviceState"
    "PhysicalDevice::QueryTag"
    "PhysicalDevice::query_tag"
    "validate_physical_device_runtime_authority"
    "PhysicalDeviceQueryBridge"
    "physical_device_query"
    "instance_query")
  if(_graphics_dynsym_output MATCHES "${_forbidden_graphics_symbol}")
    message(FATAL_ERROR
      "installed Graphics DSO exported private/test symbol ${_forbidden_graphics_symbol}")
  endif()
endforeach()
string(REPLACE "\n" ";" _graphics_dynsym_lines "${_graphics_dynsym_output}")
foreach(_graphics_dynsym_line IN LISTS _graphics_dynsym_lines)
  if(_graphics_dynsym_line MATCHES "UND" AND
     _graphics_dynsym_line MATCHES "terreate::graphics")
    message(FATAL_ERROR
      "installed Graphics DSO has an unresolved Graphics implementation symbol")
  endif()
endforeach()
execute_process(
  COMMAND "${_terreate_readelf}" --dynamic --wide "${_installed_graphics_dso}"
  RESULT_VARIABLE _graphics_dynamic_result
  OUTPUT_VARIABLE _graphics_dynamic_output
  ERROR_VARIABLE _graphics_dynamic_error)
if(NOT _graphics_dynamic_result EQUAL 0 OR
   _graphics_dynamic_output MATCHES "TEXTREL")
  message(FATAL_ERROR
    "installed Graphics DSO is not a clean relocatable ELF artifact\n"
    "${_graphics_dynamic_output}")
endif()

# A shared-DSO consumer may replace public query/observer definitions in its
# executable.  The DSO's bound production calls must still reject the forged
# default view during selection.
set(_forged_query_runtime_consumer
  "${_terreate_package_root}/all_components/forged-query-runtime-consumer")
file(MAKE_DIRECTORY "${_forged_query_runtime_consumer}")
file(WRITE "${_forged_query_runtime_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedQueryRuntimeConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
if(NOT TARGET Terreate::Core OR NOT TARGET Vulkan::Vulkan)
  message(FATAL_ERROR "shared Graphics DSO dependencies were not found")
endif()
add_executable(forged_query_runtime main.cpp)
target_include_directories(forged_query_runtime PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_query_runtime PRIVATE
  "${TERREATE_GRAPHICS_DSO}"
  Terreate::Core
  Vulkan::Vulkan)
set_target_properties(forged_query_runtime PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_forged_query_runtime_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

Result<PhysicalDeviceInventory> queryPhysicalDevices(const Instance &) {
  PhysicalDeviceInventory inventory;
  inventory.candidates.push_back(PhysicalDeviceCandidate{});
  return inventory;
}

} // namespace terreate::graphics

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
  if (!instance || !instance->valid()) {
    return 1;
  }

  const auto forged = terreate::graphics::queryPhysicalDevices(*instance);
  if (!forged || forged->size() != 1) {
    return 1;
  }
  return forged->front().device.nativeHandle() == vk::PhysicalDevice{} ? 0 : 1;
}
]=])
set(_forged_query_runtime_build
  "${_terreate_package_root}/all_components/forged-query-runtime-consumer-build")
set(_forged_query_runtime_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_query_runtime_consumer}"
  -B "${_forged_query_runtime_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_query_runtime_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_query_runtime_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_query_runtime_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_GRAPHICS_DSO:FILEPATH=${_installed_graphics_dso}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_query_runtime_configure}
  RESULT_VARIABLE _forged_query_runtime_configure_result
  OUTPUT_VARIABLE _forged_query_runtime_configure_output
  ERROR_VARIABLE _forged_query_runtime_configure_error)
if(NOT _forged_query_runtime_configure_result EQUAL 0)
  message(FATAL_ERROR
    "forged-query runtime consumer configuration failed\n"
    "${_forged_query_runtime_configure_output}\n"
    "${_forged_query_runtime_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_query_runtime_build}"
  RESULT_VARIABLE _forged_query_runtime_build_result
  OUTPUT_VARIABLE _forged_query_runtime_build_output
  ERROR_VARIABLE _forged_query_runtime_build_error)
if(NOT _forged_query_runtime_build_result EQUAL 0)
  message(FATAL_ERROR
    "forged-query runtime consumer build failed\n"
    "${_forged_query_runtime_build_output}\n"
    "${_forged_query_runtime_build_error}")
endif()
execute_process(
  COMMAND "${_forged_query_runtime_build}/forged_query_runtime"
  RESULT_VARIABLE _forged_query_runtime_result
  OUTPUT_VARIABLE _forged_query_runtime_output
  ERROR_VARIABLE _forged_query_runtime_error)
if(NOT _forged_query_runtime_result EQUAL 0)
  message(FATAL_ERROR
    "forged-query runtime consumer unexpectedly produced a valid view\n"
    "${_forged_query_runtime_output}\n${_forged_query_runtime_error}")
endif()

# A consumer must not recover the removed private implementation construction
# authority.  This shared-DSO fixture deliberately attempts to define the
# former nested type and replaces the query, but uses only the public
# nativeHandle()/plan() observations.  Compilation must fail before it can
# produce a correlated view.
set(_forged_impl_consumer
  "${_terreate_package_root}/all_components/forged-instance-impl-consumer")
file(MAKE_DIRECTORY "${_forged_impl_consumer}")
file(WRITE "${_forged_impl_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedInstanceImplConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
add_executable(forged_instance_impl main.cpp)
target_include_directories(forged_instance_impl PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_instance_impl PRIVATE
  "${TERREATE_GRAPHICS_DSO}"
  Terreate::Core
  Vulkan::Vulkan)
set_target_properties(forged_instance_impl PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_forged_impl_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

struct Instance::Impl {
  static PhysicalDevice forge(const Instance &instance) noexcept {
    const auto native = vk::PhysicalDevice{
        reinterpret_cast<VkPhysicalDevice>(static_cast<VkInstance>(instance.nativeHandle()))};
    return PhysicalDevice{native, static_cast<const void *>(instance.plan())};
  }
};

Result<PhysicalDeviceInventory> queryPhysicalDevices(const Instance &instance) {
  PhysicalDeviceInventory inventory;
  inventory.candidates.push_back(
      PhysicalDeviceCandidate{.device = Instance::Impl::forge(instance)});
  return inventory;
}

} // namespace terreate::graphics

int main() { return 0; }
]=])
set(_forged_impl_build
  "${_terreate_package_root}/all_components/forged-instance-impl-consumer-build")
set(_forged_impl_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_impl_consumer}"
  -B "${_forged_impl_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_impl_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_impl_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_impl_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_GRAPHICS_DSO:FILEPATH=${_installed_graphics_dso}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_impl_configure}
  RESULT_VARIABLE _forged_impl_configure_result
  OUTPUT_VARIABLE _forged_impl_configure_output
  ERROR_VARIABLE _forged_impl_configure_error)
if(NOT _forged_impl_configure_result EQUAL 0)
  message(FATAL_ERROR
    "forged Instance::Impl consumer configuration failed\n"
    "${_forged_impl_configure_output}\n${_forged_impl_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_impl_build}"
  RESULT_VARIABLE _forged_impl_build_result
  OUTPUT_VARIABLE _forged_impl_build_output
  ERROR_VARIABLE _forged_impl_build_error)
if(_forged_impl_build_result EQUAL 0)
  message(FATAL_ERROR
    "forged Instance::Impl authority unexpectedly compiled and linked\n"
    "${_forged_impl_build_output}\n${_forged_impl_build_error}")
endif()
string(TOLOWER
  "${_forged_impl_build_output}\n${_forged_impl_build_error}"
  _forged_impl_diagnostics)
if(NOT _forged_impl_diagnostics MATCHES
     "does not name a class|not declared|no matching|private|inaccessible")
  message(FATAL_ERROR
    "forged Instance::Impl consumer failed for an unexpected reason\n"
    "${_forged_impl_build_output}\n${_forged_impl_build_error}")
endif()

# The former nested QueryAuthority was itself an installed declaration.  A
# consumer must not be able to recreate that enclosing-private authority and
# replace the query with a view made from public plan/native observations.
set(_forged_query_authority_consumer
  "${_terreate_package_root}/all_components/forged-query-authority-consumer")
file(MAKE_DIRECTORY "${_forged_query_authority_consumer}")
file(WRITE "${_forged_query_authority_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedQueryAuthorityConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
add_executable(forged_query_authority main.cpp)
target_include_directories(forged_query_authority PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_query_authority PRIVATE
  "${TERREATE_GRAPHICS_DSO}"
  Terreate::Core
  Vulkan::Vulkan)
set_target_properties(forged_query_authority PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_forged_query_authority_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

struct PhysicalDevice::QueryAuthority {
  static PhysicalDevice forge(const Instance &instance) noexcept {
    const auto native = vk::PhysicalDevice{
        reinterpret_cast<VkPhysicalDevice>(static_cast<VkInstance>(instance.nativeHandle()))};
    return PhysicalDevice{native, static_cast<const void *>(instance.plan())};
  }
};

Result<PhysicalDeviceInventory> queryPhysicalDevices(const Instance &instance) {
  PhysicalDeviceInventory inventory;
  inventory.candidates.push_back(
      PhysicalDeviceCandidate{.device = PhysicalDevice::QueryAuthority::forge(instance)});
  return inventory;
}

} // namespace terreate::graphics

int main() { return 0; }
]=])
set(_forged_query_authority_build
  "${_terreate_package_root}/all_components/forged-query-authority-consumer-build")
set(_forged_query_authority_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_query_authority_consumer}"
  -B "${_forged_query_authority_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_query_authority_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_query_authority_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_query_authority_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_GRAPHICS_DSO:FILEPATH=${_installed_graphics_dso}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_query_authority_configure}
  RESULT_VARIABLE _forged_query_authority_configure_result
  OUTPUT_VARIABLE _forged_query_authority_configure_output
  ERROR_VARIABLE _forged_query_authority_configure_error)
if(NOT _forged_query_authority_configure_result EQUAL 0)
  message(FATAL_ERROR
    "forged QueryAuthority consumer configuration failed\n"
    "${_forged_query_authority_configure_output}\n${_forged_query_authority_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_query_authority_build}"
  RESULT_VARIABLE _forged_query_authority_build_result
  OUTPUT_VARIABLE _forged_query_authority_build_output
  ERROR_VARIABLE _forged_query_authority_build_error)
if(_forged_query_authority_build_result EQUAL 0)
  message(FATAL_ERROR
    "consumer-defined QueryAuthority unexpectedly compiled against the installed API\n"
    "${_forged_query_authority_build_output}\n"
    "${_forged_query_authority_build_error}")
endif()
string(TOLOWER
  "${_forged_query_authority_build_output}\n${_forged_query_authority_build_error}"
  _forged_query_authority_diagnostics)
if(NOT _forged_query_authority_diagnostics MATCHES
     "queryauthority|does not name a class|not a member|private|inaccessible")
  message(FATAL_ERROR
    "consumer-defined QueryAuthority failed for an unexpected reason\n"
    "${_forged_query_authority_build_output}\n${_forged_query_authority_build_error}")
endif()

# Defining the query together with every public observer must not make a
# consumer-created view selectable.  The executable observes its replacements,
# while the DSO's bound production selection still rejects the default view.
set(_forged_query_correlated_consumer
  "${_terreate_package_root}/all_components/forged-query-correlated-consumer")
file(MAKE_DIRECTORY "${_forged_query_correlated_consumer}")
file(WRITE "${_forged_query_correlated_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedQueryCorrelatedConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
add_executable(forged_query_correlated main.cpp)
target_include_directories(forged_query_correlated PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_query_correlated PRIVATE
  "${TERREATE_GRAPHICS_DSO}"
  Terreate::Core
  Vulkan::Vulkan
  "${CMAKE_DL_LIBS}")
set_target_properties(forged_query_correlated PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  ENABLE_EXPORTS ON)
target_compile_definitions(forged_query_correlated PRIVATE
  "TERREATE_GRAPHICS_DSO_PATH=\"${TERREATE_GRAPHICS_DSO}\"")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(forged_query_correlated PRIVATE
    "-Wl,--export-dynamic")
endif()
]=])
file(WRITE "${_forged_query_correlated_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include <cstdint>
#include <dlfcn.h>
#include <memory>

namespace terreate::graphics {

struct PhysicalDevice::QueryTag {};

struct ForgedPhysicalDeviceState final {
  vk::PhysicalDevice native_handle{};
  vk::Instance parent_instance{};
  const void *authority = nullptr;
};

std::uint32_t consumer_query_calls = 0;
std::uint32_t consumer_query_tag_calls = 0;
std::uint32_t consumer_constructor_calls = 0;
std::uint32_t consumer_physical_valid_calls = 0;
std::uint32_t consumer_physical_native_handle_calls = 0;
std::uint32_t consumer_physical_correlated_calls = 0;
std::uint32_t consumer_instance_valid_calls = 0;
std::uint32_t consumer_instance_native_handle_calls = 0;
std::uint32_t consumer_instance_plan_calls = 0;

TERREATE_GRAPHICS_EXPORT const PhysicalDevice::QueryTag *PhysicalDevice::query_tag() noexcept {
  static const QueryTag tag;
  ++consumer_query_tag_calls;
  return &tag;
}

TERREATE_GRAPHICS_EXPORT PhysicalDevice::PhysicalDevice(
    vk::PhysicalDevice native_handle, vk::Instance parent_instance,
    const QueryTag *query_tag_value) noexcept {
  ++consumer_constructor_calls;
  if (query_tag_value != query_tag()) {
    return;
  }
  auto state = std::make_shared<ForgedPhysicalDeviceState>();
  state->native_handle = native_handle;
  state->parent_instance = parent_instance;
  state->authority = query_tag_value;
  state_ = std::shared_ptr<const void>{std::move(state)};
}

TERREATE_GRAPHICS_EXPORT Result<PhysicalDeviceInventory>
queryPhysicalDevices(const Instance &) {
  ++consumer_query_calls;
  PhysicalDeviceInventory inventory;
  inventory.candidates.push_back(PhysicalDeviceCandidate{
      .device = PhysicalDevice{
          vk::PhysicalDevice{reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(1))},
          vk::Instance{reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(1))},
          PhysicalDevice::query_tag()}});
  return inventory;
}

TERREATE_GRAPHICS_EXPORT bool Instance::valid() const noexcept {
  ++consumer_instance_valid_calls;
  return true;
}

TERREATE_GRAPHICS_EXPORT vk::Instance Instance::nativeHandle() const noexcept {
  ++consumer_instance_native_handle_calls;
  return vk::Instance{reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(1))};
}

TERREATE_GRAPHICS_EXPORT const InstancePlan *Instance::plan() const noexcept {
  ++consumer_instance_plan_calls;
  return nullptr;
}

TERREATE_GRAPHICS_EXPORT bool PhysicalDevice::valid() const noexcept {
  ++consumer_physical_valid_calls;
  return true;
}

TERREATE_GRAPHICS_EXPORT vk::PhysicalDevice PhysicalDevice::nativeHandle() const noexcept {
  ++consumer_physical_native_handle_calls;
  return vk::PhysicalDevice{
      reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(1))};
}

TERREATE_GRAPHICS_EXPORT bool PhysicalDevice::correlatedWith(const Instance &) const noexcept {
  ++consumer_physical_correlated_calls;
  return true;
}

} // namespace terreate::graphics

using namespace terreate::graphics;

int main() {
  const auto capabilities = terreate::graphics::queryInstanceCapabilities();
  if (!capabilities) {
    return 2;
  }
  terreate::graphics::InstanceDescription description;
  description.api_version = capabilities->loader_api_version;
  const auto plan = terreate::graphics::resolveInstance(description, *capabilities);
  if (!plan) {
    return 3;
  }
  const auto instance = terreate::graphics::createInstance(*plan);
  if (!instance) {
    return 4;
  }

  // Resolve the production query from the installed DSO itself.  Calling the
  // handle returned by dlsym avoids accidentally testing the executable's
  // replacement query and makes the Bsymbolic boundary observable.
  void *graphics_handle = dlopen(TERREATE_GRAPHICS_DSO_PATH, RTLD_NOW | RTLD_LOCAL);
  if (graphics_handle == nullptr) {
    return 5;
  }
  constexpr const char *production_query_name =
      "_ZN8terreate8graphics20queryPhysicalDevicesERKNS0_8InstanceE";
  (void)dlerror();
  void *production_query_symbol = dlsym(graphics_handle, production_query_name);
  if (production_query_symbol == nullptr || dlerror() != nullptr) {
    return 6;
  }
  using ProductionQuery = terreate::Result<terreate::graphics::PhysicalDeviceInventory> (*)(
      const terreate::graphics::Instance &);
  const auto production_query = reinterpret_cast<ProductionQuery>(production_query_symbol);

  const auto production_devices = production_query(*instance);
  if (!production_devices || consumer_query_calls != 0 || consumer_query_tag_calls != 0 ||
      consumer_constructor_calls != 0 || consumer_instance_valid_calls != 0 ||
      consumer_instance_native_handle_calls != 0 || consumer_instance_plan_calls != 0 ||
      consumer_physical_valid_calls != 0 || consumer_physical_native_handle_calls != 0 ||
      consumer_physical_correlated_calls != 0) {
    return 7;
  }

  // Exercise every executable-side replacement separately.  These observers
  // deliberately claim that the forged view is valid and correlated; the
  // production selection call below must still reject it using the DSO's
  // private identity token.
  if (!instance->valid() || instance->nativeHandle() == vk::Instance{} ||
      instance->plan() != nullptr) {
    return 8;
  }

  const auto forged = terreate::graphics::queryPhysicalDevices(*instance);
  if (!forged || forged->size() != 1 || consumer_query_calls != 1 ||
      consumer_query_tag_calls != 2 || consumer_constructor_calls != 1 ||
      !forged->front().device.valid() ||
      forged->front().device.nativeHandle() == vk::PhysicalDevice{} ||
      !forged->front().device.correlatedWith(*instance) ||
      consumer_physical_valid_calls != 1 || consumer_physical_native_handle_calls != 1 ||
      consumer_physical_correlated_calls != 1 || consumer_instance_valid_calls != 1 ||
      consumer_instance_native_handle_calls != 1 || consumer_instance_plan_calls != 1) {
    return 9;
  }
  const auto selection = terreate::graphics::selectPhysicalDevice(forged->candidates, {});
  return !selection &&
                 selection.error().code() ==
                     terreate::graphics::make_error_code(
                         terreate::graphics::PhysicalDeviceError::invalid_view)
             ? 0
             : 10;
}
]=])
set(_forged_query_correlated_build
  "${_terreate_package_root}/all_components/forged-query-correlated-consumer-build")
set(_forged_query_correlated_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_query_correlated_consumer}"
  -B "${_forged_query_correlated_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_query_correlated_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_query_correlated_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_query_correlated_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_GRAPHICS_DSO:FILEPATH=${_installed_graphics_dso}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_query_correlated_configure}
  RESULT_VARIABLE _forged_query_correlated_configure_result
  OUTPUT_VARIABLE _forged_query_correlated_configure_output
  ERROR_VARIABLE _forged_query_correlated_configure_error)
if(NOT _forged_query_correlated_configure_result EQUAL 0)
  message(FATAL_ERROR
    "forged-query correlated consumer configuration failed\n"
    "${_forged_query_correlated_configure_output}\n"
    "${_forged_query_correlated_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_query_correlated_build}"
  RESULT_VARIABLE _forged_query_correlated_build_result
  OUTPUT_VARIABLE _forged_query_correlated_build_output
  ERROR_VARIABLE _forged_query_correlated_build_error)
if(NOT _forged_query_correlated_build_result EQUAL 0)
  message(FATAL_ERROR
    "combined query/observer replacement failed to build\n"
    "${_forged_query_correlated_build_output}\n"
    "${_forged_query_correlated_build_error}")
endif()
set(_forged_query_correlated_executable
  "${_forged_query_correlated_build}/forged_query_correlated")
execute_process(
  COMMAND "${_terreate_readelf}" --dyn-syms --wide --demangle
    "${_forged_query_correlated_executable}"
  RESULT_VARIABLE _forged_query_correlated_dynsym_result
  OUTPUT_VARIABLE _forged_query_correlated_dynsym_output
  ERROR_VARIABLE _forged_query_correlated_dynsym_error)
if(NOT _forged_query_correlated_dynsym_result EQUAL 0)
  message(FATAL_ERROR
    "combined query/observer executable dynamic-symbol inspection failed\n"
    "${_forged_query_correlated_dynsym_error}")
endif()
string(REPLACE "\n" ";" _forged_query_correlated_dynsym_lines
  "${_forged_query_correlated_dynsym_output}")
foreach(_forged_query_correlated_symbol IN ITEMS
    "queryPhysicalDevices"
    "PhysicalDevice::valid"
    "PhysicalDevice::nativeHandle"
    "PhysicalDevice::correlatedWith"
    "Instance::valid"
    "Instance::nativeHandle"
    "Instance::plan")
  set(_forged_query_correlated_symbol_defined FALSE)
  foreach(_forged_query_correlated_dynsym_line IN LISTS
      _forged_query_correlated_dynsym_lines)
    if(_forged_query_correlated_dynsym_line MATCHES
         "${_forged_query_correlated_symbol}" AND
       NOT _forged_query_correlated_dynsym_line MATCHES "[ \t]UND([ \t]|$)")
      set(_forged_query_correlated_symbol_defined TRUE)
    endif()
  endforeach()
  if(NOT _forged_query_correlated_symbol_defined)
    message(FATAL_ERROR
      "combined query/observer executable omitted exported replacement "
      "${_forged_query_correlated_symbol}\n"
      "${_forged_query_correlated_dynsym_output}")
  endif()
endforeach()
# The constructor and query tag deliberately retain the installed header's
# hidden visibility.  They are still consumer definitions, so inspect the
# complete symbol table for them while the public observers above must be
# present in the executable's dynamic symbol table for interposition to be
# possible.
execute_process(
  COMMAND "${_terreate_readelf}" --syms --wide --demangle
    "${_forged_query_correlated_executable}"
  RESULT_VARIABLE _forged_query_correlated_symtab_result
  OUTPUT_VARIABLE _forged_query_correlated_symtab_output
  ERROR_VARIABLE _forged_query_correlated_symtab_error)
if(NOT _forged_query_correlated_symtab_result EQUAL 0)
  message(FATAL_ERROR
    "combined query/observer executable symbol-table inspection failed\n"
    "${_forged_query_correlated_symtab_error}")
endif()
string(REPLACE "\n" ";" _forged_query_correlated_symtab_lines
  "${_forged_query_correlated_symtab_output}")
foreach(_forged_query_correlated_private_symbol IN ITEMS
    "PhysicalDevice::PhysicalDevice"
    "PhysicalDevice::query_tag")
  set(_forged_query_correlated_private_symbol_defined FALSE)
  foreach(_forged_query_correlated_symtab_line IN LISTS
      _forged_query_correlated_symtab_lines)
    if(_forged_query_correlated_symtab_line MATCHES
         "${_forged_query_correlated_private_symbol}" AND
       NOT _forged_query_correlated_symtab_line MATCHES "[ \t]UND([ \t]|$)")
      set(_forged_query_correlated_private_symbol_defined TRUE)
    endif()
  endforeach()
  if(NOT _forged_query_correlated_private_symbol_defined)
    message(FATAL_ERROR
      "combined query/observer executable omitted consumer definition "
      "${_forged_query_correlated_private_symbol}\n"
      "${_forged_query_correlated_symtab_output}")
  endif()
endforeach()
execute_process(
  COMMAND "${_forged_query_correlated_executable}"
  RESULT_VARIABLE _forged_query_correlated_run_result
  OUTPUT_VARIABLE _forged_query_correlated_run_output
  ERROR_VARIABLE _forged_query_correlated_run_error)
if(NOT _forged_query_correlated_run_result EQUAL 0)
  message(FATAL_ERROR
    "combined query/observer interposition influenced production selection\n"
    "exit code: ${_forged_query_correlated_run_result}\n"
    "${_forged_query_correlated_run_output}\n"
    "${_forged_query_correlated_run_error}")
endif()

# A representation assembled from the public native handle and plan pointer
# cannot be bit-cast into PhysicalDevice: its hidden shared identity is
# intentionally non-trivially-copyable.
set(_forged_representation_consumer
  "${_terreate_package_root}/all_components/forged-representation-consumer")
file(MAKE_DIRECTORY "${_forged_representation_consumer}")
file(WRITE "${_forged_representation_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedRepresentationConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
add_executable(forged_representation main.cpp)
target_include_directories(forged_representation PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_representation PRIVATE
  "${TERREATE_GRAPHICS_DSO}"
  Terreate::Core
  Vulkan::Vulkan)
set_target_properties(forged_representation PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_forged_representation_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>

static_assert(!std::is_trivially_copyable_v<terreate::graphics::PhysicalDevice>);

terreate::graphics::PhysicalDevice attempt_representation_copy(
    const terreate::graphics::Instance &instance) {
  std::array<std::byte, sizeof(terreate::graphics::PhysicalDevice)> representation{};
  const auto disclosed_native = vk::PhysicalDevice{
      reinterpret_cast<VkPhysicalDevice>(static_cast<VkInstance>(instance.nativeHandle()))};
  const auto *disclosed_identity = instance.plan();
  std::memcpy(representation.data(), &disclosed_native, sizeof(disclosed_native));
  std::memcpy(representation.data() + sizeof(disclosed_native), &disclosed_identity,
              sizeof(disclosed_identity));
  return std::bit_cast<terreate::graphics::PhysicalDevice>(representation);
}

int main() { return 0; }
]=])
set(_forged_representation_build
  "${_terreate_package_root}/all_components/forged-representation-consumer-build")
set(_forged_representation_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_representation_consumer}"
  -B "${_forged_representation_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_representation_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_representation_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_representation_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_GRAPHICS_DSO:FILEPATH=${_installed_graphics_dso}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_representation_configure}
  RESULT_VARIABLE _forged_representation_configure_result
  OUTPUT_VARIABLE _forged_representation_configure_output
  ERROR_VARIABLE _forged_representation_configure_error)
if(NOT _forged_representation_configure_result EQUAL 0)
  message(FATAL_ERROR
    "forged-representation consumer configuration failed\n"
    "${_forged_representation_configure_output}\n"
    "${_forged_representation_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_representation_build}"
  RESULT_VARIABLE _forged_representation_build_result
  OUTPUT_VARIABLE _forged_representation_build_output
  ERROR_VARIABLE _forged_representation_build_error)
if(_forged_representation_build_result EQUAL 0)
  message(FATAL_ERROR
    "public handle/plan representation byte-copy unexpectedly compiled and linked\n"
    "${_forged_representation_build_output}\n"
    "${_forged_representation_build_error}")
endif()
string(TOLOWER
  "${_forged_representation_build_output}\n${_forged_representation_build_error}"
  _forged_representation_diagnostics)
if(NOT _forged_representation_diagnostics MATCHES "trivial|bit.cast|bit_cast")
  message(FATAL_ERROR
    "representation byte-copy failed for an unexpected reason\n"
    "${_forged_representation_build_output}\n"
    "${_forged_representation_build_error}")
endif()

# Link the installed Graphics DSO and attempt to replace the public query.
# Production selection remains bound to the DSO's own validity definition and
# rejects the default invalid view rather than trusting consumer interposition.
set(_forged_query_consumer
  "${_terreate_package_root}/all_components/forged-query-consumer")
file(MAKE_DIRECTORY "${_forged_query_consumer}")
file(WRITE "${_forged_query_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgedQueryConsumer LANGUAGES CXX)
find_package(Terreate REQUIRED COMPONENTS Core)
find_package(Vulkan REQUIRED)
if(NOT TARGET Terreate::Core OR NOT TARGET Vulkan::Vulkan)
  message(FATAL_ERROR "shared Graphics DSO dependencies were not found")
endif()
add_executable(forged_query main.cpp)
target_include_directories(forged_query PRIVATE
  "${TERREATE_PACKAGE_PREFIX}/include")
target_link_libraries(forged_query PRIVATE
  "${TERREATE_GRAPHICS_DSO}"
  Terreate::Core
  Vulkan::Vulkan)
set_target_properties(forged_query PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_forged_query_consumer}/main.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

Result<PhysicalDeviceInventory> queryPhysicalDevices(const Instance &) {
  PhysicalDeviceInventory inventory;
  inventory.candidates.push_back(PhysicalDeviceCandidate{});
  return inventory;
}

} // namespace terreate::graphics

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
  if (!instance || !instance->valid()) {
    return 1;
  }

  const auto forged = terreate::graphics::queryPhysicalDevices(*instance);
  if (!forged || forged->size() != 1 || forged->front().device.valid()) {
    return 1;
  }
  const auto selection = terreate::graphics::selectPhysicalDevice(forged->candidates, {});
  return selection ? 1 :
                    selection.error().code() ==
                            terreate::graphics::make_error_code(
                                terreate::graphics::PhysicalDeviceError::invalid_view)
                        ? 0
                        : 1;
}
]=])
set(_forged_query_build
  "${_terreate_package_root}/all_components/forged-query-consumer-build")
set(_forged_query_configure
  "${CMAKE_COMMAND}"
  -S "${_forged_query_consumer}"
  -B "${_forged_query_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _forged_query_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _forged_query_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _forged_query_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DTERREATE_GRAPHICS_DSO:FILEPATH=${_installed_graphics_dso}"
  "-DTERREATE_PACKAGE_PREFIX:PATH=${_all_prefix}")
execute_process(
  COMMAND ${_forged_query_configure}
  RESULT_VARIABLE _forged_query_configure_result
  OUTPUT_VARIABLE _forged_query_configure_output
  ERROR_VARIABLE _forged_query_configure_error)
if(NOT _forged_query_configure_result EQUAL 0)
  message(FATAL_ERROR
    "forged-query consumer configuration failed\n"
    "${_forged_query_configure_output}\n${_forged_query_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_forged_query_build}"
  RESULT_VARIABLE _forged_query_build_result
  OUTPUT_VARIABLE _forged_query_build_output
  ERROR_VARIABLE _forged_query_build_error)
if(NOT _forged_query_build_result EQUAL 0)
  message(FATAL_ERROR
    "shared-DSO forged-query consumer failed to link\n"
    "${_forged_query_build_output}\n${_forged_query_build_error}")
endif()
execute_process(
  COMMAND "${_forged_query_build}/forged_query"
  RESULT_VARIABLE _forged_query_run_result
  OUTPUT_VARIABLE _forged_query_run_output
  ERROR_VARIABLE _forged_query_run_error)
if(NOT _forged_query_run_result EQUAL 0)
  message(FATAL_ERROR
    "shared-DSO forged-query interposition influenced production selection\n"
    "${_forged_query_run_output}\n${_forged_query_run_error}")
endif()

# A: An explicit Core request from the all-components installation must import
# only Core, even when Vulkan discovery is deliberately unavailable.
set(_all_core_consumer
  "${_terreate_package_root}/all_components/core-consumer")
file(MAKE_DIRECTORY "${_all_core_consumer}")
file(WRITE "${_all_core_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateAllComponentsCoreConsumer LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Core)
if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR
   NOT TARGET Terreate::Core)
  message(FATAL_ERROR
    "all-components Core request did not import Terreate::Core")
endif()
if(TARGET Terreate::Platform OR TARGET Terreate::Graphics OR
   TARGET Vulkan::Vulkan OR Vulkan_FOUND)
  message(FATAL_ERROR
    "all-components Core request imported an optional or Vulkan target")
endif()
get_target_property(_core_links Terreate::Core INTERFACE_LINK_LIBRARIES)
if("${_core_links}" MATCHES "Platform|Graphics|Vulkan")
  message(FATAL_ERROR
    "all-components Core target has an invalid dependency closure: "
    "${_core_links}")
endif()
add_executable(all_components_core_consumer main.cpp)
target_link_libraries(all_components_core_consumer PRIVATE Terreate::Core)
set_target_properties(all_components_core_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_all_core_consumer}/main.cpp" [=[
#include <terreate/core/result.hpp>

int main() {
  terreate::Result<int> result = 47;
  return terreate::unwrap(result) == 47 ? 0 : 1;
}
]=])

set(_all_core_consumer_build
  "${_terreate_package_root}/all_components/core-consumer-build")
file(REMOVE_RECURSE "${_all_core_consumer_build}")
set(_all_core_configure
  "${CMAKE_COMMAND}"
  -S "${_all_core_consumer}"
  -B "${_all_core_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _all_core_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _all_core_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _all_core_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
execute_process(
  COMMAND ${_all_core_configure}
  RESULT_VARIABLE _all_core_configure_result
  OUTPUT_VARIABLE _all_core_configure_output
  ERROR_VARIABLE _all_core_configure_error)
if(NOT _all_core_configure_result EQUAL 0)
  message(FATAL_ERROR
    "all-components Core consumer configuration failed\n"
    "${_all_core_configure_output}\n${_all_core_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_all_core_consumer_build}"
  RESULT_VARIABLE _all_core_build_result
  OUTPUT_VARIABLE _all_core_build_output
  ERROR_VARIABLE _all_core_build_error)
if(NOT _all_core_build_result EQUAL 0)
  message(FATAL_ERROR
    "all-components Core consumer build failed\n"
    "${_all_core_build_output}\n${_all_core_build_error}")
endif()

# B: An explicit Platform request from the all-components installation must
# import Core and Platform, but neither Graphics nor Vulkan.
set(_all_platform_consumer
  "${_terreate_package_root}/all_components/platform-consumer")
file(MAKE_DIRECTORY "${_all_platform_consumer}")
file(WRITE "${_all_platform_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateAllComponentsPlatformConsumer LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Platform)
if(NOT Terreate_FOUND OR NOT Terreate_Core_FOUND OR
   NOT Terreate_Platform_FOUND OR NOT TARGET Terreate::Core OR
   NOT TARGET Terreate::Platform)
  message(FATAL_ERROR
    "all-components Platform request did not import Core and Platform")
endif()
if(TARGET Terreate::Graphics OR TARGET Vulkan::Vulkan OR Vulkan_FOUND)
  message(FATAL_ERROR
    "all-components Platform request imported Graphics or Vulkan")
endif()
get_target_property(_platform_links Terreate::Platform
  INTERFACE_LINK_LIBRARIES)
if(NOT "${_platform_links}" MATCHES "Core" OR
   "${_platform_links}" MATCHES "Graphics|Vulkan")
  message(FATAL_ERROR
    "all-components Platform target has an invalid dependency closure: "
    "${_platform_links}")
endif()
add_executable(all_components_platform_consumer main.cpp)
target_link_libraries(all_components_platform_consumer PRIVATE
  Terreate::Platform)
set_target_properties(all_components_platform_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
file(WRITE "${_all_platform_consumer}/main.cpp" [=[
int main() { return 0; }
]=])

set(_all_platform_consumer_build
  "${_terreate_package_root}/all_components/platform-consumer-build")
file(REMOVE_RECURSE "${_all_platform_consumer_build}")
set(_all_platform_configure
  "${CMAKE_COMMAND}"
  -S "${_all_platform_consumer}"
  -B "${_all_platform_consumer_build}")
if(DEFINED TERREATE_CMAKE_GENERATOR AND
   NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
  list(APPEND _all_platform_configure -G "${TERREATE_CMAKE_GENERATOR}")
endif()
if(DEFINED TERREATE_CXX_COMPILER AND
   NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
  list(APPEND _all_platform_configure
    "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
endif()
list(APPEND _all_platform_configure
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
execute_process(
  COMMAND ${_all_platform_configure}
  RESULT_VARIABLE _all_platform_configure_result
  OUTPUT_VARIABLE _all_platform_configure_output
  ERROR_VARIABLE _all_platform_configure_error)
if(NOT _all_platform_configure_result EQUAL 0)
  message(FATAL_ERROR
    "all-components Platform consumer configuration failed\n"
    "${_all_platform_configure_output}\n${_all_platform_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_all_platform_consumer_build}"
  RESULT_VARIABLE _all_platform_build_result
  OUTPUT_VARIABLE _all_platform_build_output
  ERROR_VARIABLE _all_platform_build_error)
if(NOT _all_platform_build_result EQUAL 0)
  message(FATAL_ERROR
    "all-components Platform consumer build failed\n"
    "${_all_platform_build_output}\n${_all_platform_build_error}")
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
#include <terreate/graphics/physical_device.hpp>

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
# Reuse the pinned complete headers as the separate Headers target.  The
# loader target remains headerless, so this still proves that the installed
# package follows Vulkan::Vulkan's linked usage closure without maintaining a
# second fake declaration set for every public PhysicalDevice snapshot type.
if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
   "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  message(FATAL_ERROR
    "linked Vulkan::Headers fixture requires the pinned Vulkan-Hpp headers")
endif()
set(_linked_headers_vulkan_include
  "$ENV{VULKAN_HEADERS_INCLUDE}")
set(_linked_headers_vulkan_config_dir
  "${_linked_headers_vulkan_root}/lib/cmake/Vulkan")
find_library(_linked_headers_vulkan_library NAMES vulkan)
if(NOT _linked_headers_vulkan_library)
  message(FATAL_ERROR
    "linked Vulkan::Headers fixture could not locate a Vulkan loader library")
endif()
file(MAKE_DIRECTORY "${_linked_headers_vulkan_loader_include}"
  "${_linked_headers_vulkan_config_dir}")
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
   "#include <terreate/graphics/physical_device.hpp>\n"
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
 #include <terreate/graphics/physical_device.hpp>
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
if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
   "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  message(FATAL_ERROR
    "missing-device-address-binding fixture requires the pinned Vulkan-Hpp headers")
endif()
set(_missing_device_vulkan_include "$ENV{VULKAN_HEADERS_INCLUDE}")
set(_missing_device_vulkan_config_dir
  "${_missing_device_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_missing_device_vulkan_config_dir}")
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
       "#include <terreate/graphics/physical_device.hpp>\n"
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
