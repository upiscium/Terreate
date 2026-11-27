# The stb_image Adapter (#277): decodes baseline image formats from memory into an
# Asset ImageData. It is an Adapter in the architecture sense: it depends on its
# contract (Asset) and its backend (stb_image) only, and Asset, Graphics and every
# other component stay free of it and of stb.
if(NOT TARGET terreate_asset)
  message(FATAL_ERROR
    "Terreate AdapterAssetImageStb requires Asset; refusing to create an "
    "implicit component dependency")
endif()
if("${_terreate_stb_include_dir}" STREQUAL "")
  message(FATAL_ERROR
    "Terreate AdapterAssetImageStb needs stb_image.h; the top-level project "
    "resolves it (TERREATE_STB_INCLUDE_DIR or STB_INCLUDE)")
endif()

add_library(terreate_adapter_asset_image_stb STATIC
  src/decoder.cpp
  src/stb_image_implementation.cpp
  src/stb_image_backend.hpp
  include/terreate/adapter/asset_image_stb/decoder.hpp)
target_include_directories(terreate_adapter_asset_image_stb
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# stb_image is a private implementation detail: it is compiled into this static
# library, no public header includes it, and it is not part of the usage
# requirements, so a consumer needs no stb. The directory is a SYSTEM include so
# the third-party header never produces diagnostics of its own (nor for clang-tidy,
# which treats system headers as not ours).
target_include_directories(terreate_adapter_asset_image_stb SYSTEM PRIVATE
  "${_terreate_stb_include_dir}")
# Exactly Asset (Core comes through it).
target_link_libraries(terreate_adapter_asset_image_stb PUBLIC Terreate::Asset)
terreate_configure_target(terreate_adapter_asset_image_stb)

set_target_properties(terreate_adapter_asset_image_stb PROPERTIES
  EXPORT_NAME AdapterAssetImageStb)
add_library(Terreate::AdapterAssetImageStb ALIAS terreate_adapter_asset_image_stb)

install(TARGETS terreate_adapter_asset_image_stb
  EXPORT TerreateAdapterAssetImageStbTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
