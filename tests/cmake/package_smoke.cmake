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
set(_terreate_package_root "${_terreate_package_binary_root}/generated")
file(REMOVE_RECURSE "${_terreate_package_root}")
file(MAKE_DIRECTORY "${_terreate_package_root}")

function(terreate_append_generator_and_compiler command_variable)
  if(DEFINED TERREATE_CMAKE_GENERATOR AND
     NOT "${TERREATE_CMAKE_GENERATOR}" STREQUAL "")
    list(APPEND ${command_variable} -G "${TERREATE_CMAKE_GENERATOR}")
  endif()
  if(DEFINED TERREATE_CXX_COMPILER AND
     NOT "${TERREATE_CXX_COMPILER}" STREQUAL "")
    list(APPEND ${command_variable}
      "-DCMAKE_CXX_COMPILER=${TERREATE_CXX_COMPILER}")
  endif()
  set(${command_variable} "${${command_variable}}" PARENT_SCOPE)
endfunction()

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

  file(GLOB_RECURSE _installed_paths LIST_DIRECTORIES true "${prefix}/*")
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
  terreate_append_generator_and_compiler(_configure_command)
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

function(terreate_build_consumer name prefix component source)
  set(_consumer "${_terreate_package_root}/${name}/consumer")
  set(_build "${_terreate_package_root}/${name}/consumer-build")
  file(REMOVE_RECURSE "${_consumer}" "${_build}")
  file(MAKE_DIRECTORY "${_consumer}")
  file(WRITE "${_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreatePackageConsumer LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS ]=] "${component}" [=[)
add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE Terreate::]=] "${component}" [=[)
set_target_properties(package_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
  file(WRITE "${_consumer}/main.cpp" "${source}")

  set(_configure_command
    "${CMAKE_COMMAND}" -S "${_consumer}" -B "${_build}"
    "-DCMAKE_PREFIX_PATH:PATH=${prefix}")
  terreate_append_generator_and_compiler(_configure_command)
  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "${name} consumer configuration failed\n"
      "${_configure_output}\n${_configure_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
      "${name} consumer build failed\n"
      "${_build_output}\n${_build_error}")
  endif()
  execute_process(
    COMMAND "${_build}/package_consumer"
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_output
    ERROR_VARIABLE _run_error)
  if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR
      "${name} consumer failed at runtime\n"
      "${_run_output}\n${_run_error}")
  endif()
endfunction()

