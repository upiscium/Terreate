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

  # Vulkan-Hpp is a header-only development surface here.  It must come from
  # the pinned project devShell rather than an inherited SDK or host search path.
  # Keep the compiler's dynamically discovered implicit directories above for
  # clang-tidy/compiler context only; they are not Vulkan dependency discovery.
  if(NOT DEFINED ENV{VULKAN_HEADERS_INCLUDE} OR
     "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
    message(FATAL_ERROR
      "Terreate requires the project devShell contract: "
      "VULKAN_HEADERS_INCLUDE must be set before configuring. "
      "Run `nix develop` and configure again.")
  endif()

  set(PROJECT_VULKAN_HPP_INCLUDE_DIR "$ENV{VULKAN_HEADERS_INCLUDE}")
  if(NOT EXISTS "${PROJECT_VULKAN_HPP_INCLUDE_DIR}/vulkan/vulkan_raii.hpp")
    message(FATAL_ERROR
      "Terreate requires Vulkan-Hpp at "
      "${PROJECT_VULKAN_HPP_INCLUDE_DIR}/vulkan/vulkan_raii.hpp from "
      "VULKAN_HEADERS_INCLUDE; refusing inherited or default search paths.")
  endif()
  message(STATUS "Vulkan-Hpp include directory: ${PROJECT_VULKAN_HPP_INCLUDE_DIR}")

  add_library(project_vulkan_headers INTERFACE)
  target_include_directories(project_vulkan_headers SYSTEM INTERFACE
    "${PROJECT_VULKAN_HPP_INCLUDE_DIR}")

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

# BUILD_* is the sole public option spelling.  In particular, a module never
# turns Core on implicitly.
option(TERREATE_BUILD_CORE
  "Build the dependency-free Terreate Core component" ON)
option(TERREATE_BUILD_PLATFORM
  "Build the independently selectable Terreate Platform component" ON)
option(TERREATE_BUILD_GRAPHICS
  "Build the independently selectable Terreate Graphics component" ON)

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
  target_link_libraries(terreate_app PRIVATE project_vulkan_headers)

  enable_testing()
  add_executable(terreate_tests tests/test_smoke.cpp)
  target_link_libraries(terreate_tests PRIVATE Terreate::Core)
  terreate_configure_target(terreate_tests)
  target_link_libraries(terreate_tests PRIVATE project_vulkan_headers)
  add_test(NAME smoke COMMAND terreate_tests)

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
  export(EXPORT TerreateTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/TerreateTargets.cmake"
    NAMESPACE Terreate::)

  set(_terreate_build_config "${CMAKE_CURRENT_BINARY_DIR}/TerreateConfig.cmake")
  set(_terreate_install_config
    "${CMAKE_CURRENT_BINARY_DIR}/generated/TerreateConfig.cmake")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/TerreateConfig.cmake.in"
    "${_terreate_build_config}"
    INSTALL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
  configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/TerreateConfig.cmake.in"
    "${_terreate_install_config}"
    INSTALL_DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TerreateConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

  install(EXPORT TerreateTargets
    FILE TerreateTargets.cmake
    NAMESPACE Terreate::
    DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
  install(FILES
    "${_terreate_install_config}"
    "${CMAKE_CURRENT_BINARY_DIR}/TerreateConfigVersion.cmake"
    DESTINATION "${TERREATE_INSTALL_CMAKEDIR}")
endif()
