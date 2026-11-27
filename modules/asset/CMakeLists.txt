if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Asset requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

# Asset is CPU-only: it has no native dependency (no Vulkan, no GLFW) and
# links neither Graphics nor Platform, so a consumer of Asset alone receives
# nothing but Core (#345).
add_library(terreate_asset STATIC
  src/image_data.cpp
  src/mesh_data.cpp
  src/primitives.cpp
  include/terreate/asset/image_data.hpp
  include/terreate/asset/mesh_data.hpp
  include/terreate/asset/primitives.hpp)
target_include_directories(terreate_asset
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(terreate_asset PUBLIC Terreate::Core)
terreate_configure_target(terreate_asset)

set_target_properties(terreate_asset PROPERTIES
  EXPORT_NAME Asset)
add_library(Terreate::Asset ALIAS terreate_asset)

install(TARGETS terreate_asset
  EXPORT TerreateAssetTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
