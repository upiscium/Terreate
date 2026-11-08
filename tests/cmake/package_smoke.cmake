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
            "^include/terreate/(core/(diagnostics|error|result)|graphics/(allocator|buffer|command_pool|command_buffer|transfer_commands|upload_batch|readback_batch|spirv_binary|shader_module|pipeline_layout|compute_pipeline|completion|submission|retirement|image|image_view|sampler|device|instance|physical_device))\\.hpp$")
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
  if(graphics)
    foreach(_resource IN ITEMS buffer command_pool command_buffer transfer_commands upload_batch readback_batch spirv_binary shader_module pipeline_layout compute_pipeline submission retirement image image_view sampler)
      if(NOT EXISTS "${_prefix}/include/terreate/graphics/${_resource}.hpp")
        message(FATAL_ERROR "${name} install is missing public Graphics ${_resource} header")
      endif()
    endforeach()
    # The sibling #257 Completion API is optional in the standalone #263
    # checkout, but MUST be present in an installed combined Wave A package.
    if(EXISTS "${TERREATE_SOURCE_DIR}/modules/graphics/include/terreate/graphics/completion.hpp")
      if(NOT EXISTS "${_prefix}/include/terreate/graphics/completion.hpp")
        message(FATAL_ERROR "${name} install is missing public Graphics completion header")
      endif()
    endif()
  endif()
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
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/transfer_commands.hpp>
#include <terreate/graphics/upload_batch.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/retirement.hpp>
// Optional sibling Wave A #257 public Completion header, present in combined verification.
#if __has_include(<terreate/graphics/completion.hpp>)
#include <terreate/graphics/completion.hpp>
static_assert(std::is_trivially_copyable_v<terreate::graphics::Completion>);
#endif
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/sampler.hpp>
#include <terreate/graphics/spirv_binary.hpp>
#include <terreate/graphics/shader_module.hpp>
#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/compute_pipeline.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<terreate::graphics::Allocator>);
static_assert(!std::is_copy_constructible_v<terreate::graphics::Allocator>);
static_assert(std::is_move_constructible_v<terreate::graphics::Allocator>);
static_assert(!std::is_copy_constructible_v<terreate::graphics::Buffer>);
static_assert(!std::is_copy_assignable_v<terreate::graphics::Buffer>);
static_assert(std::is_nothrow_move_constructible_v<terreate::graphics::Buffer>);
static_assert(std::is_nothrow_move_assignable_v<terreate::graphics::Buffer>);
static_assert(!std::is_default_constructible_v<terreate::graphics::Image>);
static_assert(!std::is_copy_constructible_v<terreate::graphics::Image>);
static_assert(!std::is_copy_assignable_v<terreate::graphics::Image>);
static_assert(std::is_nothrow_move_constructible_v<terreate::graphics::Image>);
static_assert(std::is_nothrow_move_assignable_v<terreate::graphics::Image>);
static_assert(!std::is_default_constructible_v<terreate::graphics::ImageView>);
static_assert(!std::is_copy_constructible_v<terreate::graphics::ImageView>);
static_assert(!std::is_copy_assignable_v<terreate::graphics::ImageView>);
static_assert(std::is_nothrow_move_constructible_v<terreate::graphics::ImageView>);
static_assert(std::is_nothrow_move_assignable_v<terreate::graphics::ImageView>);
static_assert(!std::is_default_constructible_v<terreate::graphics::Sampler>);
static_assert(!std::is_copy_constructible_v<terreate::graphics::Sampler>);
static_assert(!std::is_copy_assignable_v<terreate::graphics::Sampler>);
static_assert(std::is_nothrow_move_constructible_v<terreate::graphics::Sampler>);
static_assert(std::is_nothrow_move_assignable_v<terreate::graphics::Sampler>);
static_assert(!std::is_default_constructible_v<terreate::graphics::Device>);
static_assert(!std::is_copy_constructible_v<terreate::graphics::Device>);
static_assert(std::is_move_constructible_v<terreate::graphics::Device>);
static_assert(std::is_copy_constructible_v<terreate::graphics::Queue>);
static_assert(std::is_trivially_copyable_v<terreate::graphics::Queue>);
static_assert(!std::is_constructible_v<terreate::graphics::Queue,
                                      vk::Queue, const void *>);
