if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Graphics requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

# Graphics is intentionally independent of Platform.  No Vulkan, GLFW, or VMA
# API is introduced until a later component task owns that dependency.
set(_terreate_graphics_anchor
  "${CMAKE_CURRENT_BINARY_DIR}/graphics_anchor.cpp")
set(_terreate_graphics_anchor_content [=[
#include <terreate/graphics.hpp>
#include "detail/graphics_detail.hpp"
namespace terreate::graphics {
void component_anchor() noexcept {
  (void)detail::component_id;
}
}
]=])
file(WRITE "${_terreate_graphics_anchor}"
  "${_terreate_graphics_anchor_content}")

add_library(terreate_graphics STATIC "${_terreate_graphics_anchor}")
target_include_directories(terreate_graphics
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(terreate_graphics PUBLIC Terreate::Core)
terreate_configure_target(terreate_graphics)

set_target_properties(terreate_graphics PROPERTIES
  EXPORT_NAME Graphics)
add_library(Terreate::Graphics ALIAS terreate_graphics)

install(TARGETS terreate_graphics
  EXPORT TerreateTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
