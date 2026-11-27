# The Asset-Graphics Integration (#279): optional glue that turns an Asset
# MeshData into Graphics buffers and upload items through the public APIs only.
# Asset stays Vulkan-free and Graphics stays independent of Asset; neither
# depends on this target.
if(NOT TARGET terreate_asset OR NOT TARGET terreate_graphics)
  message(FATAL_ERROR
    "Terreate IntegrationAssetGraphics requires both Asset and Graphics; "
    "refusing to create an implicit component dependency")
endif()

add_library(terreate_integration_asset_graphics STATIC
  src/mesh_upload.cpp
  src/image_upload.cpp
  include/terreate/integration/asset_graphics/mesh_upload.hpp
  include/terreate/integration/asset_graphics/image_upload.hpp)
target_include_directories(terreate_integration_asset_graphics
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Exactly Asset and Graphics (Core and Vulkan come through them), as correct
# usage requirements for this static archive's own references.
target_link_libraries(terreate_integration_asset_graphics PUBLIC
  Terreate::Asset Terreate::Graphics)
terreate_configure_target(terreate_integration_asset_graphics)

set_target_properties(terreate_integration_asset_graphics PROPERTIES
  EXPORT_NAME IntegrationAssetGraphics)
add_library(Terreate::IntegrationAssetGraphics ALIAS terreate_integration_asset_graphics)

install(TARGETS terreate_integration_asset_graphics
  EXPORT TerreateIntegrationAssetGraphicsTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