function(terreate_expect_compile_failure name source diagnostics)
  set(_consumer "${_terreate_package_root}/all_components/${name}-consumer")
  set(_build "${_terreate_package_root}/all_components/${name}-consumer-build")
  file(REMOVE_RECURSE "${_consumer}" "${_build}")
  file(MAKE_DIRECTORY "${_consumer}")
  file(WRITE "${_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateForgeRegression LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS Graphics)
add_executable(forge_regression main.cpp)
target_link_libraries(forge_regression PRIVATE Terreate::Graphics)
set_target_properties(forge_regression PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
  file(WRITE "${_consumer}/main.cpp" "${source}")
  set(_configure_command
    "${CMAKE_COMMAND}" -S "${_consumer}" -B "${_build}"
    "-DCMAKE_PREFIX_PATH:PATH=${all_components_PREFIX}")
  terreate_append_generator_and_compiler(_configure_command)
  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "${name} forge regression configuration failed\n"
      "${_configure_output}\n${_configure_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(_build_result EQUAL 0)
    message(FATAL_ERROR
      "${name} forge regression unexpectedly compiled\n"
      "${_build_output}")
  endif()
  string(TOLOWER "${_build_output}\n${_build_error}" _diagnostics)
  if(NOT _diagnostics MATCHES "${diagnostics}")
    message(FATAL_ERROR
      "${name} forge regression failed for an unexpected reason\n"
      "${_build_output}\n${_build_error}")
  endif()
endfunction()

function(terreate_expect_link_failure name source diagnostics)
  set(_consumer "${_terreate_package_root}/all_components/${name}-consumer")
  set(_build "${_terreate_package_root}/all_components/${name}-consumer-build")
  file(REMOVE_RECURSE "${_consumer}" "${_build}")
  file(MAKE_DIRECTORY "${_consumer}")
  file(WRITE "${_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateLinkForgeRegression LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS Graphics)
add_executable(link_forge_regression main.cpp)
target_link_libraries(link_forge_regression PRIVATE Terreate::Graphics)
set_target_properties(link_forge_regression PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
]=])
  file(WRITE "${_consumer}/main.cpp" "${source}")
  set(_configure_command
    "${CMAKE_COMMAND}" -S "${_consumer}" -B "${_build}"
    "-DCMAKE_PREFIX_PATH:PATH=${all_components_PREFIX}")
  terreate_append_generator_and_compiler(_configure_command)
  execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
  if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
      "${name} link regression configuration failed"
      "${_configure_output}"
      "${_configure_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
  if(_build_result EQUAL 0)
    message(FATAL_ERROR
      "${name} link regression unexpectedly succeeded"
      "${_build_output}")
  endif()
  string(TOLOWER "${_build_output} ${_build_error}" _diagnostics)
  if(NOT _diagnostics MATCHES "${diagnostics}")
    message(FATAL_ERROR
      "${name} link regression failed for an unexpected reason"
      "${_build_output}"
      "${_build_error}")
  endif()
endfunction()

terreate_package_configure_and_install(all_components ON ON)
set(_all_prefix "${all_components_PREFIX}")

if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
   "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  message(FATAL_ERROR
    "static Graphics package regression requires an explicit Vulkan-Hpp root")
endif()

file(GLOB_RECURSE _installed_graphics_archives
  "${_all_prefix}/*/libterreate_graphics.a")
list(LENGTH _installed_graphics_archives _installed_graphics_archive_count)
if(NOT _installed_graphics_archive_count EQUAL 1)
  message(FATAL_ERROR
    "installed package did not provide exactly one Graphics archive: "
    "${_installed_graphics_archives}")
endif()
file(GLOB_RECURSE _installed_graphics_shared
  "${_all_prefix}/*/libterreate_graphics.so*"
  "${_all_prefix}/*/libterreate_graphics.dylib"
  "${_all_prefix}/*/terreate_graphics.dll")
if(_installed_graphics_shared)
  message(FATAL_ERROR
    "installed static Graphics package exposed a shared artifact: "
    "${_installed_graphics_shared}")
endif()

set(_installed_config_candidates)
file(GLOB_RECURSE _installed_config_candidates
  "${_all_prefix}/*/TerreateConfig.cmake")
if(NOT _installed_config_candidates)
  message(FATAL_ERROR "installed package config is missing")
endif()
list(GET _installed_config_candidates 0 _installed_config)
file(READ "${_installed_config}" _installed_config_contents)
if(_installed_config_contents MATCHES "VULKAN_HEADERS_INCLUDE" OR
   _installed_config_contents MATCHES "${_terreate_source_dir}")
  message(FATAL_ERROR
    "installed package config leaked a build-only Vulkan/source path")
endif()
file(GLOB_RECURSE _installed_graphics_targets
  "${_all_prefix}/*/TerreateGraphicsTargets.cmake")
if(NOT _installed_graphics_targets)
  message(FATAL_ERROR "installed Graphics target export is missing")
endif()
list(GET _installed_graphics_targets 0 _installed_graphics_target)
file(READ "${_installed_graphics_target}" _installed_graphics_target_contents)
foreach(_forbidden_graphics_marker IN ITEMS
    "SHARED_LIBRARY" ".so" "Bsymbolic" "z,defs" "exclude-libs")
  string(FIND "${_installed_graphics_target_contents}"
    "${_forbidden_graphics_marker}" _forbidden_graphics_offset)
  if(NOT _forbidden_graphics_offset EQUAL -1)
    message(FATAL_ERROR
      "installed Graphics export retained a DSO-only selection boundary: "
      "${_forbidden_graphics_marker}")
  endif()