static_assert(std::is_copy_constructible_v<terreate::graphics::PhysicalDevice>);
static_assert(std::is_trivially_copyable_v<terreate::graphics::PhysicalDevice>);
static_assert(sizeof(terreate::graphics::PhysicalDevice) <= 2 * sizeof(void *));
static_assert(!std::is_constructible_v<terreate::graphics::PhysicalDevice,
                                      vk::PhysicalDevice, const void *>);
static_assert(!std::is_convertible_v<terreate::graphics::PhysicalDevice,
                                     vk::PhysicalDevice>);
static_assert(std::is_copy_constructible_v<terreate::graphics::PhysicalDeviceCandidate>);
static_assert(std::is_move_constructible_v<terreate::graphics::PhysicalDeviceCandidate>);
static_assert(!std::is_copy_assignable_v<terreate::graphics::PhysicalDeviceCandidate>);
static_assert(!std::is_move_assignable_v<terreate::graphics::PhysicalDeviceCandidate>);
static_assert(!std::is_default_constructible_v<terreate::graphics::PhysicalDeviceCandidate>);
static_assert(!std::is_constructible_v<terreate::graphics::PhysicalDeviceCandidate,
                                       terreate::graphics::PhysicalDevice,
                                       terreate::graphics::PhysicalDeviceCapabilities,
                                       std::size_t>);
static_assert(!std::is_constructible_v<terreate::graphics::PhysicalDeviceCandidate,
                                       vk::PhysicalDevice,
                                       terreate::graphics::PhysicalDeviceCapabilities,
                                       std::size_t>);
static_assert(
    std::is_same_v<decltype(std::declval<terreate::graphics::PhysicalDeviceCandidate &>().device()),
                   const terreate::graphics::PhysicalDevice &>);
static_assert(std::is_same_v<
              decltype(std::declval<terreate::graphics::PhysicalDeviceCandidate &>().capabilities()),
              const terreate::graphics::PhysicalDeviceCapabilities &>);
static_assert(!std::is_assignable_v<
              decltype(std::declval<terreate::graphics::PhysicalDeviceCandidate &>().device()),
              terreate::graphics::PhysicalDevice>);
static_assert(!std::is_assignable_v<
              decltype(std::declval<terreate::graphics::PhysicalDeviceCandidate &>().capabilities()),
              terreate::graphics::PhysicalDeviceCapabilities>);

