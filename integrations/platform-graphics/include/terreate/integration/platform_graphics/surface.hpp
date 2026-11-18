#ifndef TERREATE_INTEGRATION_PLATFORM_GRAPHICS_SURFACE_HPP
#define TERREATE_INTEGRATION_PLATFORM_GRAPHICS_SURFACE_HPP

#include <cstdint>
#include <system_error>

#include <terreate/core/result.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/platform/runtime.hpp>
#include <terreate/platform/window.hpp>

namespace terreate::integration::platform_graphics {

enum class SurfaceError : std::uint8_t {
  /// The Window was not created by this Runtime, or one of them is moved-from.
  parent_mismatch = 1,
  /// The Instance did not enable an extension GLFW needs to create window
  /// surfaces on this backend. The Error's detail names the missing one.
  required_extension_not_enabled,
};

[[nodiscard]] const std::error_category &surface_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SurfaceError error) noexcept;

/// Create a Vulkan Surface for a Platform Window (glfwCreateWindowSurface).
///
/// This is the one place Platform and Graphics meet. Neither depends on the
/// other: Platform stays Vulkan-free and Graphics knows nothing of windows.
/// This Integration uses only their public APIs.
///
/// No logical Device or Queue is needed, so the Surface can exist before the
/// Device is created. Every check runs before the native call, in this order:
/// - `parent_mismatch`: `window` was not created by `runtime`;
/// - the Platform errors of `runtime.requiredInstanceExtensions()`, returned
///   unchanged: `wrong_thread` (the Runtime thread rule) and
///   `vulkan_unavailable` (GLFW names no surface extensions, as on the null
///   backend; this is never a silent success);
/// - `required_extension_not_enabled`: `instance` did not enable every
///   extension in that list. The application passes the list to its Instance
///   request through `InstanceDescription::required_extensions`;
/// - the Graphics errors of graphics::createSurface (an invalid Instance, a
///   missing VK_KHR_surface), and a native VkResult failure from GLFW.
/// GLFW's own errors also reach the Runtime's diagnostic sink.
///
/// Lifetime contract, not checked: the returned Surface is destroyed before
/// the Window and before the Instance, and after every swapchain created for
/// it. All of `instance`, `runtime` and `window` are borrowed only during the
/// call.
[[nodiscard]] auto createWindowSurface(const graphics::Instance &instance,
                                       const platform::Runtime &runtime,
                                       const platform::Window &window)
    -> terreate::Result<graphics::Surface>;

} // namespace terreate::integration::platform_graphics

namespace std {
template <>
struct is_error_code_enum<terreate::integration::platform_graphics::SurfaceError> : true_type {};
} // namespace std

#endif // TERREATE_INTEGRATION_PLATFORM_GRAPHICS_SURFACE_HPP
