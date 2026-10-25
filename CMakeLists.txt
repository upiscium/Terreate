cmake_minimum_required(VERSION 3.28)
project(Terreate VERSION 0.1.0 LANGUAGES CXX)

# The repository build is intentionally GCC-only.  clangd/clang-tidy may still
# consume the generated compilation database, but they are not the build
# compiler.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "Terreate must be built with GCC/g++; got ${CMAKE_CXX_COMPILER_ID} "
    "(${CMAKE_CXX_COMPILER})")
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Export the compilation database for clangd and clang-tidy" FORCE)
endif()

# BUILD_* is the sole public option spelling.  In particular, a module never
# turns Core on implicitly.
option(TERREATE_BUILD_CORE
  "Build the dependency-free Terreate Core component" ON)
option(TERREATE_BUILD_PLATFORM
  "Build the independently selectable Terreate Platform component" ON)
option(TERREATE_BUILD_GRAPHICS
  "Build the independently selectable Terreate Graphics component" ON)

set(TERREATE_MINIMUM_VULKAN_API_VERSION "1.3")

# Keep the explicit Hpp root available when Terreate is added as a source
# subdirectory.  The Graphics target publishes this same value through its
# BUILD_INTERFACE; the top-level path is validated below before any target is
# created, while the source-subdirectory path is validated by Graphics' probe.
if(TERREATE_BUILD_GRAPHICS AND
   DEFINED ENV{VULKAN_HEADERS_INCLUDE} AND
   NOT "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  set(PROJECT_VULKAN_HPP_INCLUDE_DIR "$ENV{VULKAN_HEADERS_INCLUDE}")
endif()

# Check the header-only part of Vulkan before any Graphics source is compiled.
# FindVulkan is intentionally not used for this check: a loader-only package
# can provide Vulkan::Vulkan while omitting Vulkan-Hpp entirely.
function(terreate_vulkan_headers_legacy include_directory result_variable)
  set(_satisfies FALSE)
  set(_has_version_header FALSE)
  set(_has_required_api FALSE)

  if(EXISTS "${include_directory}/vulkan/vulkan_raii.hpp")
    foreach(_vulkan_header IN ITEMS
        "${include_directory}/vulkan/vulkan_core.h"
        "${include_directory}/vulkan/vulkan.hpp"
        "${include_directory}/vulkan/vulkan_raii.hpp")
      if(EXISTS "${_vulkan_header}")
        if(NOT "${_vulkan_header}" MATCHES "/vulkan_raii\\.hpp$")
          set(_has_version_header TRUE)
        endif()
        file(STRINGS "${_vulkan_header}" _vulkan_version_lines
          REGEX "VK_API_VERSION_1_3|VK_HEADER_VERSION_COMPLETE")
        foreach(_vulkan_version_line IN LISTS _vulkan_version_lines)
          if(_vulkan_version_line MATCHES
             "#[ \t]*define[ \t]+VK_API_VERSION_1_3([ \t]|$)")
            set(_has_required_api TRUE)
          elseif(_vulkan_version_line MATCHES
                 "VK_MAKE_API_VERSION\\([^,]*,[ \t]*([0-9]+),[ \t]*([0-9]+),")
            if("${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" VERSION_GREATER_EQUAL
               "${TERREATE_MINIMUM_VULKAN_API_VERSION}")
              set(_has_required_api TRUE)
            endif()
          endif()
        endforeach()
      endif()
    endforeach()

    # A config-package fixture may expose the RAII header without shipping the
    # complete Vulkan C header set.  In that case the package's declared
    # version is the only trustworthy version evidence available.
    if(NOT _has_version_header AND DEFINED Vulkan_VERSION AND
       NOT "${Vulkan_VERSION}" STREQUAL "" AND
       NOT "${Vulkan_VERSION}" MATCHES "-NOTFOUND$" AND
       NOT "${Vulkan_VERSION}" VERSION_LESS
           "${TERREATE_MINIMUM_VULKAN_API_VERSION}")
      set(_has_required_api TRUE)
    endif()
  endif()

  if(_has_required_api)
    set(_satisfies TRUE)
  endif()
  set(${result_variable} "${_satisfies}" PARENT_SCOPE)
endfunction()

function(terreate_vulkan_headers_complete include_directory result_variable)
  set(_satisfies FALSE)
  set(_core_header "${include_directory}/vulkan/vulkan_core.h")
  set(_hpp_header "${include_directory}/vulkan/vulkan.hpp")
  set(_raii_header "${include_directory}/vulkan/vulkan_raii.hpp")

  if(EXISTS "${_core_header}" AND EXISTS "${_hpp_header}" AND
     EXISTS "${_raii_header}")
    file(READ "${_core_header}" _core_contents)
    file(READ "${_raii_header}" _raii_contents)
    set(_hpp_contents)
    foreach(_hpp_declaration_header IN ITEMS
        "${_hpp_header}"
        "${include_directory}/vulkan/vulkan_enums.hpp")
      if(EXISTS "${_hpp_declaration_header}")
        file(READ "${_hpp_declaration_header}" _hpp_declaration_contents)
        string(APPEND _hpp_contents "${_hpp_declaration_contents}")
      endif()
    endforeach()
    set(_complete TRUE)

    # Graphics uses the common C Vulkan declarations directly in its native
    # adapter. Device Address Binding remains an optional raw extension bit;
    # it is deliberately not part of the public header compile contract.
    foreach(_declaration IN ITEMS
        VK_API_VERSION_1_3
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT
        VkDebugUtilsMessageTypeFlagsEXT)
      string(FIND "${_core_contents}" "${_declaration}" _declaration_position)
      if(_declaration_position EQUAL -1)
        set(_complete FALSE)
      endif()
    endforeach()

    # Vulkan-Hpp and its RAII layer are separate generated surfaces. Merely
    # finding a file with the right name is not sufficient: a loader package
    # or a decoy header can otherwise make package configuration succeed and
    # fail only when the public instance header is compiled.
    foreach(_declaration IN ITEMS
        DebugUtilsMessageTypeFlagBitsEXT
        eGeneral
        eValidation
        ePerformance)
      string(FIND "${_hpp_contents}" "${_declaration}" _declaration_position)
      if(_declaration_position EQUAL -1)
        set(_complete FALSE)
      endif()
    endforeach()
    foreach(_declaration IN ITEMS
        Context
        Instance
        DebugUtilsMessengerEXT)
      string(FIND "${_raii_contents}" "${_declaration}" _declaration_position)
      if(_declaration_position EQUAL -1)
        set(_complete FALSE)
      endif()
    endforeach()

    if(_complete)
      set(_satisfies TRUE)
    endif()
  endif()

  set(${result_variable} "${_satisfies}" PARENT_SCOPE)
endfunction()

function(terreate_require_vulkan_headers include_directory description)
  terreate_vulkan_headers_complete("${include_directory}" _headers_satisfy)
  if(NOT _headers_satisfy)
    message(FATAL_ERROR
      "Terreate Graphics requires complete Vulkan >= "
      "${TERREATE_MINIMUM_VULKAN_API_VERSION} declarations in "
      "${description}/vulkan/vulkan_core.h, "
      "${description}/vulkan/vulkan.hpp, and "
      "${description}/vulkan/vulkan_raii.hpp, including the required "
      "Debug Utils declarations; refusing a loader-only, "
      "partial, or decoy header package at ${include_directory}")
  endif()
endfunction()

# Validate the headers that a source-subdirectory consumer will actually see.
# Inspecting each directory in isolation is insufficient: an earlier partial
# Vulkan directory can shadow a later complete transitive header directory.
# Keep this probe local to configure time and remove all generated state before
# returning so it cannot become part of a consumer's build or install output.
function(terreate_probe_vulkan_headers include_directories result_variable)
  set(_probe_root
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/TerreateVulkanHeadersProbe")
  set(_probe_source_dir "${_probe_root}/source")
  set(_probe_binary_dir "${_probe_root}/build")
  file(REMOVE_RECURSE "${_probe_root}")
  file(MAKE_DIRECTORY "${_probe_source_dir}")

  file(WRITE "${_probe_source_dir}/probe.cpp" [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/device.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

static_assert(VK_API_VERSION_1_3 != 0);
static_assert(sizeof(vk::raii::Context) > 0);
static_assert(sizeof(vk::raii::Instance) > 0);
static_assert(sizeof(vk::raii::DebugUtilsMessengerEXT) > 0);
static_assert(sizeof(vk::raii::PhysicalDevice) > 0);
static_assert(sizeof(vk::raii::Device) > 0);

int main() { return 0; }
]=])
  file(WRITE "${_probe_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(TerreateVulkanHeadersProbe LANGUAGES CXX)
# Keep the explicitly supplied probe paths in target include order even when a
# toolchain reports one of them as an implicit compiler directory.
set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
add_library(terreate_vulkan_headers_probe STATIC probe.cpp)
set_target_properties(terreate_vulkan_headers_probe PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  CXX_SCAN_FOR_MODULES OFF)
]=])

  set(_probe_sentinel_dir "${_probe_root}/ambient-header-sentinel")
  file(MAKE_DIRECTORY "${_probe_sentinel_dir}/vulkan")
  foreach(_sentinel_header IN ITEMS
      vulkan_core.h vulkan.hpp vulkan_raii.hpp)
    file(WRITE "${_probe_sentinel_dir}/vulkan/${_sentinel_header}"
      "#error Terreate Vulkan header probe reached an ambient header path\n")
  endforeach()

  # Use CMake's portable SYSTEM include abstraction rather than emitting a
  # compiler-specific -isystem option.  Keep the target-provided directories
  # in their effective order, then place the sentinel before implicit compiler
  # directories so an ambient Vulkan header cannot satisfy the probe.
  file(APPEND "${_probe_source_dir}/CMakeLists.txt"
    "target_include_directories(terreate_vulkan_headers_probe SYSTEM PRIVATE\n")
  foreach(_include_directory IN LISTS include_directories)
    if(NOT "${_include_directory}" STREQUAL "")
      file(APPEND "${_probe_source_dir}/CMakeLists.txt"
        "  \"${_include_directory}\"\n")
    endif()
  endforeach()
  file(APPEND "${_probe_source_dir}/CMakeLists.txt"
    "  \"${_probe_sentinel_dir}\"\n")
  foreach(_implicit_include_directory IN LISTS
      CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    if(NOT "${_implicit_include_directory}" MATCHES "[Vv]ulkan")
      file(APPEND "${_probe_source_dir}/CMakeLists.txt"
        "  \"${_implicit_include_directory}\"\n")
    endif()
  endforeach()
  file(APPEND "${_probe_source_dir}/CMakeLists.txt" ")\n")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
     CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    file(APPEND "${_probe_source_dir}/CMakeLists.txt"
      "target_compile_options(terreate_vulkan_headers_probe PRIVATE -nostdinc)\n")
  endif()

  try_compile(_probe_succeeded
    "${_probe_binary_dir}"
    "${_probe_source_dir}"
    TerreateVulkanHeadersProbe
    NO_CACHE
    OUTPUT_VARIABLE _probe_output)
  file(REMOVE_RECURSE "${_probe_root}")
  set(${result_variable} "${_probe_succeeded}" PARENT_SCOPE)
endfunction()

if(PROJECT_IS_TOP_LEVEL)
  # CMake deliberately omits compiler-provided include directories from a
  # compilation command.  clang-tidy uses those commands, however, and must be
  # able to see the GCC/Nix standard library headers without a hard-coded store
  # path or an invocation-specific compiler argument.  CMake also filters
  # implicit directories back out of target_include_directories(), so publish
  # them as compiler options instead.  The selected compiler determines these
  # paths during CMake's toolchain detection.
  add_library(project_compiler_context INTERFACE)
  set(PROJECT_COMPILER_INCLUDE_OPTIONS)
  foreach(project_include_directory IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    list(APPEND PROJECT_COMPILER_INCLUDE_OPTIONS
      "SHELL:-isystem \"${project_include_directory}\"")
  endforeach()
  target_compile_options(project_compiler_context INTERFACE
    ${PROJECT_COMPILER_INCLUDE_OPTIONS})

  if(TERREATE_BUILD_GRAPHICS)
    # Vulkan-Hpp is a header-only development surface here.  It must come from
    # the pinned project devShell rather than an inherited SDK or host search path.
    # Keep the compiler's dynamically discovered implicit directories above for
    # clang-tidy/compiler context only; they are not Vulkan dependency discovery.
    if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
       "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
      message(FATAL_ERROR
        "Terreate requires the project devShell contract: "
        "VULKAN_HEADERS_INCLUDE must be set before configuring Graphics. "
        "Run `nix develop` and configure again.")
    endif()

    set(PROJECT_VULKAN_HPP_INCLUDE_DIR "$ENV{VULKAN_HEADERS_INCLUDE}")
    terreate_require_vulkan_headers("${PROJECT_VULKAN_HPP_INCLUDE_DIR}"
      "VULKAN_HEADERS_INCLUDE")
    message(STATUS "Vulkan-Hpp include directory: ${PROJECT_VULKAN_HPP_INCLUDE_DIR}")

    add_library(project_vulkan_headers INTERFACE)
    target_include_directories(project_vulkan_headers SYSTEM INTERFACE
      "${PROJECT_VULKAN_HPP_INCLUDE_DIR}")
  endif()

  # Keep the editor entrypoint at the repository root while retaining the
  # canonical Ninja build tree.  The generated symlink is ignored by Git.  Do
  # not replace an existing entrypoint: a regular file or a link to another
  # database must not silently change the commands consumed by tooling.
  set(PROJECT_COMPILE_COMMANDS_ENTRYPOINT
    "${PROJECT_SOURCE_DIR}/compile_commands.json")
  set(PROJECT_COMPILE_COMMANDS_TARGET
    ".build/default/compile_commands.json")
  if(EXISTS "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT}" OR
     IS_SYMLINK "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT}")
    if(NOT IS_SYMLINK "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT}")
      message(FATAL_ERROR
        "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT} must be a symlink to "
        "${PROJECT_COMPILE_COMMANDS_TARGET}; refusing to use a conflicting "
        "regular entry")
    endif()
    file(READ_SYMLINK "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT}"
      PROJECT_COMPILE_COMMANDS_LINK_TARGET)
    if(NOT PROJECT_COMPILE_COMMANDS_LINK_TARGET STREQUAL
       PROJECT_COMPILE_COMMANDS_TARGET)
      message(FATAL_ERROR
        "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT} points to "
        "${PROJECT_COMPILE_COMMANDS_LINK_TARGET}, expected "
        "${PROJECT_COMPILE_COMMANDS_TARGET}")
    endif()
  else()
    file(CREATE_LINK "${PROJECT_COMPILE_COMMANDS_TARGET}"
      "${PROJECT_COMPILE_COMMANDS_ENTRYPOINT}" SYMBOLIC)
  endif()
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(TERREATE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Terreate"
  CACHE PATH "Installation directory for Terreate's CMake package")

if((TERREATE_BUILD_PLATFORM OR TERREATE_BUILD_GRAPHICS) AND
   NOT TERREATE_BUILD_CORE)
  message(FATAL_ERROR
    "Terreate Platform and Graphics require Terreate Core; "
    "set TERREATE_BUILD_CORE=ON instead of enabling Core implicitly")
endif()

# Keep C++ policy on every component target, including when Terreate is added
# as a subdirectory by a consumer.  The private compiler context is applied as
# compile options rather than a link dependency, so it can never leak into an
# installed component's usage requirements.
function(terreate_configure_target target)
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  target_compile_features(${target} PUBLIC cxx_std_23)
  set_property(TARGET ${target} PROPERTY CXX_SCAN_FOR_MODULES OFF)

  if(PROJECT_IS_TOP_LEVEL AND DEFINED PROJECT_COMPILER_INCLUDE_OPTIONS)
    target_compile_options(${target} PRIVATE
      ${PROJECT_COMPILER_INCLUDE_OPTIONS})
  endif()
endfunction()

if(TERREATE_BUILD_CORE)
  add_subdirectory(core)
endif()
if(TERREATE_BUILD_PLATFORM)
  add_subdirectory(modules/platform)
endif()
if(TERREATE_BUILD_GRAPHICS)
  add_subdirectory(modules/graphics)
endif()

if(PROJECT_IS_TOP_LEVEL AND TERREATE_BUILD_CORE)
  add_executable(terreate_app src/main.cpp)
  target_link_libraries(terreate_app PRIVATE Terreate::Core)
  terreate_configure_target(terreate_app)

  enable_testing()
  add_executable(terreate_tests tests/test_smoke.cpp)
  target_link_libraries(terreate_tests PRIVATE Terreate::Core)
  terreate_configure_target(terreate_tests)
  add_test(NAME smoke COMMAND terreate_tests)

  add_executable(terreate_core_result_tests tests/test_core_result.cpp)
  target_link_libraries(terreate_core_result_tests PRIVATE Terreate::Core)
  terreate_configure_target(terreate_core_result_tests)
  add_test(NAME core.error_result COMMAND terreate_core_result_tests)

  add_executable(terreate_core_diagnostics_tests tests/test_core_diagnostics.cpp)
  target_link_libraries(terreate_core_diagnostics_tests PRIVATE Terreate::Core)
  terreate_configure_target(terreate_core_diagnostics_tests)
  add_test(NAME core.diagnostics COMMAND terreate_core_diagnostics_tests)

  if(TERREATE_BUILD_GRAPHICS)
    add_executable(terreate_graphics_diagnostics_tests
      tests/test_graphics_diagnostics.cpp)
    target_link_libraries(terreate_graphics_diagnostics_tests
      PRIVATE Terreate::Graphics)
    target_include_directories(terreate_graphics_diagnostics_tests PRIVATE
      "${PROJECT_SOURCE_DIR}/modules/graphics/src")
    terreate_configure_target(terreate_graphics_diagnostics_tests)
    add_test(NAME graphics.diagnostics COMMAND terreate_graphics_diagnostics_tests)

    add_executable(terreate_graphics_instance_tests
      tests/test_graphics_instance.cpp)
    target_link_libraries(terreate_graphics_instance_tests
      PRIVATE Terreate::Graphics)
    target_include_directories(terreate_graphics_instance_tests PRIVATE
      "${PROJECT_SOURCE_DIR}/modules/graphics/src")
    terreate_configure_target(terreate_graphics_instance_tests)
    add_test(NAME graphics.instance COMMAND terreate_graphics_instance_tests)
    set_tests_properties(graphics.instance PROPERTIES
      PASS_REGULAR_EXPRESSION
        "graphics\\.instance: headless-instance=PASS")

    add_executable(terreate_graphics_physical_device_tests
      tests/test_graphics_physical_device.cpp)
    target_link_libraries(terreate_graphics_physical_device_tests
      PRIVATE Terreate::Graphics)
    target_include_directories(terreate_graphics_physical_device_tests PRIVATE
      "${PROJECT_SOURCE_DIR}/modules/graphics/src")
    terreate_configure_target(terreate_graphics_physical_device_tests)
    add_test(NAME graphics.physical_device
      COMMAND terreate_graphics_physical_device_tests)
    set_tests_properties(graphics.physical_device PROPERTIES
      PASS_REGULAR_EXPRESSION
        "graphics\\.physical-device: headless-query=PASS")

    add_executable(terreate_graphics_device_tests
      tests/test_graphics_device.cpp)
    target_link_libraries(terreate_graphics_device_tests
      PRIVATE Terreate::Graphics)
    target_include_directories(terreate_graphics_device_tests PRIVATE
      "${PROJECT_SOURCE_DIR}/modules/graphics/src")
    terreate_configure_target(terreate_graphics_device_tests)
    add_test(NAME graphics.device COMMAND terreate_graphics_device_tests)
    set_tests_properties(graphics.device PROPERTIES
      PASS_REGULAR_EXPRESSION
        "graphics\\.device: headless-device=PASS"
      FAIL_REGULAR_EXPRESSION
        "graphics\\.device: assertion=FAIL")
  endif()

  add_test(
    NAME cmake.component_matrix
    COMMAND ${CMAKE_COMMAND}
      -DTERREATE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DTERREATE_MATRIX_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/component-matrix
      -DTERREATE_CMAKE_GENERATOR=${CMAKE_GENERATOR}
      -DTERREATE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -P ${PROJECT_SOURCE_DIR}/tests/cmake/component_matrix.cmake)
  add_test(
    NAME cmake.package_smoke
    COMMAND ${CMAKE_COMMAND}
      -DTERREATE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DTERREATE_PACKAGE_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-smoke-no-public-headers
      -DTERREATE_CMAKE_GENERATOR=${CMAKE_GENERATOR}
      -DTERREATE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -P ${PROJECT_SOURCE_DIR}/tests/cmake/package_smoke.cmake)
endif()

# Each component registers its own install target in the shared export set.
# The export is intentionally created only when at least one component exists;
# this keeps an explicit all-disabled configuration valid while making a
# package with no usable targets fail clearly at find_package time.
set(_terreate_has_components FALSE)
if(TERREATE_BUILD_CORE OR TERREATE_BUILD_PLATFORM OR TERREATE_BUILD_GRAPHICS)
  set(_terreate_has_components TRUE)
endif()

if(_terreate_has_components)
  if(TERREATE_BUILD_CORE)
    export(TARGETS terreate_core
      FILE "${CMAKE_CURRENT_BINARY_DIR}/TerreateCoreTargets.cmake"
      NAMESPACE Terreate::)
  endif()
  if(TERREATE_BUILD_PLATFORM)
    export(TARGETS terreate_platform
      FILE "${CMAKE_CURRENT_BINARY_DIR}/TerreatePlatformTargets.cmake"
      NAMESPACE Terreate::)
  endif()
  if(TERREATE_BUILD_GRAPHICS)
    export(TARGETS terreate_graphics
      FILE "${CMAKE_CURRENT_BINARY_DIR}/TerreateGraphicsTargets.cmake"
      NAMESPACE Terreate::)
  endif()

  set(_terreate_build_config "${CMAKE_CURRENT_BINARY_DIR}/TerreateConfig.cmake")
  set(_terreate_install_config
    "${CMAKE_CURRENT_BINARY_DIR}/generated/TerreateConfig.cmake")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

  # The build-tree package must probe the headers in their source locations;
  # an installed package instead resolves its public headers below the saved
  # install prefix.  Keep the roots distinct so a build-tree consumer cannot
  # accidentally validate against an installed or ambient header set.
  set(_terreate_build_config_header_roots
    "${PROJECT_SOURCE_DIR}/core/include"
    "${PROJECT_SOURCE_DIR}/modules/graphics/include")
  if(TERREATE_BUILD_GRAPHICS AND
     DEFINED PROJECT_VULKAN_HPP_INCLUDE_DIR AND
     NOT "${PROJECT_VULKAN_HPP_INCLUDE_DIR}" STREQUAL "")
    list(APPEND _terreate_build_config_header_roots
      "${PROJECT_VULKAN_HPP_INCLUDE_DIR}")
  endif()
  set(_terreate_install_config_header_roots
    "${CMAKE_INSTALL_INCLUDEDIR}")

  set(TERREATE_CONFIG_HEADER_ROOTS "${_terreate_build_config_header_roots}")
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/TerreateConfig.cmake.in"
    "${_terreate_build_config}"
    INSTALL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

  set(TERREATE_CONFIG_HEADER_ROOTS "${_terreate_install_config_header_roots}")
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/TerreateConfig.cmake.in"
    "${_terreate_install_config}"
    INSTALL_DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
  unset(TERREATE_CONFIG_HEADER_ROOTS)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TerreateConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

  if(TERREATE_BUILD_CORE)
    install(EXPORT TerreateCoreTargets
      FILE TerreateCoreTargets.cmake
      NAMESPACE Terreate::
      DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
  endif()
  if(TERREATE_BUILD_PLATFORM)
    install(EXPORT TerreatePlatformTargets
      FILE TerreatePlatformTargets.cmake
      NAMESPACE Terreate::
      DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
  endif()
  if(TERREATE_BUILD_GRAPHICS)
    install(EXPORT TerreateGraphicsTargets
      FILE TerreateGraphicsTargets.cmake
      NAMESPACE Terreate::
      DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
  endif()
  install(FILES
    "${_terreate_install_config}"
    "${CMAKE_CURRENT_BINARY_DIR}/TerreateConfigVersion.cmake"
    DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
endif()
