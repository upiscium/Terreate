cmake_minimum_required(VERSION 3.28)
project(TerreateComponentMatrix LANGUAGES CXX)

foreach(_required_variable IN ITEMS TERREATE_SOURCE_DIR TERREATE_MATRIX_COMPONENT)
  if(NOT DEFINED ${_required_variable} OR
     "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_required_variable} is required")
  endif()
endforeach()

add_subdirectory("${TERREATE_SOURCE_DIR}" terreate)

set(_component "${TERREATE_MATRIX_COMPONENT}")
if(NOT TARGET Terreate::${_component})
  message(FATAL_ERROR "expected Terreate::${_component} to be configured")
endif()

foreach(_component_name IN ITEMS Core Asset Platform Graphics)
  string(TOUPPER "${_component_name}" _component_upper)
  if(TERREATE_EXPECT_${_component_upper})
    if(NOT TARGET Terreate::${_component_name})
      message(FATAL_ERROR
        "expected enabled target Terreate::${_component_name}")
    endif()
  elseif(TARGET Terreate::${_component_name})
    message(FATAL_ERROR
      "disabled component unexpectedly created Terreate::${_component_name}")
  endif()
endforeach()

# The Platform-Graphics Integration (#363): present exactly when expected, and
# it connects Platform and Graphics, never the other way around.
if(TERREATE_EXPECT_INTEGRATION_PLATFORM_GRAPHICS)
  if(NOT TARGET Terreate::IntegrationPlatformGraphics)
    message(FATAL_ERROR "expected enabled target Terreate::IntegrationPlatformGraphics")
  endif()
  get_target_property(_integration_links terreate_integration_platform_graphics
    INTERFACE_LINK_LIBRARIES)
  if(NOT "${_integration_links}" MATCHES "Platform" OR
     NOT "${_integration_links}" MATCHES "Graphics")
    message(FATAL_ERROR
      "the Integration must publicly link Platform and Graphics: ${_integration_links}")
  endif()
elseif(TARGET Terreate::IntegrationPlatformGraphics)
  message(FATAL_ERROR
    "disabled component unexpectedly created Terreate::IntegrationPlatformGraphics")
endif()

# The Asset-Graphics Integration (#279): present exactly when expected, and its
# public dependencies are exactly Asset and Graphics (Core and Vulkan reach it
# through them). Nothing links Platform, and no module links it.
if(TERREATE_EXPECT_INTEGRATION_ASSET_GRAPHICS)
  if(NOT TARGET Terreate::IntegrationAssetGraphics)
    message(FATAL_ERROR "expected enabled target Terreate::IntegrationAssetGraphics")
  endif()
  foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_ag_links terreate_integration_asset_graphics ${_link_property})
    list(SORT _ag_links)
    if(NOT "${_ag_links}" STREQUAL "Terreate::Asset;Terreate::Graphics")
      message(FATAL_ERROR
        "the Asset-Graphics Integration must publicly link exactly Asset and Graphics "
        "in ${_link_property}: ${_ag_links}")
    endif()
  endforeach()
elseif(TARGET Terreate::IntegrationAssetGraphics)
  message(FATAL_ERROR
    "disabled component unexpectedly created Terreate::IntegrationAssetGraphics")
endif()

# The stb_image Adapter (#277): present exactly when expected. Its only public
# dependency is Asset (Core comes through it); stb is a private build input, so
# it appears in neither its link interface nor its public include directories,
# and it never links Graphics, Platform or an Integration. No module links it.
if(TERREATE_EXPECT_ADAPTER_ASSET_IMAGE_STB)
  if(NOT TARGET Terreate::AdapterAssetImageStb)
    message(FATAL_ERROR "expected enabled target Terreate::AdapterAssetImageStb")
  endif()
  foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_stb_links terreate_adapter_asset_image_stb ${_link_property})
    if(NOT "${_stb_links}" STREQUAL "Terreate::Asset")
      message(FATAL_ERROR
        "the stb_image Adapter must publicly link exactly Asset in ${_link_property}: "
        "${_stb_links}")
    endif()
  endforeach()
  foreach(_interface_property IN ITEMS
      INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
      INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS)
    get_target_property(_stb_interface terreate_adapter_asset_image_stb ${_interface_property})
    if("${_stb_interface}" MATCHES "stb_image|stb-[0-9]|STB_")
      message(FATAL_ERROR
        "stb_image leaked into the public ${_interface_property} of the Adapter: "
        "${_stb_interface}")
    endif()
  endforeach()
