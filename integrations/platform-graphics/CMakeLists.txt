# The first Integration (#227): optional glue that connects two components
# through their public APIs only. Platform stays Vulkan-free and Graphics stays
# independent of Platform; neither depends on this target.
if(NOT TARGET terreate_platform OR NOT TARGET terreate_graphics)
  message(FATAL_ERROR
    "Terreate IntegrationPlatformGraphics requires both Platform and Graphics; "
    "refusing to create an implicit component dependency")
endif()

add_library(terreate_integration_platform_graphics STATIC
  src/surface.cpp
  include/terreate/integration/platform_graphics/surface.hpp)
target_include_directories(terreate_integration_platform_graphics
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Platform brings GLFW and Graphics brings Vulkan, as correct usage
# requirements for this static archive's own references.
target_link_libraries(terreate_integration_platform_graphics PUBLIC
  Terreate::Platform Terreate::Graphics)
terreate_configure_target(terreate_integration_platform_graphics)

set_target_properties(terreate_integration_platform_graphics PROPERTIES
  EXPORT_NAME IntegrationPlatformGraphics)
add_library(Terreate::IntegrationPlatformGraphics ALIAS
  terreate_integration_platform_graphics)

install(TARGETS terreate_integration_platform_graphics
  EXPORT TerreateIntegrationPlatformGraphicsTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
