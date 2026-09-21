cmake_minimum_required(VERSION 3.28)
project(terreate LANGUAGES CXX)

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

add_library(project_lib src/lib.cpp)
target_include_directories(project_lib PUBLIC include)

add_executable(project_app src/main.cpp)
target_link_libraries(project_app PRIVATE project_lib)

set(project_targets project_lib project_app)
if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  add_executable(project_tests tests/test_smoke.cpp)
  target_link_libraries(project_tests PRIVATE project_lib)
  add_test(NAME smoke COMMAND project_tests)
  list(APPEND project_targets project_tests)
endif()

foreach(project_target IN LISTS project_targets)
  if(PROJECT_IS_TOP_LEVEL)
    target_link_libraries(${project_target} PRIVATE
      project_compiler_context
      project_vulkan_headers)
  endif()
  set_target_properties(${project_target} PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  set_property(TARGET ${project_target} PROPERTY CXX_SCAN_FOR_MODULES OFF)
endforeach()