endforeach()

set(_graphics_consumer_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(std::is_copy_constructible_v<terreate::graphics::PhysicalDevice>);
static_assert(std::is_trivially_copyable_v<terreate::graphics::PhysicalDevice>);
static_assert(sizeof(terreate::graphics::PhysicalDevice) <= 2 * sizeof(void *));
static_assert(!std::is_constructible_v<terreate::graphics::PhysicalDevice,
                                      vk::PhysicalDevice, const void *>);
static_assert(!std::is_convertible_v<terreate::graphics::PhysicalDevice,
                                     vk::PhysicalDevice>);

int main() {
  const auto capabilities = terreate::graphics::queryInstanceCapabilities();
  if (!capabilities) {
    return 1;
  }
  terreate::graphics::InstanceDescription description;
  description.api_version = capabilities->loader_api_version;
  const auto plan = terreate::graphics::resolveInstance(description, *capabilities);
  if (!plan) {
    return 2;
  }
  const auto instance = terreate::graphics::createInstance(*plan);
  if (!instance || !instance->valid()) {
    return 3;
  }
  const auto devices = terreate::graphics::queryPhysicalDevices(*instance);
  if (!devices || devices->empty()) {
    return devices.has_value() ? 5 : 4;
  }

  const auto &candidate = devices->front();
  if (!candidate.device.valid() || !candidate.device.correlatedWith(*instance)) {
    return 6;
  }
  auto forged_bytes =
      std::bit_cast<std::array<std::byte, sizeof(terreate::graphics::PhysicalDevice)>>(candidate.device);
  const auto replacement_handle = std::bit_cast<VkPhysicalDevice>(std::uintptr_t{1});
  const auto replacement = vk::PhysicalDevice{replacement_handle};
  const auto replacement_bytes =
      std::bit_cast<std::array<std::byte, sizeof(vk::PhysicalDevice)>>(replacement);
  std::copy(replacement_bytes.begin(), replacement_bytes.end(), forged_bytes.begin());
  const auto forged = std::bit_cast<terreate::graphics::PhysicalDevice>(forged_bytes);
  if (forged.valid() || forged.nativeHandle() != vk::PhysicalDevice{} ||
      forged.correlatedWith(*instance)) {
    return 7;
  }

  const terreate::graphics::PhysicalDeviceCandidate forged_candidate{
      .device = forged,
      .capabilities = candidate.capabilities,
      .enumeration_index = candidate.enumeration_index,
  };
  const auto selection = terreate::graphics::selectPhysicalDevice(
      forged_candidate, terreate::graphics::PhysicalDeviceRequirements{});
  return !selection && selection.error().code() ==
                            terreate::graphics::make_error_code(
                                terreate::graphics::PhysicalDeviceError::invalid_view)
             ? 0
             : 8;
}
]=])
terreate_build_consumer(all_components_runtime "${_all_prefix}" Graphics
  "${_graphics_consumer_source}")

set(_forged_raw_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

terreate::graphics::PhysicalDevice forge(
    const terreate::graphics::Instance &instance) {
  const auto native = vk::PhysicalDevice{};
  return terreate::graphics::PhysicalDevice{
      native, static_cast<const void *>(instance.plan())};
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(forged_public_constructor "${_forged_raw_source}"
  "private|inaccessible|no matching|constructible")

set(_forged_token_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

int main() {
  const terreate::graphics::PhysicalDevice device;
  return device.instanceIdentity() == nullptr ? 0 : 1;
}
]=])
terreate_expect_compile_failure(forged_public_token "${_forged_token_source}"
  "no member|private|inaccessible")

