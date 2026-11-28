# The cgltf Adapter (#278): decodes glTF 2.0 and GLB from memory into an Asset
# ModelData. It is an Adapter in the architecture sense: it depends on its
# contract (Asset) and its backend (cgltf) only, and Asset, Graphics, the stb
# Adapter and every other component stay free of it and of cgltf.
if(NOT TARGET terreate_asset)
  message(FATAL_ERROR
    "Terreate AdapterAssetGltfCgltf requires Asset; refusing to create an "
    "implicit component dependency")
endif()
if("${_terreate_cgltf_include_dir}" STREQUAL "")
  message(FATAL_ERROR
    "Terreate AdapterAssetGltfCgltf needs cgltf.h; the top-level project "
    "resolves it (TERREATE_CGLTF_INCLUDE_DIR or CGLTF_INCLUDE)")
endif()

add_library(terreate_adapter_asset_gltf_cgltf STATIC
  src/decoder.cpp
  src/cgltf_implementation.cpp
  src/cgltf_backend.hpp
  include/terreate/adapter/asset_gltf_cgltf/decoder.hpp)
target_include_directories(terreate_adapter_asset_gltf_cgltf
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# cgltf is a private implementation detail: it is compiled into this static
# library, no public header includes it, and it is not part of the usage
# requirements, so a consumer needs no cgltf. The directory is a SYSTEM include so
# the third-party header never produces diagnostics of its own (nor for clang-tidy,
# which treats system headers as not ours).
target_include_directories(terreate_adapter_asset_gltf_cgltf SYSTEM PRIVATE
  "${_terreate_cgltf_include_dir}")
# Exactly Asset (Core comes through it); not the stb Adapter.
target_link_libraries(terreate_adapter_asset_gltf_cgltf PUBLIC Terreate::Asset)
terreate_configure_target(terreate_adapter_asset_gltf_cgltf)

set_target_properties(terreate_adapter_asset_gltf_cgltf PROPERTIES
  EXPORT_NAME AdapterAssetGltfCgltf)
add_library(Terreate::AdapterAssetGltfCgltf ALIAS terreate_adapter_asset_gltf_cgltf)

install(TARGETS terreate_adapter_asset_gltf_cgltf
  EXPORT TerreateAdapterAssetGltfCgltfTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
