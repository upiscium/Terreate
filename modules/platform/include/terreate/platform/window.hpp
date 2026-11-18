#ifndef TERREATE_PLATFORM_WINDOW_HPP
#define TERREATE_PLATFORM_WINDOW_HPP

#include <cstdint>
#include <memory>
#include <string>

#include <terreate/core/result.hpp>
#include <terreate/platform/runtime.hpp>

// The native GLFW window type; the public headers never include GLFW.
struct GLFWwindow;

namespace terreate::platform {

/// A size in screen coordinates (window size) or pixels (framebuffer size).
struct WindowExtent {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool operator==(const WindowExtent &) const = default;
};

/// The ratio between the window's current DPI and the platform default.
struct ContentScale {
  float x = 1.0F;
  float y = 1.0F;
  bool operator==(const ContentScale &) const = default;
};

/// A window request. The window is always created without a client API
/// (GLFW_NO_API) so that Vulkan can present to it. Every hint is reset before
/// creation, so no setting carries over from an earlier window. The title is
/// UTF-8 and must not contain NUL; both sizes must be in 1..INT_MAX.
struct WindowDescription {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::string title{};
  bool resizable = true;
  bool visible = true;
  bool decorated = true;
  bool focused = true;
  bool operator==(const WindowDescription &) const = default;
};

/// The request as given, and the effective description after creation: the
/// window manager may choose another size, and GLFW may ignore hints the
/// backend cannot honor.
struct WindowConfiguration {
  WindowDescription requested{};
  WindowDescription effective{};
};

/// A move-only owner of one GLFW window. See Runtime for the thread and
/// destruction-order contract. Every query is checked against the Runtime's
/// thread and fails with wrong_thread elsewhere, or invalid_window for a
/// moved-from Window, before any GLFW call. Events are not processed here.
class Window final {
public:
  Window() = delete;
  Window(const Window &) = delete;
  Window &operator=(const Window &) = delete;
  Window(Window &&other) noexcept;
  Window &operator=(Window &&other) noexcept;
  ~Window();

  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] bool valid() const noexcept { return implementation_ != nullptr; }
  [[nodiscard]] bool correlatedWith(const Runtime &runtime) const noexcept;
  [[nodiscard]] const WindowConfiguration *configuration() const noexcept;

  /// The borrowed native handle: the escape hatch for GLFW calls Terreate
  /// does not wrap. Never destroy it; it is null for a moved-from Window.
  [[nodiscard]] GLFWwindow *nativeHandle() const noexcept;

  /// Window size in screen coordinates (glfwGetWindowSize).
  [[nodiscard]] auto size() const -> terreate::Result<WindowExtent>;
  /// Framebuffer size in pixels (glfwGetFramebufferSize), the extent a
  /// swapchain needs.
  [[nodiscard]] auto framebufferSize() const -> terreate::Result<WindowExtent>;
  [[nodiscard]] auto contentScale() const -> terreate::Result<ContentScale>;
  /// The close flag. GLFW sets it when the user asks to close the window, but
  /// only while the application processes events (#348).
  [[nodiscard]] auto shouldClose() const -> terreate::Result<bool>;
  [[nodiscard]] auto setShouldClose(bool value) const -> terreate::Result<void>;

  struct Impl;

private:
  explicit Window(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_{};

  friend auto createWindow(const Runtime &runtime, const WindowDescription &description)
      -> terreate::Result<Window>;
};

/// Create a window owned by the caller. `runtime` is borrowed only during the
/// call, but the Window must be destroyed before it.
[[nodiscard]] auto createWindow(const Runtime &runtime, const WindowDescription &description)
    -> terreate::Result<Window>;

} // namespace terreate::platform

#endif // TERREATE_PLATFORM_WINDOW_HPP
