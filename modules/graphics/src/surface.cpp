#include <terreate/graphics/surface.hpp>

#include <algorithm>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace terreate::graphics {
namespace {
class SurfaceErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.surface"; }
  std::string message(int value) const override {
    switch (static_cast<SurfaceError>(value)) {
    case SurfaceError::invalid_instance:
      return "invalid Instance";
    case SurfaceError::surface_extension_not_enabled:
      return "VK_KHR_surface is not enabled on the Instance";
    case SurfaceError::function_unavailable:
      return "the default vkDestroySurfaceKHR could not be loaded";
    case SurfaceError::invalid_creator:
      return "invalid SurfaceCreator";
    case SurfaceError::invalid_surface:
      return "no surface was provided";
    case SurfaceError::allocation_failed:
      return "allocation failed";
    }
    return "unknown surface error";
  }
};

terreate::Result<Surface> fail(SurfaceError error, const char *operation, const char *detail) {
  try {
    return std::unexpected(terreate::Error{make_error_code(error), operation, detail});
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return std::unexpected(terreate::Error{make_error_code(error)});
  }
}

terreate::Result<Surface> nativeFailure(VkResult result, const char *operation) {
  try {
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           operation, "native surface creation failed"});
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result))});
  }
}

// Checks shared by createSurface and adoptSurface, in the order the public
// contract promises: they run before any native call.
std::optional<terreate::Result<Surface>> checkInstance(const Instance &instance,
                                                       const char *operation) {
  if (!instance.valid() || instance.plan() == nullptr)
    return fail(SurfaceError::invalid_instance, operation, "moved-from or plan-less Instance");
  const auto &enabled = instance.plan()->enabledExtensions();
  if (std::ranges::find(enabled, std::string_view{VK_KHR_SURFACE_EXTENSION_NAME}) == enabled.end())
    return fail(SurfaceError::surface_extension_not_enabled, operation,
                "enable VK_KHR_surface in the Instance request");
  return std::nullopt;
}

// The default destroy function is loaded only when no destroyer is given, and
// only after every check that needs no native call. vkDestroySurfaceKHR belongs
// to an instance extension, so it is loaded through the Instance, as for Debug
// Utils. `function_unavailable` therefore applies to the default path only.
std::optional<terreate::Result<Surface>> loadDefaultDestroy(const Instance &instance,
                                                            const SurfaceDestroyer &destroyer,
                                                            const char *operation,
                                                            PFN_vkDestroySurfaceKHR &out) {
  if (destroyer.destroy != nullptr)
    return std::nullopt;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  out = reinterpret_cast<PFN_vkDestroySurfaceKHR>(vkGetInstanceProcAddr(
      static_cast<VkInstance>(instance.nativeHandle()), "vkDestroySurfaceKHR"));
  if (out == nullptr)
    return fail(SurfaceError::function_unavailable, operation, "vkGetInstanceProcAddr");
  return std::nullopt;
}
} // namespace

struct Surface::Impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  SurfaceDestroyer destroyer{};
  PFN_vkDestroySurfaceKHR default_destroy = nullptr;

  Impl() = default;
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  ~Impl() {
    if (surface == VK_NULL_HANDLE)
      return;
    if (destroyer.destroy != nullptr)
      destroyer.destroy(instance, surface, destroyer.user);
    else if (default_destroy != nullptr)
      default_destroy(instance, surface, nullptr);
  }
};

const std::error_category &surface_error_category() noexcept {
  static const SurfaceErrorCategory category;
  return category;
}
std::error_code make_error_code(SurfaceError error) noexcept {
  return {static_cast<int>(error), surface_error_category()};
}

Surface::Surface(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Surface::Surface(Surface &&other) noexcept = default;
Surface &Surface::operator=(Surface &&other) noexcept = default;
Surface::~Surface() = default;

vk::SurfaceKHR Surface::nativeHandle() const noexcept { return vk::SurfaceKHR{nativeSurface()}; }
VkSurfaceKHR Surface::nativeSurface() const noexcept {
  return implementation_ == nullptr ? VK_NULL_HANDLE : implementation_->surface;
}
vk::Instance Surface::nativeInstance() const noexcept {
  return implementation_ == nullptr ? vk::Instance{} : vk::Instance{implementation_->instance};
}
bool Surface::correlatedWith(const Instance &instance) const noexcept {
  return valid() && instance.valid() &&
         implementation_->instance == static_cast<VkInstance>(instance.nativeHandle());
}

auto createSurface(const Instance &instance, const SurfaceCreator &creator)
    -> terreate::Result<Surface> {
  constexpr auto operation = "createSurface";
  if (auto rejected = checkInstance(instance, operation))
    return std::move(*rejected);
  if (creator.create == nullptr)
    return fail(SurfaceError::invalid_creator, operation, "SurfaceCreator::create is null");
  PFN_vkDestroySurfaceKHR default_destroy = nullptr;
  if (auto rejected = loadDefaultDestroy(instance, creator.destroyer, operation, default_destroy))
    return std::move(*rejected);

  std::unique_ptr<Surface::Impl> impl;
  try {
    impl = std::make_unique<Surface::Impl>();
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(SurfaceError::allocation_failed)});
  }
  impl->instance = static_cast<VkInstance>(instance.nativeHandle());
  impl->destroyer = creator.destroyer;
  impl->default_destroy = default_destroy;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const VkResult result = creator.create(impl->instance, &surface, creator.user);
  // A failed creation leaves no surface (Vulkan sets the handle to null), so
  // there is nothing to destroy; `impl` owns nothing yet.
  if (result != VK_SUCCESS)
    return nativeFailure(result, operation);
  if (surface == VK_NULL_HANDLE)
    return fail(SurfaceError::invalid_surface, operation, "VK_SUCCESS with a null handle");
  impl->surface = surface;
  return Surface{std::move(impl)};
}

auto adoptSurface(const Instance &instance, vk::SurfaceKHR surface, SurfaceDestroyer destroyer)
    -> terreate::Result<Surface> {
  constexpr auto operation = "adoptSurface";
  if (auto rejected = checkInstance(instance, operation))
    return std::move(*rejected);
  if (!surface)
    return fail(SurfaceError::invalid_surface, operation, "null surface handle");
  PFN_vkDestroySurfaceKHR default_destroy = nullptr;
  if (auto rejected = loadDefaultDestroy(instance, destroyer, operation, default_destroy))
    return std::move(*rejected);
  std::unique_ptr<Surface::Impl> impl;
  try {
    impl = std::make_unique<Surface::Impl>();
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(SurfaceError::allocation_failed)});
  }
  impl->instance = static_cast<VkInstance>(instance.nativeHandle());
  impl->surface = static_cast<VkSurfaceKHR>(surface);
  impl->destroyer = destroyer;
  impl->default_destroy = default_destroy;
  return Surface{std::move(impl)};
}

} // namespace terreate::graphics