elseif(TARGET Terreate::AdapterAssetImageStb)
  message(FATAL_ERROR
    "disabled component unexpectedly created Terreate::AdapterAssetImageStb")
endif()

# The cgltf Adapter (#278): present exactly when expected. Like the stb_image Adapter its only
# public dependency is Asset (Core comes through it), not the stb_image Adapter; cgltf is a
# private build input, so it appears in neither its link interface nor its public include
# directories, and it never links Graphics, Platform, an Integration or another Adapter. No
# module links it.
if(TERREATE_EXPECT_ADAPTER_ASSET_GLTF_CGLTF)
  if(NOT TARGET Terreate::AdapterAssetGltfCgltf)
    message(FATAL_ERROR "expected enabled target Terreate::AdapterAssetGltfCgltf")
  endif()
  foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_gltf_links terreate_adapter_asset_gltf_cgltf ${_link_property})
    if(NOT "${_gltf_links}" STREQUAL "Terreate::Asset")
      message(FATAL_ERROR
        "the cgltf Adapter must publicly link exactly Asset in ${_link_property}: "
        "${_gltf_links}")
    endif()
  endforeach()
  foreach(_interface_property IN ITEMS
      INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
      INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS)
    get_target_property(_gltf_interface terreate_adapter_asset_gltf_cgltf ${_interface_property})
    if("${_gltf_interface}" MATCHES "cgltf\\.h|CGLTF_")
      message(FATAL_ERROR
        "cgltf leaked into the public ${_interface_property} of the Adapter: "
        "${_gltf_interface}")
    endif()
  endforeach()
  if(TARGET terreate_adapter_asset_image_stb)
    foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_stb_links terreate_adapter_asset_image_stb ${_link_property})
      if("${_stb_links}" MATCHES "[Gg]ltf")
        message(FATAL_ERROR
          "the stb_image Adapter must not link the cgltf Adapter in ${_link_property}: "
          "${_stb_links}")
      endif()
    endforeach()
  endif()
elseif(TARGET Terreate::AdapterAssetGltfCgltf)
  message(FATAL_ERROR
    "disabled component unexpectedly created Terreate::AdapterAssetGltfCgltf")
endif()
foreach(_module IN ITEMS core asset platform graphics)
  if(TARGET terreate_${_module})
    foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_module_links terreate_${_module} ${_link_property})
      if("${_module_links}" MATCHES "[Ii]ntegration|[Aa]dapter|[Ss]tb")
        message(FATAL_ERROR
          "module ${_module} depends upward on an Integration or Adapter in ${_link_property}: "
          "${_module_links}")
      endif()
    endforeach()
  endif()
endforeach()