int main() {
  // Verify public, installed ShaderModule and CPU-only SPIR-V symbols link
  // without a runtime shader compiler or reflection backend.
  using terreate::graphics::SpirvBinary;
  using terreate::graphics::ShaderModule;
  static_assert(!std::is_default_constructible_v<SpirvBinary>);
  static_assert(std::is_copy_constructible_v<SpirvBinary>);
  static_assert(!std::is_copy_constructible_v<ShaderModule>);
  static_assert(std::is_nothrow_move_constructible_v<ShaderModule>);
  const std::array<std::uint32_t, 5> spirv_words{
      0x07230203U, 0x00010000U, 0, 1, 0};
  auto spirv = SpirvBinary::fromWords(spirv_words);
  if (!spirv || !spirv->valid() || spirv->byteSize() != 20) return 40;
  auto spirv_bytes = spirv->toBytes();
  if (!spirv_bytes || !SpirvBinary::fromBytes(*spirv_bytes)) return 41;
  const auto create_module = &terreate::graphics::createShaderModule;
  if (!create_module ||
      terreate::graphics::make_error_code(
          terreate::graphics::ShaderModuleError::invalid_device).value() == 0) return 42;

  using terreate::graphics::PipelineLayout;
  using terreate::graphics::ComputePipeline;
  static_assert(!std::is_default_constructible_v<PipelineLayout>);
  static_assert(!std::is_copy_constructible_v<PipelineLayout>);
  static_assert(!std::is_copy_assignable_v<PipelineLayout>);
  static_assert(std::is_nothrow_move_constructible_v<PipelineLayout>);
  static_assert(std::is_nothrow_move_assignable_v<PipelineLayout>);
  static_assert(!std::is_default_constructible_v<ComputePipeline>);
  static_assert(!std::is_copy_constructible_v<ComputePipeline>);
  static_assert(!std::is_copy_assignable_v<ComputePipeline>);
  static_assert(std::is_nothrow_move_constructible_v<ComputePipeline>);
  static_assert(std::is_nothrow_move_assignable_v<ComputePipeline>);
  if (!terreate::graphics::validatePipelineLayoutDescription({}, 0) ||
      !terreate::graphics::validateComputePipelineDescription({"main"}) ||
      terreate::graphics::validateComputePipelineDescription({}) ||
      terreate::graphics::make_error_code(
          terreate::graphics::PipelineLayoutError::invalid_description).value() == 0 ||
      terreate::graphics::make_error_code(
          terreate::graphics::ComputePipelineError::invalid_entry_point).value() == 0) return 43;

  // Verify installed upload preparation/recording signatures without GPU work.
  // Phase A public Readback convenience API is usable by installed consumers.
  static_assert(!std::is_copy_constructible_v<terreate::graphics::PreparedReadbackBatch>);
  static_assert(std::is_nothrow_move_constructible_v<terreate::graphics::PreparedReadbackBatch>);
  const auto readback_prepare = &terreate::graphics::prepareReadbackBatch;
  const auto readback_record = &terreate::graphics::recordReadbackBatch;
  const auto readback_copy = &terreate::graphics::tryCopyCompletedReadbackBatch;
  if (!readback_prepare || !readback_record || !readback_copy) return 28;
  // Phase B public Image Readback is compatible with the installed Graphics package.
  static_assert(std::is_standard_layout_v<terreate::graphics::ImageReadbackItem>);
  static_assert(std::is_trivially_copyable_v<terreate::graphics::PreparedImageReadback>);
  static_assert(std::is_trivially_copyable_v<terreate::graphics::CompletedImageReadback>);
  using ReadbackPreparedImages =
      decltype(std::declval<const terreate::graphics::PreparedReadbackBatch &>().images());
  using ReadbackCompletedImages =
      decltype(std::declval<const terreate::graphics::CompletedReadbackBatch &>().images());
  using ImageReadbackCPUBytes =
      decltype(std::declval<const terreate::graphics::CompletedReadbackBatch &>().imageDataFor(0));
  static_assert(std::is_same_v<ReadbackPreparedImages,
                               std::span<const terreate::graphics::PreparedImageReadback>>);
  static_assert(std::is_same_v<ReadbackCompletedImages,
                               std::span<const terreate::graphics::CompletedImageReadback>>);
  static_assert(std::is_same_v<ImageReadbackCPUBytes, std::span<const std::byte>>);
  if (terreate::graphics::make_error_code(
          terreate::graphics::ReadbackError::invalid_image_footprint).value() == 0) return 29;
  const auto upload_prepare = &terreate::graphics::prepareUploadBatch;
  const auto upload_record = &terreate::graphics::recordUploadBatch;
  static_assert(!std::is_copy_constructible_v<terreate::graphics::PreparedUploadBatch>);
  static_assert(std::is_nothrow_move_constructible_v<terreate::graphics::PreparedUploadBatch>);
  if (!upload_prepare || !upload_record ||
      terreate::graphics::make_error_code(
          terreate::graphics::UploadError::invalid_batch).value() == 0) return 26;
  // Phase B public Image upload surface is usable by installed consumers.
  static_assert(std::is_standard_layout_v<terreate::graphics::ImageUploadItem>);
  static_assert(std::is_trivially_copyable_v<terreate::graphics::PreparedImageUpload>);
  using PhaseBImages = decltype(std::declval<const terreate::graphics::PreparedUploadBatch &>().images());
  static_assert(std::is_same_v<PhaseBImages, std::span<const terreate::graphics::PreparedImageUpload>>);
  if (terreate::graphics::make_error_code(
          terreate::graphics::UploadError::invalid_image_footprint).value() == 0) return 27;
  // Verify installed transfer recording API links without command execution.
  const auto transfer_barriers = &terreate::graphics::recordBarriers;
  const auto transfer_copy_buffer = &terreate::graphics::recordCopyBuffer;
  const auto transfer_copy_image = &terreate::graphics::recordCopyImage;
  const auto transfer_to_image = &terreate::graphics::recordCopyBufferToImage;
  const auto transfer_from_image = &terreate::graphics::recordCopyImageToBuffer;
  if (!transfer_barriers || !transfer_copy_buffer || !transfer_copy_image ||
      !transfer_to_image || !transfer_from_image ||
      terreate::graphics::make_error_code(
          terreate::graphics::TransferCommandError::invalid_region).value() == 0) return 25;
  // Exercise an installed, linked submission entry point without native work.
  const auto submit_entry = &terreate::graphics::submit;
  const auto poll_entry = &terreate::graphics::SubmissionReceipt::poll;
  const auto wait_entry = &terreate::graphics::SubmissionReceipt::wait;
  static_assert(!std::is_copy_constructible_v<terreate::graphics::RetirementQueue>);
  static_assert(!std::is_move_constructible_v<terreate::graphics::RetirementQueue>);
  terreate::graphics::RetirementQueue retirement;
  if (!retirement.requireEmpty() || retirement.pendingCount() != 0) return 24;
  static_assert(!std::is_default_constructible_v<terreate::graphics::SubmissionReceipt>);
  if (!submit_entry || !poll_entry || !wait_entry ||
      terreate::graphics::make_error_code(
          terreate::graphics::SubmissionError::invalid_batch).value() == 0) return 23;
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
  if (!candidate.device().valid() || !candidate.device().correlatedWith(*instance)) {
    return 6;
  }
  const auto copied_candidate = candidate;
  if (copied_candidate.enumerationIndex() != candidate.enumerationIndex() ||
      copied_candidate.capabilities().properties.deviceName[0] == '\0') {
    return 7;
  }
  const auto selection = terreate::graphics::selectPhysicalDevice(
      candidate, terreate::graphics::PhysicalDeviceRequirements{});
  if (!selection) {
    return 8;
  }

  std::size_t queue_family_index = 0;
  vk::QueueFlags queue_flags{};
  for (std::size_t index = 0; index < candidate.capabilities().queue_families.size(); ++index) {
    const auto family = candidate.capabilities().queue_families[index];
    if (family.queueCount == 0 || static_cast<VkQueueFlags>(family.queueFlags) == 0) {
      continue;
    }
    queue_family_index = index;
    queue_flags = (static_cast<VkQueueFlags>(family.queueFlags) &
                   static_cast<VkQueueFlags>(vk::QueueFlagBits::eGraphics)) != 0
                      ? vk::QueueFlagBits::eGraphics
                      : ((static_cast<VkQueueFlags>(family.queueFlags) &
                          static_cast<VkQueueFlags>(vk::QueueFlagBits::eCompute)) != 0
                             ? vk::QueueFlagBits::eCompute
                             : vk::QueueFlagBits::eTransfer);
    break;
  }
  if (static_cast<VkQueueFlags>(queue_flags) == 0) {
    return 9;
  }
  terreate::graphics::DeviceDescription device_description;
  device_description.queue_requests.push_back(terreate::graphics::DeviceQueueRequest{
      .caller_id = "package-main",
      .strength = terreate::graphics::RequirementStrength::required,
      .required_flags = queue_flags,
      .allowed_family_indices = {queue_family_index},
      .preferred_family_indices = {queue_family_index},
      .priority = 1.0F,
  });
  const auto device_plan = terreate::graphics::resolveDevice(candidate, device_description);
  if (!device_plan) {
    return 10;
  }
  auto device = terreate::graphics::createDevice(*instance, *device_plan);
  if (!device || !device->valid() || !device->queue("package-main")) {
    return 11;
  }
  auto allocator = terreate::graphics::createAllocator(*instance, candidate, *device);
  if (!allocator || !allocator->valid() ||
      allocator->nativeHandle() == VK_NULL_HANDLE ||
      allocator->configuration() == nullptr ||
      allocator->effectiveFlags() != 0) return 12;
  const auto budgets = allocator->budgets();
  const auto statistics = allocator->statistics();
  terreate::graphics::BufferDescription buffer_description;
  buffer_description.size = 256;
  buffer_description.usage = vk::BufferUsageFlagBits::eTransferSrc;
  auto buffer = terreate::graphics::createBuffer(*allocator, buffer_description);
  if (!buffer || !buffer->valid() || buffer->nativeHandle() == vk::Buffer{}) return 14;
  terreate::graphics::ImageDescription image_description;
  image_description.format = vk::Format::eR8G8B8A8Unorm;
  image_description.extent = vk::Extent3D{4, 4, 1};
  image_description.usage = vk::ImageUsageFlagBits::eSampled;
  auto image = terreate::graphics::createImage(*allocator, image_description);
  if (!image || !image->valid() || image->nativeHandle() == vk::Image{} ||
      !image->configuration() || !image->allocationInfo()) return 15;
  terreate::graphics::ImageViewDescription view_description;
  view_description.format = image_description.format;
  view_description.subresource_range = vk::ImageSubresourceRange{
      vk::ImageAspectFlagBits::eColor, 0, VK_REMAINING_MIP_LEVELS,
      0, VK_REMAINING_ARRAY_LAYERS};
  auto view = terreate::graphics::createImageView(*device, *image, view_description);
  if (!view || !view->valid() || view->nativeImageView() == VK_NULL_HANDLE ||
      !view->correlatedWith(*image) || !view->correlatedWith(*device) ||
      !view->configuration() || view->configuration()->effective.subresource_range.levelCount != 1)
    return 16;
  auto sampler = terreate::graphics::createSampler(*device);
  if (!sampler || !sampler->valid() || sampler->nativeSampler() == VK_NULL_HANDLE ||
      !sampler->configuration() || !sampler->correlatedWith(*device)) return 17;
  auto moved_view = std::move(*view);
  auto moved_sampler = std::move(*sampler);
  if (view->valid() || view->configuration() || sampler->valid() || sampler->configuration() ||
      !moved_view.valid() || !moved_sampler.valid()) return 18;

  // Exercise installed pipeline factories and observers, not just header
  // declarations or optimizable function-address checks. The fixture is the
  // same offline-compiled SPIR-V 1.0 compute main used by the native tests.
  const std::array<std::uint32_t, 70> compute_words{
      0x07230203, 0x00010000, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
      0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
      0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
      0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
      0x6e69616d, 0x00000000, 0x00040047, 0x00000009, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
      0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
      0x00000007, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001, 0x0006002c,
      0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00050036, 0x00000002, 0x00000004,
      0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
  };
  auto compute_binary = SpirvBinary::fromWords(compute_words);
  if (!compute_binary) return 44;
  auto pipeline_layout = terreate::graphics::createPipelineLayout(
      *device, {{vk::PushConstantRange{vk::ShaderStageFlagBits::eCompute, 0, 16}}});
  auto compute_shader = terreate::graphics::createShaderModule(*device, *compute_binary);
  if (!pipeline_layout || !compute_shader || !pipeline_layout->configuration() ||
      pipeline_layout->nativePipelineLayout() == VK_NULL_HANDLE ||
      pipeline_layout->nativeHandle() == vk::PipelineLayout{} ||
      pipeline_layout->nativeDevice() != device->nativeHandle() ||
      pipeline_layout->nativeVkDevice() != static_cast<VkDevice>(device->nativeHandle()) ||
      !pipeline_layout->correlatedWith(*device)) return 45;
  auto compute_pipeline = terreate::graphics::createComputePipeline(
      *device, *compute_shader, *pipeline_layout, {"main"});
  if (!compute_pipeline || !compute_pipeline->configuration() ||
      compute_pipeline->nativePipeline() == VK_NULL_HANDLE ||
      compute_pipeline->nativeHandle() == vk::Pipeline{} ||
      compute_pipeline->nativeDevice() != device->nativeHandle() ||
      compute_pipeline->nativeVkDevice() != static_cast<VkDevice>(device->nativeHandle()) ||
      compute_pipeline->nativeLayout() != pipeline_layout->nativeHandle() ||
      !compute_pipeline->correlatedWith(*device) ||
      !compute_pipeline->correlatedWith(*pipeline_layout) ||
      compute_pipeline->configuration()->requested.entry_point != "main" ||
      compute_pipeline->configuration()->effective.entry_point != "main" ||
      compute_pipeline->configuration()->stage != vk::ShaderStageFlagBits::eCompute) return 46;
  auto moved_pipeline = std::move(*compute_pipeline);
  if (compute_pipeline->valid() || compute_pipeline->configuration() ||
      !moved_pipeline.valid()) return 47;
  // Pipeline dies before its layout; no compute work is submitted.

  // The installed Graphics archive also exposes the explicit CommandPool /
  // CommandBuffer API. The application controls recording and no submit occurs.
  terreate::graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue_family_index);
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto command_pool = terreate::graphics::createCommandPool(*device, pool_description);
  if (!command_pool || !command_pool->valid() ||
      command_pool->nativeHandle() == vk::CommandPool{} ||
      !command_pool->effective() ||
      command_pool->effective()->queue_family_index != pool_description.queue_family_index)
    return 19;
  auto command_buffer = command_pool->allocatePrimary();
  if (!command_buffer || !command_buffer->valid() ||
      command_buffer->nativeHandle() == vk::CommandBuffer{} ||
      command_buffer->state() != terreate::graphics::CommandBufferState::initial)
    return 20;
  if (!command_buffer->begin() || !command_buffer->end() ||
      command_buffer->state() != terreate::graphics::CommandBufferState::executable)
    return 21;
  if (!command_buffer->reset() ||
      command_buffer->state() != terreate::graphics::CommandBufferState::initial)
    return 22;
  // No GPU work is submitted. View dies before Image; resources precede Allocator/Device/Instance.
  // CPU ownership does not stand in for application-owned GPU completion.
  return budgets && statistics && device->plan() != nullptr &&
         device->queues().size() == 1 ? 0 : 13;
}
]=])
terreate_build_consumer(all_components_runtime "${_all_prefix}" Graphics
  "${_graphics_consumer_source}")