set(_forged_impl_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

int main() {
  using forged_impl = terreate::graphics::Instance::Impl;
  return sizeof(forged_impl);
}
]=])
terreate_expect_compile_failure(forged_instance_impl "${_forged_impl_source}"
  "private|inaccessible|incomplete")

set(_forged_authority_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

int main() {
  using forged_authority = terreate::graphics::PhysicalDevice::QueryAuthority;
  return sizeof(forged_authority);
}
]=])
terreate_expect_compile_failure(forged_query_authority "${_forged_authority_source}"
  "private|inaccessible|incomplete")

set(_combined_authority_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

// This fixture is intentionally a consumer-side replacement attempt.  It
// combines the friend query seam with every private PhysicalDevice authority
// member that could otherwise let a static archive avoid its production
// validation object.
struct PhysicalDevice::QueryAuthority {};

const PhysicalDevice::QueryAuthority *PhysicalDevice::query_authority() noexcept {
  static const QueryAuthority authority;
  return &authority;
}

PhysicalDevice::PhysicalDevice(vk::PhysicalDevice,
                               const Instance::PhysicalDeviceToken *,
                               const QueryAuthority *) noexcept {}

PhysicalDevice::operator bool() const noexcept { return true; }

bool PhysicalDevice::valid() const noexcept { return true; }

vk::PhysicalDevice PhysicalDevice::nativeHandle() const noexcept { return {}; }

bool PhysicalDevice::correlatedWith(const Instance &) const noexcept { return true; }

terreate::Result<PhysicalDeviceInventory>
queryPhysicalDevices(const Instance &) {
  return PhysicalDeviceInventory{};
}

} // namespace terreate::graphics

int main() {
  // These genuine Instance calls force the archive's instance.cpp object to
  // be extracted.  Its authoritative PhysicalDevice definitions must then
  // collide with the replacement definitions above.
  const auto capabilities = terreate::graphics::queryInstanceCapabilities();
  if (!capabilities) {
    return 1;
  }
  terreate::graphics::InstanceDescription description;
  description.api_version = capabilities->loader_api_version;
  const auto plan = terreate::graphics::resolveInstance(description, *capabilities);
  if (!plan) {
    return 2;
  }
  const auto instance = terreate::graphics::createInstance(*plan);
  if (!instance || !instance->valid() || instance->nativeHandle() == vk::Instance{}) {
    return 3;
  }
  const auto devices = terreate::graphics::queryPhysicalDevices(*instance);
  return devices.has_value() ? 4 : 5;
}
]=])
terreate_expect_link_failure(combined_authority_replacement
  "${_combined_authority_source}"
  "multiple definition|duplicate symbol|first defined here")

set(_selection_only_authority_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

namespace terreate::graphics {

// This fixture is deliberately limited to the selection API.  It supplies
// every private PhysicalDevice authority/validator definition that could let a
// static consumer avoid the production Instance object, but never constructs a
// genuine Instance.
struct PhysicalDevice::QueryAuthority {};

const PhysicalDevice::QueryAuthority *PhysicalDevice::query_authority() noexcept {
  static const QueryAuthority authority;
  return &authority;
}

PhysicalDevice::PhysicalDevice(vk::PhysicalDevice,
                               const Instance::PhysicalDeviceToken *,
                               const QueryAuthority *) noexcept {}

PhysicalDevice::operator bool() const noexcept { return true; }

bool PhysicalDevice::valid() const noexcept { return true; }

vk::PhysicalDevice PhysicalDevice::nativeHandle() const noexcept { return {}; }

bool PhysicalDevice::correlatedWith(const Instance &) const noexcept { return true; }

bool PhysicalDevice::capabilitiesMatch(const PhysicalDeviceCapabilities &) const noexcept {
  return true;
}

bool Instance::valid() const noexcept { return true; }

vk::Instance Instance::nativeHandle() const noexcept { return {}; }

const InstancePlan *Instance::plan() const noexcept { return nullptr; }

} // namespace terreate::graphics

int main() {
  // The replacement validators make this synthetic candidate appear valid.
  // Production selection must still pull the authoritative Instance object,
  // so linking this consumer must fail on duplicate definitions above.
  const terreate::graphics::PhysicalDeviceCandidate candidate{};
  const auto selection = terreate::graphics::selectPhysicalDevice(
      candidate, terreate::graphics::PhysicalDeviceRequirements{});
  return selection.has_value() ? 0 : 1;
}
]=])
terreate_expect_link_failure(selection_only_authority_replacement
  "${_selection_only_authority_source}"
  "multiple definition.*physicaldevice|duplicate symbol.*physicaldevice|physicaldevice.*multiple definition|physicaldevice.*duplicate symbol")

