if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Graphics requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

find_package(Vulkan REQUIRED)

# Graphics is intentionally independent of Platform.  Vulkan is the first
# Graphics-owned native dependency; Window/GLFW, Surface, PhysicalDevice,
# Device, and VMA remain outside this component.
add_library(terreate_graphics STATIC
  src/graphics.cpp
  src/graphics_diagnostics.cpp
  src/instance.cpp)
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

# Top-level builds must compile against the explicitly pinned Vulkan-Hpp
# include directory established by the root project.  The public Vulkan target
# remains the installed consumer dependency; a direct private include keeps
# this target out of the installed export set and prevents fallback to an
# inherited SDK include path.
if(DEFINED PROJECT_VULKAN_HPP_INCLUDE_DIR AND
   NOT "${PROJECT_VULKAN_HPP_INCLUDE_DIR}" STREQUAL "")
  target_include_directories(terreate_graphics SYSTEM PRIVATE
    "${PROJECT_VULKAN_HPP_INCLUDE_DIR}")
elseif(DEFINED ENV{VULKAN_HEADERS_INCLUDE} AND
       NOT "$ENV{VULKAN_HEADERS_INCLUDE}" STREQUAL "")
  target_include_directories(terreate_graphics SYSTEM PRIVATE
    "$ENV{VULKAN_HEADERS_INCLUDE}")
endif()
terreate_configure_target(terreate_graphics)

set_target_properties(terreate_graphics PROPERTIES
  EXPORT_NAME Graphics)
add_library(Terreate::Graphics ALIAS terreate_graphics)

install(TARGETS terreate_graphics
  EXPORT TerreateGraphicsTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
