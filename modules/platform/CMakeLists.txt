if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Platform requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

# Platform has no external backend in this skeleton.  Generate its anchor so
# the component remains a real independently linkable target without adding a
# placeholder production API or a GLFW/VMA dependency.
set(_terreate_platform_anchor
  "${CMAKE_CURRENT_BINARY_DIR}/platform_anchor.cpp")
set(_terreate_platform_anchor_content [=[
#include <terreate/platform.hpp>
#include "detail/platform_detail.hpp"
namespace terreate::platform {
void component_anchor() noexcept {
  (void)detail::component_id;
}
}
]=])
file(WRITE "${_terreate_platform_anchor}"
  "${_terreate_platform_anchor_content}")

add_library(terreate_platform STATIC "${_terreate_platform_anchor}")
target_include_directories(terreate_platform
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(terreate_platform PUBLIC Terreate::Core)
terreate_configure_target(terreate_platform)

set_target_properties(terreate_platform PROPERTIES
  EXPORT_NAME Platform)
add_library(Terreate::Platform ALIAS terreate_platform)

install(TARGETS terreate_platform
  EXPORT TerreateTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