set(_all_component_config_consumer
  "${_terreate_package_root}/all_components/config-consumer")
set(_all_component_config_build
  "${_terreate_package_root}/all_components/config-consumer-build")
file(MAKE_DIRECTORY "${_all_component_config_consumer}")
file(WRITE "${_all_component_config_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreatePackageConfigConsumer LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS Core Graphics OPTIONAL_COMPONENTS Future)
if(NOT TARGET Terreate::Core OR NOT TARGET Terreate::Graphics)
  message(FATAL_ERROR "Core+Graphics package request omitted a requested target")
endif()
if(TARGET Terreate::Platform)
  message(FATAL_ERROR "Core+Graphics package request imported Platform")
endif()
if(DEFINED Terreate_Future_FOUND AND Terreate_Future_FOUND)
  message(FATAL_ERROR "unknown optional component was reported as found")
endif()
get_target_property(_graphics_type Terreate::Graphics TYPE)
if(NOT "${_graphics_type}" STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "Graphics package target is not STATIC_LIBRARY: ${_graphics_type}")
endif()
get_target_property(_graphics_links Terreate::Graphics INTERFACE_LINK_LIBRARIES)
if(NOT "${_graphics_links}" MATCHES "Core" OR
   NOT "${_graphics_links}" MATCHES "Vulkan::Vulkan" OR
   "${_graphics_links}" MATCHES "Platform")
  message(FATAL_ERROR "Graphics package dependency closure is invalid: ${_graphics_links}")
endif()
]=])
set(_config_command
  "${CMAKE_COMMAND}" -S "${_all_component_config_consumer}"
  -B "${_all_component_config_build}"
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}")
terreate_append_generator_and_compiler(_config_command)
execute_process(
  COMMAND ${_config_command}
  RESULT_VARIABLE _config_result
  OUTPUT_VARIABLE _config_output
  ERROR_VARIABLE _config_error)
if(NOT _config_result EQUAL 0)
  message(FATAL_ERROR
    "installed package configuration closure failed\n"
    "${_config_output}\n${_config_error}")
endif()

terreate_package_configure_and_install(core_only OFF OFF)
set(_core_source [=[
#include <terreate/core/result.hpp>
int main() {
  terreate::Result<int> result = 11;
  return terreate::unwrap(result) == 11 ? 0 : 1;
}
]=])
terreate_build_consumer(core_only_runtime "${core_only_PREFIX}" Core
  "${_core_source}")

terreate_package_configure_and_install(platform_only ON OFF)
set(_platform_source "int main() { return 0; }\n")
terreate_build_consumer(platform_only_runtime "${platform_only_PREFIX}" Platform
  "${_platform_source}")

terreate_package_configure_and_install(graphics_only OFF ON)
terreate_build_consumer(graphics_only_runtime "${graphics_only_PREFIX}" Graphics
  "${_graphics_consumer_source}")

set(_core_isolation_consumer
  "${_terreate_package_root}/all_components/core-isolation-consumer")
set(_core_isolation_build
  "${_terreate_package_root}/all_components/core-isolation-consumer-build")
