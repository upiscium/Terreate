if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Graphics requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

find_package(Vulkan REQUIRED)
if(NOT TARGET Vulkan::Vulkan)
  message(FATAL_ERROR
    "Terreate Graphics requires Vulkan::Vulkan from a Vulkan >= 1.3 package")
endif()

if(DEFINED Vulkan_VERSION AND NOT "${Vulkan_VERSION}" STREQUAL "" AND
   NOT "${Vulkan_VERSION}" MATCHES "-NOTFOUND$" AND
   "${Vulkan_VERSION}" VERSION_LESS "${TERREATE_MINIMUM_VULKAN_API_VERSION}")
  message(FATAL_ERROR
    "Terreate Graphics requires Vulkan >= "
    "${TERREATE_MINIMUM_VULKAN_API_VERSION}; found Vulkan ${Vulkan_VERSION}")
endif()

# A Vulkan package may split the loader and header targets.  Walk the complete
# usage closure of Vulkan::Vulkan so a source build rejects a loader/basic-only
# package during configure, before an instance.cpp translation unit is built.
set(_terreate_vulkan_include_dirs)
set(_terreate_vulkan_usage_targets Vulkan::Vulkan)
set(_terreate_vulkan_usage_seen)
while(_terreate_vulkan_usage_targets)
  list(POP_FRONT _terreate_vulkan_usage_targets
    _terreate_vulkan_usage_target)
  if(NOT TARGET "${_terreate_vulkan_usage_target}" OR
     "${_terreate_vulkan_usage_target}" IN_LIST _terreate_vulkan_usage_seen)
    continue()
  endif()
  list(APPEND _terreate_vulkan_usage_seen "${_terreate_vulkan_usage_target}")

  foreach(_terreate_vulkan_include_property IN ITEMS
      INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    get_target_property(_terreate_target_include_dirs
      "${_terreate_vulkan_usage_target}"
      ${_terreate_vulkan_include_property})
    if(_terreate_target_include_dirs AND
       NOT "${_terreate_target_include_dirs}" MATCHES "-NOTFOUND$")
      foreach(_terreate_target_include_dir IN LISTS _terreate_target_include_dirs)
        set(_terreate_stripped_include_dir
          "${_terreate_target_include_dir}")
        string(REGEX REPLACE
          "^\\$<BUILD_INTERFACE:(.*)>$" "\\1"
          _terreate_stripped_include_dir "${_terreate_stripped_include_dir}")
        string(REGEX REPLACE
          "^\\$<INSTALL_INTERFACE:(.*)>$" "\\1"
          _terreate_stripped_include_dir "${_terreate_stripped_include_dir}")
        string(GENEX_STRIP "${_terreate_stripped_include_dir}"
          _terreate_stripped_include_dir)
        if(NOT "${_terreate_stripped_include_dir}" STREQUAL "")
          list(APPEND _terreate_vulkan_include_dirs
            "${_terreate_stripped_include_dir}")
        endif()
      endforeach()
    endif()
  endforeach()

  get_target_property(_terreate_vulkan_linked_targets
    "${_terreate_vulkan_usage_target}" INTERFACE_LINK_LIBRARIES)
  if(_terreate_vulkan_linked_targets AND
     NOT "${_terreate_vulkan_linked_targets}" MATCHES "-NOTFOUND$")
    foreach(_terreate_vulkan_linked_target IN LISTS
        _terreate_vulkan_linked_targets)
      set(_terreate_stripped_linked_target
        "${_terreate_vulkan_linked_target}")
      string(REGEX REPLACE
        "^\\$<LINK_ONLY:([^>]+)>$" "\\1"
        _terreate_stripped_linked_target "${_terreate_stripped_linked_target}")
      string(GENEX_STRIP "${_terreate_stripped_linked_target}"
        _terreate_stripped_linked_target)
      if(TARGET "${_terreate_stripped_linked_target}")
        list(APPEND _terreate_vulkan_usage_targets
          "${_terreate_stripped_linked_target}")
      endif()
    endforeach()
  endif()
endwhile()
list(REMOVE_DUPLICATES _terreate_vulkan_include_dirs)

set(_terreate_vulkan_explicit_include_dir)
if(DEFINED PROJECT_VULKAN_HPP_INCLUDE_DIR AND
   NOT "${PROJECT_VULKAN_HPP_INCLUDE_DIR}" STREQUAL "")
  set(_terreate_vulkan_explicit_include_dir
    "${PROJECT_VULKAN_HPP_INCLUDE_DIR}")
elseif(DEFINED ENV{VULKAN_HEADERS_INCLUDE} AND
       NOT "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  set(_terreate_vulkan_explicit_include_dir
    "$ENV{VULKAN_HEADERS_INCLUDE}")
endif()

# Validate the effective source-consumer order rather than selecting the first
# directory that looks complete in isolation.  A partial directory earlier in
# Vulkan::Vulkan's usage closure must shadow the later Headers directory and
# therefore fail this compile probe.
set(_terreate_vulkan_probe_include_dirs
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include")
if(NOT "${_terreate_vulkan_explicit_include_dir}" STREQUAL "")
  list(APPEND _terreate_vulkan_probe_include_dirs
    "${_terreate_vulkan_explicit_include_dir}")
elseif(DEFINED Vulkan_LIBRARY AND NOT "${Vulkan_LIBRARY}" STREQUAL "" AND
       DEFINED Vulkan_INCLUDE_DIR AND NOT "${Vulkan_INCLUDE_DIR}" STREQUAL "")
  # FindVulkan may expose its headers as a variable while relying on the
  # compiler's implicit include path instead of publishing them on the target.
  # The source target receives this same direct include path below, so it must
  # be probed before Vulkan::Vulkan's transitive closure.
  list(APPEND _terreate_vulkan_probe_include_dirs "${Vulkan_INCLUDE_DIR}")
  set(_terreate_vulkan_hpp_include_dir "${Vulkan_INCLUDE_DIR}")
endif()
list(APPEND _terreate_vulkan_probe_include_dirs
  ${_terreate_vulkan_include_dirs})
list(REMOVE_DUPLICATES _terreate_vulkan_probe_include_dirs)

terreate_probe_vulkan_headers("${_terreate_vulkan_probe_include_dirs}"
  _terreate_vulkan_headers_available)
if(NOT _terreate_vulkan_headers_available)
  message(FATAL_ERROR
    "Terreate Graphics requires Vulkan >= "
    "${TERREATE_MINIMUM_VULKAN_API_VERSION} with a compileable "
    "vulkan/vulkan_core.h, vulkan/vulkan.hpp, and "
    "vulkan/vulkan_raii.hpp usage closure through the effective "
    "Vulkan::Vulkan include order; refusing a loader/basic-only, "
    "partial, or decoy package before compile")
endif()

if(NOT "${_terreate_vulkan_explicit_include_dir}" STREQUAL "")
  set(_terreate_vulkan_hpp_include_dir
    "${_terreate_vulkan_explicit_include_dir}")
endif()

# Graphics is intentionally independent of Platform.  Vulkan is the first
# Graphics-owned native dependency; Window/GLFW, Surface presentation, logical
# Device creation, and VMA/resources remain outside this component.  The
# PhysicalDevice API below is observation-only and owns no native device.
add_library(terreate_graphics SHARED
  src/graphics.cpp
  src/graphics_diagnostics.cpp
  src/instance.cpp
  src/physical_device.cpp)
target_include_directories(terreate_graphics
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(terreate_graphics
  PUBLIC
    Terreate::Core
    Vulkan::Vulkan)

terreate_configure_target(terreate_graphics)

set_target_properties(terreate_graphics PROPERTIES
  EXPORT_NAME Graphics
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  BUILD_RPATH_USE_ORIGIN TRUE
  INSTALL_RPATH "$ORIGIN")
target_compile_definitions(terreate_graphics PRIVATE TERREATE_GRAPHICS_BUILDING_SHARED)

# Graphics is a single relocatable ELF boundary.  Bind calls to definitions in
# this DSO so a consumer cannot interpose the public query/observer methods
# used by production selection, and reject any unresolved implementation
# reference at link time.  Core is PIC and --exclude-libs keeps its static
# implementation out of Graphics' dynamic API.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(terreate_graphics PRIVATE
    "-Wl,-Bsymbolic-functions"
    "-Wl,-z,defs"
    "-Wl,--exclude-libs,ALL")
endif()
add_library(Terreate::Graphics ALIAS terreate_graphics)

install(TARGETS terreate_graphics
  EXPORT TerreateGraphicsTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Builds must compile against the explicitly pinned Vulkan-Hpp include directory
# established by the root project (or its dev-shell environment when Graphics is
# added as a source subdirectory).  Publish that path only to source consumers:
# BUILD_INTERFACE keeps the development-only path out of installed exports,
# while the public Vulkan target remains the installed consumer dependency.
if(NOT "${_terreate_vulkan_hpp_include_dir}" STREQUAL "")
  target_include_directories(terreate_graphics SYSTEM PUBLIC
    "$<BUILD_INTERFACE:${_terreate_vulkan_hpp_include_dir}>")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
