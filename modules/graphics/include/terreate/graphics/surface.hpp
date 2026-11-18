#ifndef TERREATE_GRAPHICS_SURFACE_HPP
#define TERREATE_GRAPHICS_SURFACE_HPP

#include <cstdint>
#include <memory>
#include <system_error>

#include <terreate/core/result.hpp>
#include <terreate/graphics/instance.hpp>

namespace terreate::graphics {

enum class SurfaceError : std::uint8_t {
  invalid_instance = 1,
  surface_extension_not_enabled,
  /// Only with the default destroyer: vkDestroySurfaceKHR could not be loaded.
  function_unavailable,
  invalid_creator,
  invalid_surface,
  allocation_failed,
};

[[nodiscard]] const std::error_category &surface_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(SurfaceError error) noexcept;

/// How a Surface is destroyed. A null `destroy` means the default: Graphics
/// calls vkDestroySurfaceKHR(instance, surface, nullptr). A backend whose
/// surface needs more than that (a custom allocator, a window-system library
/// that owns extra state) supplies its own function, which must destroy the
/// surface. `user` must stay valid until the Surface is destroyed.
struct SurfaceDestroyer {
  using Function = void (*)(VkInstance instance, VkSurfaceKHR surface, void *user);
  Function destroy = nullptr;
  void *user = nullptr;
};

/// How to create a native surface: the escape hatch for any window system
/// (xcb, Wayland, Win32, VK_EXT_headless_surface, ...). Graphics calls
/// `create` once, synchronously, and takes ownership of the result at once, so
/// no raw handle can leak between creation and ownership. The function must
/// return VK_SUCCESS only with a non-null handle in `*surface`, and leave no
/// surface behind when it fails. Unless `destroyer` is given, the surface must
/// have been created with no custom allocator.
struct SurfaceCreator {
  using Function = VkResult (*)(VkInstance instance, VkSurfaceKHR *surface, void *user);
  Function create = nullptr;
  void *user = nullptr;
  SurfaceDestroyer destroyer{};
};

/// A move-only owner of one VkSurfaceKHR. It needs no Device or Queue, so it
/// can exist before the logical Device is created. Nothing here knows about
/// windows: a Surface is made by createSurface or adoptSurface.
///
/// Lifetime contract, not checked (like Device before Instance): the Surface
/// must be destroyed before its Instance, before the native window it was
/// created for, and after every VkSwapchainKHR created for it has been
/// destroyed (VUID-vkDestroySurfaceKHR-surface-01266). The Instance is
/// borrowed, never retained.
class Surface final {
public:
  Surface() = delete;
  Surface(const Surface &) = delete;
  Surface &operator=(const Surface &) = delete;
  Surface(Surface &&other) noexcept;
  /// Destroys the surface this object owned before taking over `other`'s.
  Surface &operator=(Surface &&other) noexcept;
  ~Surface();

  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] bool valid() const noexcept { return implementation_ != nullptr; }

  /// The borrowed native handle; never destroy it. Null for a moved-from
  /// Surface.
  [[nodiscard]] vk::SurfaceKHR nativeHandle() const noexcept;
  [[nodiscard]] VkSurfaceKHR nativeSurface() const noexcept;
  /// The VkInstance that owns this Surface.
  [[nodiscard]] vk::Instance nativeInstance() const noexcept;
  /// True when `instance` is the Instance that owns this Surface (by native
  /// handle, like every Graphics correlatedWith).
  [[nodiscard]] bool correlatedWith(const Instance &instance) const noexcept;

  struct Impl;

private:
  explicit Surface(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_{};

  friend auto createSurface(const Instance &, const SurfaceCreator &) -> terreate::Result<Surface>;
  friend auto adoptSurface(const Instance &, vk::SurfaceKHR, SurfaceDestroyer)
      -> terreate::Result<Surface>;
};

/// Create a Surface through `creator`. Fails, before any native call, with
/// invalid_instance, surface_extension_not_enabled (the Instance plan did not
/// enable VK_KHR_surface; never a silent no-op) or invalid_creator. A native
/// VkResult failure from the creator is returned as the native error; a null
/// handle after VK_SUCCESS is invalid_surface.
[[nodiscard]] auto createSurface(const Instance &instance, const SurfaceCreator &creator)
    -> terreate::Result<Surface>;

/// Take ownership of a surface created elsewhere. The caller vouches that
/// `surface` is a live surface of `instance`, created with no custom allocator
/// unless `destroyer` is given; only a null handle is rejected
/// (invalid_surface). The same Instance checks apply.
[[nodiscard]] auto adoptSurface(const Instance &instance, vk::SurfaceKHR surface,
                                SurfaceDestroyer destroyer = {}) -> terreate::Result<Surface>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::SurfaceError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_SURFACE_HPP