file(MAKE_DIRECTORY "${_core_isolation_consumer}")
file(WRITE "${_core_isolation_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateCoreIsolationConsumer LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Core)
if(TARGET Terreate::Platform OR TARGET Terreate::Graphics OR
   TARGET Vulkan::Vulkan OR Vulkan_FOUND)
  message(FATAL_ERROR "Core-only package imported an optional/Vulkan target")
endif()
add_executable(core_isolation main.cpp)
target_link_libraries(core_isolation PRIVATE Terreate::Core)
]=])
file(WRITE "${_core_isolation_consumer}/main.cpp" "int main() { return 0; }\n")
set(_core_isolation_command
  "${CMAKE_COMMAND}" -S "${_core_isolation_consumer}"
  -B "${_core_isolation_build}"
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
terreate_append_generator_and_compiler(_core_isolation_command)
execute_process(
  COMMAND ${_core_isolation_command}
  RESULT_VARIABLE _core_isolation_result
  OUTPUT_VARIABLE _core_isolation_output
  ERROR_VARIABLE _core_isolation_error)
if(NOT _core_isolation_result EQUAL 0)
  message(FATAL_ERROR
    "Core package isolation configuration failed\n"
    "${_core_isolation_output}\n${_core_isolation_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_core_isolation_build}"
  RESULT_VARIABLE _core_isolation_build_result
  OUTPUT_VARIABLE _core_isolation_build_output
  ERROR_VARIABLE _core_isolation_build_error)
if(NOT _core_isolation_build_result EQUAL 0)
  message(FATAL_ERROR
    "Core package isolation build failed\n"
    "${_core_isolation_build_output}\n${_core_isolation_build_error}")
endif()

set(_optional_no_vulkan_consumer
  "${_terreate_package_root}/all_components/optional-no-vulkan-consumer")
set(_optional_no_vulkan_build
  "${_terreate_package_root}/all_components/optional-no-vulkan-consumer-build")
file(MAKE_DIRECTORY "${_optional_no_vulkan_consumer}")
file(WRITE "${_optional_no_vulkan_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateOptionalGraphicsWithoutVulkan LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Core OPTIONAL_COMPONENTS Graphics)
if(NOT Terreate_Core_FOUND OR NOT TARGET Terreate::Core OR
   Terreate_Graphics_FOUND OR TARGET Terreate::Graphics)
  message(FATAL_ERROR "optional Graphics changed Core-only package resolution")
endif()
if(NOT DEFINED Terreate_NOT_FOUND_MESSAGE OR
   NOT "${Terreate_NOT_FOUND_MESSAGE}" MATCHES "Vulkan")
  message(FATAL_ERROR "optional Graphics did not preserve the Vulkan diagnostic")
endif()
]=])
set(_optional_no_vulkan_command
  "${CMAKE_COMMAND}" -S "${_optional_no_vulkan_consumer}"
  -B "${_optional_no_vulkan_build}"
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
terreate_append_generator_and_compiler(_optional_no_vulkan_command)
execute_process(
  COMMAND ${_optional_no_vulkan_command}
  RESULT_VARIABLE _optional_no_vulkan_result
  OUTPUT_VARIABLE _optional_no_vulkan_output
  ERROR_VARIABLE _optional_no_vulkan_error)
if(NOT _optional_no_vulkan_result EQUAL 0)
  message(FATAL_ERROR
    "optional Graphics without Vulkan configuration failed\n"
    "${_optional_no_vulkan_output}\n${_optional_no_vulkan_error}")
endif()

set(_required_no_vulkan_consumer
  "${_terreate_package_root}/all_components/required-no-vulkan-consumer")
set(_required_no_vulkan_build
  "${_terreate_package_root}/all_components/required-no-vulkan-consumer-build")
file(MAKE_DIRECTORY "${_required_no_vulkan_consumer}")
file(WRITE "${_required_no_vulkan_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateRequiredGraphicsWithoutVulkan LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
find_package(Terreate REQUIRED COMPONENTS Graphics)
]=])
set(_required_no_vulkan_command
  "${CMAKE_COMMAND}" -S "${_required_no_vulkan_consumer}"
  -B "${_required_no_vulkan_build}"
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE)
terreate_append_generator_and_compiler(_required_no_vulkan_command)
execute_process(
  COMMAND ${_required_no_vulkan_command}
  RESULT_VARIABLE _required_no_vulkan_result
  OUTPUT_VARIABLE _required_no_vulkan_output
  ERROR_VARIABLE _required_no_vulkan_error)