set(_candidate_assignment_source [=[
#include <terreate/graphics/physical_device.hpp>

void reject_candidate_assignment(terreate::graphics::PhysicalDeviceCandidate &target,
                                 const terreate::graphics::PhysicalDeviceCandidate &source) {
  target = source;
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(candidate_assignment "${_candidate_assignment_source}"
  "deleted|implicitly-deleted|read-only|const")

set(_candidate_construction_source [=[
#include <terreate/graphics/physical_device.hpp>

terreate::graphics::PhysicalDeviceCandidate reject_candidate_construction() {
  return {terreate::graphics::PhysicalDevice{},
          terreate::graphics::PhysicalDeviceCapabilities{}, 0};
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(candidate_construction "${_candidate_construction_source}"
  "private|inaccessible|no matching|constructible")

set(_candidate_device_splice_source [=[
#include <terreate/graphics/physical_device.hpp>

void reject_device_splice(terreate::graphics::PhysicalDeviceCandidate &candidate,
                          terreate::graphics::PhysicalDevice replacement) {
  candidate.device() = replacement;
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(candidate_device_splice "${_candidate_device_splice_source}"
  "read-only|const|discard|assignment")

set(_candidate_capability_splice_source [=[
#include <terreate/graphics/physical_device.hpp>

void reject_capability_splice(terreate::graphics::PhysicalDeviceCandidate &candidate,
                              terreate::graphics::PhysicalDeviceCapabilities replacement) {
  candidate.capabilities() = replacement;
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(candidate_capability_splice "${_candidate_capability_splice_source}"
  "read-only|const|discard|assignment")

set(_candidate_capability_member_source [=[
#include <terreate/graphics/physical_device.hpp>

void reject_capability_member_mutation(
    terreate::graphics::PhysicalDeviceCandidate &candidate) {
  candidate.capabilities().properties.deviceID = 0;
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(candidate_capability_member_mutation
  "${_candidate_capability_member_source}" "read-only|const|discard|assignment")

set(_inventory_construction_source [=[
#include <terreate/graphics/physical_device.hpp>

terreate::graphics::PhysicalDeviceInventory reject_inventory_construction() {
  return terreate::graphics::PhysicalDeviceInventory{
      std::vector<terreate::graphics::PhysicalDeviceCandidate>{}};
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(inventory_construction "${_inventory_construction_source}"
  "private|inaccessible|no matching|constructible")

set(_inventory_mutation_source [=[
#include <terreate/graphics/physical_device.hpp>

void reject_inventory_mutation(terreate::graphics::PhysicalDeviceInventory &inventory,
                               const terreate::graphics::PhysicalDeviceCandidate &candidate) {
  inventory[0] = candidate;
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(inventory_mutation "${_inventory_mutation_source}"
  "read-only|const|discard|assignment|deleted")

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

set(_physical_device_private_member_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

int main() {
  const terreate::graphics::PhysicalDevice device;
  return device.parent_identity_ == nullptr ? 0 : 1;
}
]=])
terreate_expect_compile_failure(physical_device_private_member
  "${_physical_device_private_member_source}"
  "no member|private|inaccessible")

set(_physical_device_mutation_source [=[
#include <terreate/graphics/physical_device.hpp>

void reject_physical_device_mutation(terreate::graphics::PhysicalDevice &device) {
  device.native_handle_ = {};
}

int main() { return 0; }
]=])
terreate_expect_compile_failure(physical_device_mutation "${_physical_device_mutation_source}"
  "no member|private|inaccessible|read-only|const")

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
   NOT "${_graphics_links}" MATCHES "GPUOpen::VulkanMemoryAllocator" OR
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

# A partial Vulkan package must not satisfy the installed Graphics header
# contract, even when it also publishes a complete transitive Headers target.
# The partial directory is intentionally first in the effective target include
# order, so a probe that checks each directory in isolation would accept the
# package and defer the failure to a consumer compile.
set(_decoy_vulkan_root "${_terreate_package_root}/decoy-vulkan")
set(_decoy_vulkan_include "${_decoy_vulkan_root}/include")
set(_decoy_vulkan_config "${_decoy_vulkan_root}/lib/cmake/Vulkan")
file(MAKE_DIRECTORY "${_decoy_vulkan_include}/vulkan"
  "${_decoy_vulkan_config}")
file(WRITE "${_decoy_vulkan_include}/vulkan/vulkan_core.h"
  "#pragma once\n"
  "#define VK_API_VERSION_1_3 4202496U\n")
file(WRITE "${_decoy_vulkan_include}/vulkan/vulkan.hpp"
  "#pragma once\n"
  "// Deliberately partial Hpp declarations.\n")
file(WRITE "${_decoy_vulkan_include}/vulkan/vulkan_raii.hpp"
  "#pragma once\n"
  "// Deliberately partial RAII declarations; Device is absent.\n")
file(WRITE "${_decoy_vulkan_config}/VulkanConfig.cmake"
  "set(Vulkan_VERSION 1.3.0)\n"
  "set(Vulkan_FOUND TRUE)\n"
  "add_library(Vulkan::Headers INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Headers PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"$ENV{VULKAN_HEADERS_INCLUDE}\")\n"
  "add_library(Vulkan::Vulkan INTERFACE IMPORTED)\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_INCLUDE_DIRECTORIES\n"
  "  \"${_decoy_vulkan_include}\")\n"
  "set_property(TARGET Vulkan::Vulkan PROPERTY INTERFACE_LINK_LIBRARIES\n"
  "  Vulkan::Headers)\n")
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