foreach(_target_name IN ITEMS Core Asset Platform Graphics)
  string(TOLOWER "${_target_name}" _target_lower)
  if(TARGET terreate_${_target_lower})
    foreach(_interface_property IN ITEMS
        INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
      get_target_property(_interface_includes terreate_${_target_lower}
        ${_interface_property})
      if("${_interface_includes}" MATCHES "(^|;)[^;]*/src(;|$)" OR
         "${_interface_includes}" MATCHES "/src/")
        message(FATAL_ERROR
          "private ${_target_name} include directory leaked into its public "
          "${_interface_property}: ${_interface_includes}")
      endif()
    endforeach()
    get_target_property(_module_scanning terreate_${_target_lower}
      CXX_SCAN_FOR_MODULES)
    if(NOT "${_module_scanning}" STREQUAL "OFF")
      message(FATAL_ERROR
        "C++ module scanning must be OFF for Terreate::${_target_name}")
    endif()
  endif()
endforeach()

foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
  if(TARGET terreate_core)
    get_target_property(_core_links terreate_core ${_link_property})
    if("${_core_links}" MATCHES "[Aa]sset|[Pp]latform|[Gg]raphics")
      message(FATAL_ERROR
        "Core has an upward component dependency in ${_link_property}: "
        "${_core_links}")
    endif()
  endif()
  # Asset is CPU-only: Core is its only dependency. It never links Graphics,
  # Platform, an Integration, Vulkan or GLFW, in either direction of usage.
  if(TARGET terreate_asset)
    get_target_property(_asset_links terreate_asset ${_link_property})
    if(NOT "${_asset_links}" MATCHES "[Cc]ore")
      message(FATAL_ERROR
        "Asset must retain its public Core dependency in "
        "${_link_property}: ${_asset_links}")
    endif()
    if("${_asset_links}" MATCHES "[Gg]raphics|[Pp]latform|[Ii]ntegration|[Vv]ulkan|glfw")
      message(FATAL_ERROR
        "Asset has an invalid dependency in ${_link_property}: ${_asset_links}")
    endif()
  endif()
  if(TARGET terreate_platform)
    get_target_property(_platform_links terreate_platform ${_link_property})
    if(NOT "${_platform_links}" MATCHES "[Cc]ore")
      message(FATAL_ERROR
        "Platform must retain its public Core dependency in "
        "${_link_property}: ${_platform_links}")
    endif()
    if("${_platform_links}" MATCHES "[Gg]raphics")
      message(FATAL_ERROR
        "Platform has an invalid Graphics dependency in ${_link_property}: "
        "${_platform_links}")
    endif()
  endif()
  if(TARGET terreate_graphics)
    get_target_property(_graphics_links terreate_graphics ${_link_property})
    if(NOT "${_graphics_links}" MATCHES "[Cc]ore")
      message(FATAL_ERROR
        "Graphics must retain its public Core dependency in "
        "${_link_property}: ${_graphics_links}")
    endif()
    if("${_graphics_links}" MATCHES "[Aa]sset")
      message(FATAL_ERROR
        "Graphics must not depend on Asset in ${_link_property}: "
        "${_graphics_links}")
    endif()
    if("${_graphics_links}" MATCHES "[Pp]latform")
      message(FATAL_ERROR
        "Graphics must not depend on Platform in ${_link_property}: "
        "${_graphics_links}")
    endif()
  endif()
endforeach()

if(NOT _component MATCHES "^(Core|Asset|Platform|Graphics|IntegrationPlatformGraphics|IntegrationAssetGraphics|AdapterAssetImageStb|AdapterAssetGltfCgltf)$")
  message(FATAL_ERROR "unknown matrix component ${_component}")
endif()

if(_component STREQUAL "Graphics")
  set(_consumer_source [=[
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/allocator.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <type_traits>

static_assert(VK_API_VERSION_1_3 != 0);
static_assert(sizeof(vk::raii::Context) > 0);
static_assert(sizeof(vk::raii::Device) > 0);
static_assert(!std::is_copy_constructible_v<terreate::graphics::Allocator>);
static_assert(std::is_move_constructible_v<terreate::graphics::Allocator>);

int main() {
  const auto capabilities = terreate::graphics::queryInstanceCapabilities();
  return capabilities.has_value() ? 0 : 1;
}
]=])
elseif(_component STREQUAL "Asset")
  # One include proves the Asset usage requirements reach a consumer, and the
  # consumer sees no Vulkan or GLFW header through them.
  set(_consumer_source [=[
#include <terreate/asset/mesh_data.hpp>

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR)
#error "Asset must not expose Vulkan or GLFW"
#endif

int main() {
  const terreate::asset::MeshData mesh;
  return terreate::asset::validate(mesh).has_value() ? 1 : 0;
}
]=])
elseif(_component STREQUAL "IntegrationPlatformGraphics")
  # The public header of the Integration pulls in Platform and Graphics, so one
  # include proves all three usage requirements reach a consumer.
  set(_consumer_source [=[
#include <terreate/integration/platform_graphics/surface.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<
    decltype(terreate::integration::platform_graphics::createWindowSurface(
        std::declval<const terreate::graphics::Instance &>(),
        std::declval<const terreate::platform::Runtime &>(),
        std::declval<const terreate::platform::Window &>())),
    terreate::Result<terreate::graphics::Surface>>);

int main() { return 0; }
]=])
elseif(_component STREQUAL "AdapterAssetImageStb")
  # One include proves the Adapter's and Asset's usage requirements reach a consumer, and
  # that the consumer sees neither stb_image, Vulkan nor GLFW through them.
  set(_consumer_source [=[
#include <terreate/adapter/asset_image_stb/decoder.hpp>

#include <type_traits>

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR) || \
    defined(STBI_INCLUDE_STB_IMAGE_H)
#error "the stb_image Adapter must not expose stb_image, Vulkan or GLFW"
#endif

static_assert(std::is_same_v<
    decltype(terreate::adapter::asset_image_stb::decodeImage(
        std::declval<const terreate::asset::DecodeInput &>())),
    terreate::Result<terreate::adapter::asset_image_stb::ImageDecodeResult>>);

int main() {
  // The header needs no stb: an empty input is refused by name.
  const auto result = terreate::adapter::asset_image_stb::decodeImage({});
  return result.has_value() ? 1 : 0;
}
]=])
elseif(_component STREQUAL "AdapterAssetGltfCgltf")
  # One include proves the Adapter's and Asset's usage requirements reach a consumer, and that
  # the consumer sees neither cgltf, stb_image, Vulkan nor GLFW through them.
  set(_consumer_source [=[
#include <terreate/adapter/asset_gltf_cgltf/decoder.hpp>

#include <type_traits>

#if defined(VK_VERSION_1_0) || defined(VULKAN_HPP_H) || defined(GLFW_VERSION_MAJOR) || \
    defined(CGLTF_H_INCLUDED__) || defined(STBI_INCLUDE_STB_IMAGE_H)
#error "the cgltf Adapter must not expose cgltf, stb_image, Vulkan or GLFW"
#endif

static_assert(std::is_same_v<
    decltype(terreate::adapter::asset_gltf_cgltf::decodeGltf(
        std::declval<const terreate::asset::DecodeInput &>())),
    terreate::Result<terreate::adapter::asset_gltf_cgltf::GltfDecodeResult>>);

int main() {
  // The header needs no cgltf: an empty input is refused by name.
  const auto result = terreate::adapter::asset_gltf_cgltf::decodeGltf({});
  return result.has_value() ? 1 : 0;
}
]=])
elseif(_component STREQUAL "IntegrationAssetGraphics")
  # The public header of the Integration pulls in Asset and Graphics, so one
  # include proves both usage requirements reach a consumer (and no Platform).
  set(_consumer_source [=[
#include <terreate/integration/asset_graphics/image_upload.hpp>
#include <terreate/integration/asset_graphics/mesh_upload.hpp>
#include <terreate/integration/asset_graphics/mesh_upload_convenience.hpp>

#include <type_traits>
#include <utility>

#if defined(GLFW_VERSION_MAJOR)
#error "the Asset-Graphics Integration must not expose GLFW"
#endif

static_assert(std::is_same_v<
    decltype(terreate::integration::asset_graphics::planMeshUpload(
        std::declval<const terreate::asset::MeshData &>(),
        std::declval<const terreate::integration::asset_graphics::MeshUploadDescription &>())),
    terreate::Result<terreate::integration::asset_graphics::MeshUploadPlan>>);

int main() { return 0; }
]=])
else()
  set(_consumer_source [=[
int main() { return 0; }
]=])
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/matrix_consumer.cpp"
  "${_consumer_source}")
add_executable(matrix_consumer
  "${CMAKE_CURRENT_BINARY_DIR}/matrix_consumer.cpp")
target_link_libraries(matrix_consumer PRIVATE Terreate::${_component})
set_target_properties(matrix_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
set_property(TARGET matrix_consumer PROPERTY CXX_SCAN_FOR_MODULES OFF)