if(_required_no_vulkan_result EQUAL 0)
  message(FATAL_ERROR "required Graphics unexpectedly configured without Vulkan")
endif()
string(TOLOWER "${_required_no_vulkan_output}\n${_required_no_vulkan_error}"
  _required_no_vulkan_diagnostics)
if(NOT _required_no_vulkan_diagnostics MATCHES "vulkan")
  message(FATAL_ERROR
    "required Graphics failure did not identify Vulkan\n"
    "${_required_no_vulkan_output}\n${_required_no_vulkan_error}")
endif()

# A version-only or loader-only Vulkan package must not satisfy the installed
# Graphics header contract.  The target intentionally exposes decoy files that
# do not contain the declarations used by the isolated compile probe.
set(_decoy_vulkan_root "${_terreate_package_root}/decoy-vulkan")
set(_decoy_vulkan_include "${_decoy_vulkan_root}/include")
set(_decoy_vulkan_config "${_decoy_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_decoy_vulkan_include}/vulkan"
  "${_decoy_vulkan_config}")
file(WRITE "${_decoy_vulkan_include}/vulkan/vulkan_core.h"
  "#define VK_API_VERSION_1_3 4202496U\n")
file(WRITE "${_decoy_vulkan_include}/vulkan/vulkan.hpp"
  "#pragma once\n")
file(WRITE "${_decoy_vulkan_include}/vulkan/vulkan_raii.hpp"
  "#pragma once\n")
file(WRITE "${_decoy_vulkan_config}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_decoy_vulkan_include}\")\n")
file(WRITE "${_decoy_vulkan_config}/VulkanConfigVersion.cmake"
  "set(PACKAGE_VERSION \"1.3.0\")\n"
  "if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)\n"
  "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
  "endif()\n")

set(_decoy_consumer "${_terreate_package_root}/all_components/decoy-consumer")
set(_decoy_build "${_terreate_package_root}/all_components/decoy-consumer-build")
file(MAKE_DIRECTORY "${_decoy_consumer}")
file(WRITE "${_decoy_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateDecoyVulkan LANGUAGES CXX)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(CMAKE_FIND_PACKAGE_NO_MODULE TRUE)
find_package(Terreate REQUIRED COMPONENTS Graphics)
]=])
set(_decoy_command
  "${CMAKE_COMMAND}" -S "${_decoy_consumer}" -B "${_decoy_build}"
  "-DCMAKE_PREFIX_PATH:PATH=${_all_prefix}"
  "-DVulkan_DIR:PATH=${_decoy_vulkan_config}")
terreate_append_generator_and_compiler(_decoy_command)
execute_process(
  COMMAND ${_decoy_command}
  RESULT_VARIABLE _decoy_result
  OUTPUT_VARIABLE _decoy_output
  ERROR_VARIABLE _decoy_error)
if(_decoy_result EQUAL 0)
  message(FATAL_ERROR "incomplete Vulkan package unexpectedly enabled Graphics")
endif()
string(TOLOWER "${_decoy_output}\n${_decoy_error}" _decoy_diagnostics)
if(NOT _decoy_diagnostics MATCHES "compile|declaration|vulkan")
  message(FATAL_ERROR
    "incomplete Vulkan rejection was not actionable\n"
    "${_decoy_output}\n${_decoy_error}")
endif()
