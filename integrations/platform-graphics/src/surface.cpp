#include <terreate/integration/platform_graphics/surface.hpp>

// The Graphics header brings in the Vulkan types GLFW needs for
// glfwCreateWindowSurface; it must come before the GLFW header.
#include <terreate/graphics/instance.hpp>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace terreate::integration::platform_graphics {
namespace {
class SurfaceErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.integration.platform_graphics"; }
  std::string message(int value) const override {
    switch (static_cast<SurfaceError>(value)) {
    case SurfaceError::parent_mismatch:
      return "the Window was not created by this Runtime";
    case SurfaceError::required_extension_not_enabled:
      return "the Instance did not enable an extension GLFW needs for window surfaces";
    }
    return "unknown platform-graphics surface error";
  }
};

terreate::Error fail(SurfaceError error, const char *operation, const std::string &detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return terreate::Error{make_error_code(error)};
  }
}

struct WindowCreator {
  GLFWwindow *window = nullptr;
};

// GLFW sets the handle to VK_NULL_HANDLE on failure, as the SurfaceCreator
// contract requires, and allows this call from any thread.
VkResult createFromWindow(VkInstance instance, VkSurfaceKHR *surface, void *user) {
  const auto *creator = static_cast<const WindowCreator *>(user);
  return glfwCreateWindowSurface(instance, creator->window, nullptr, surface);
}
} // namespace

const std::error_category &surface_error_category() noexcept {
  static const SurfaceErrorCategory category;
  return category;
}
std::error_code make_error_code(SurfaceError error) noexcept {
  return {static_cast<int>(error), surface_error_category()};
}

auto createWindowSurface(const graphics::Instance &instance, const platform::Runtime &runtime,
                         const platform::Window &window) -> terreate::Result<graphics::Surface> {
  constexpr auto operation = "createWindowSurface";
  if (!window.correlatedWith(runtime))
    return std::unexpected(
        fail(SurfaceError::parent_mismatch, operation, "Window and Runtime do not match"));

  // Thread-checked by the Runtime, and the list GLFW needs on this backend.
  const auto required = runtime.requiredInstanceExtensions();
  if (!required)
    return std::unexpected(required.error());

  // An invalid Instance is reported by graphics::createSurface below.
  if (instance.valid() && instance.plan() != nullptr) {
    const auto &enabled = instance.plan()->enabledExtensions();
    for (const auto &name : *required) {
      if (std::ranges::find(enabled, name) == enabled.end())
        return std::unexpected(fail(SurfaceError::required_extension_not_enabled, operation,
                                    "missing extension " + name));
    }
  }

  WindowCreator creator{window.nativeHandle()};
  return graphics::createSurface(instance, graphics::SurfaceCreator{&createFromWindow, &creator});
}

} // namespace terreate::integration::platform_graphics
