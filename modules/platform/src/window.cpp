#include <terreate/platform/window.hpp>

#include "runtime_impl.hpp"

#include <GLFW/glfw3.h>

#include <atomic>
#include <climits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace terreate::platform {

// Heap-owned, so its address is stable across Window moves: it is the GLFW
// user pointer that Terreate's callbacks use to find the window and Runtime.
struct Window::Impl {
  GLFWwindow *window = nullptr;
  Runtime::Impl *runtime = nullptr;
  WindowConfiguration configuration{};
  WindowId id{};

  Impl() = default;
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  ~Impl() {
    if (window != nullptr) {
      // GLFW calls no callback for a destroyed window; clear the pointer too,
      // so nothing can reach this Impl afterwards.
      glfwSetWindowUserPointer(window, nullptr);
      glfwDestroyWindow(window);
      --runtime->live_windows;
    }
  }
};

namespace {
// Never reused for the life of the process, across Runtimes too.
std::atomic<std::uint64_t> next_window_id{1};

// Store one event for the next poll or wait. Runs inside GLFW on the Runtime
// thread, so it must not throw; an event that cannot be stored is counted.
void store(GLFWwindow *window, WindowEventData data) noexcept {
  auto *impl = static_cast<Window::Impl *>(glfwGetWindowUserPointer(window));
  if (impl == nullptr)
    return;
  try {
    impl->runtime->pending_events.push_back(WindowEvent{impl->id, data});
  } catch (const std::bad_alloc &) {
    ++impl->runtime->dropped_events;
  }
}

void countDropped(GLFWwindow *window) noexcept {
  if (auto *impl = static_cast<Window::Impl *>(glfwGetWindowUserPointer(window)))
    ++impl->runtime->dropped_events;
}

std::optional<InputAction> inputAction(int action) noexcept {
  switch (action) {
  case GLFW_RELEASE:
    return InputAction::release;
  case GLFW_PRESS:
    return InputAction::press;
  case GLFW_REPEAT:
    return InputAction::repeat;
  default:
    return std::nullopt;
  }
}

std::uint32_t nonNegative(int value) noexcept {
  return static_cast<std::uint32_t>(value < 0 ? 0 : value);
}

void onClose(GLFWwindow *window) noexcept { store(window, CloseRequested{}); }
void onSize(GLFWwindow *window, int width, int height) noexcept {
  store(window, WindowResized{nonNegative(width), nonNegative(height)});
}
void onFramebufferSize(GLFWwindow *window, int width, int height) noexcept {
  store(window, FramebufferResized{nonNegative(width), nonNegative(height)});
}
void onContentScale(GLFWwindow *window, float x, float y) noexcept {
  store(window, ContentScaleChanged{x, y});
}
void onFocus(GLFWwindow *window, int focused) noexcept {
  store(window, FocusChanged{focused == GLFW_TRUE});
}
void onIconify(GLFWwindow *window, int iconified) noexcept {
  store(window, IconifiedChanged{iconified == GLFW_TRUE});
}
void onMaximize(GLFWwindow *window, int maximized) noexcept {
  store(window, MaximizedChanged{maximized == GLFW_TRUE});
}
void onPosition(GLFWwindow *window, int x, int y) noexcept { store(window, WindowMoved{x, y}); }
void onRefresh(GLFWwindow *window) noexcept { store(window, RefreshRequested{}); }
// GLFW fixes these callback signatures.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void onKey(GLFWwindow *window, int key, int scancode, int action, int mods) noexcept {
  const auto mapped = inputAction(action);
  if (!mapped) {
    countDropped(window);
    return;
  }
  store(window, KeyInput{key, scancode, *mapped, mods});
}
void onMouseButton(GLFWwindow *window, int button, int action, int mods) noexcept {
  const auto mapped = inputAction(action);
  if (!mapped) {
    countDropped(window);
    return;
  }
  store(window, MouseButtonInput{button, *mapped, mods});
}
// NOLINTEND(bugprone-easily-swappable-parameters)
void onCursorPosition(GLFWwindow *window, double x, double y) noexcept {
  store(window, CursorMoved{x, y});
}
void onCursorEnter(GLFWwindow *window, int entered) noexcept {
  store(window, CursorEntered{entered == GLFW_TRUE});
}
void onScroll(GLFWwindow *window, double x, double y) noexcept { store(window, Scrolled{x, y}); }
void onCharacter(GLFWwindow *window, unsigned int codepoint) noexcept {
  store(window, CharacterInput{static_cast<char32_t>(codepoint)});
}

// Terreate reserves the user pointer and these callbacks (documented).
void registerCallbacks(GLFWwindow *window, Window::Impl *impl) noexcept {
  glfwSetWindowUserPointer(window, impl);
  glfwSetWindowCloseCallback(window, &onClose);
  glfwSetWindowSizeCallback(window, &onSize);
  glfwSetFramebufferSizeCallback(window, &onFramebufferSize);
  glfwSetWindowContentScaleCallback(window, &onContentScale);
  glfwSetWindowFocusCallback(window, &onFocus);
  glfwSetWindowIconifyCallback(window, &onIconify);
  glfwSetWindowMaximizeCallback(window, &onMaximize);
  glfwSetWindowPosCallback(window, &onPosition);
  glfwSetWindowRefreshCallback(window, &onRefresh);
  glfwSetKeyCallback(window, &onKey);
  glfwSetMouseButtonCallback(window, &onMouseButton);
  glfwSetCursorPosCallback(window, &onCursorPosition);
  glfwSetCursorEnterCallback(window, &onCursorEnter);
  glfwSetScrollCallback(window, &onScroll);
  glfwSetCharCallback(window, &onCharacter);
}

// Drop any GLFW error left by an earlier call, so a failure check after the
// next call sees only that call's error. Dropped errors already reached the
// Runtime's sink through the error callback.
void clearGlfwError() noexcept { glfwGetError(nullptr); }

terreate::Result<void> afterCall(const char *operation) {
  const char *description = nullptr;
  const int code = glfwGetError(&description);
  if (code == GLFW_NO_ERROR)
    return {};
  return std::unexpected(detail::glfwError(code, operation, description));
}

terreate::Result<void> checkWindow(const Window::Impl *impl, const char *operation) {
  if (impl == nullptr || impl->window == nullptr)
    return std::unexpected(
        detail::platformError(PlatformError::invalid_window, operation, nullptr));
  return detail::checkThread(impl->runtime, operation);
}

bool validSize(std::uint32_t value) noexcept {
  return value != 0 && value <= static_cast<std::uint32_t>(INT_MAX);
}

WindowExtent extent(int width, int height) noexcept {
  return {static_cast<std::uint32_t>(width < 0 ? 0 : width),
          static_cast<std::uint32_t>(height < 0 ? 0 : height)};
}
} // namespace

auto createWindow(const Runtime &runtime, const WindowDescription &description)
    -> terreate::Result<Window> {
  constexpr auto operation = "createWindow";
  if (auto checked = detail::checkThread(runtime.implementation_.get(), operation); !checked)
    return std::unexpected(std::move(checked.error()));
  if (!validSize(description.width) || !validSize(description.height))
    return std::unexpected(detail::platformError(PlatformError::invalid_description, operation,
                                                 "width and height must be in 1..INT_MAX"));
  if (description.title.find('\0') != std::string::npos)
    return std::unexpected(
        detail::platformError(PlatformError::invalid_description, operation, "title contains NUL"));

  std::unique_ptr<Window::Impl> impl;
  try {
    impl = std::make_unique<Window::Impl>();
    impl->configuration.requested = description;
    impl->configuration.effective = description;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(PlatformError::allocation_failed)});
  }

  // Hints are process-global: start from the defaults so nothing carries over
  // from an earlier window, and restore them afterwards for native users.
  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, description.resizable ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_VISIBLE, description.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_DECORATED, description.decorated ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_FOCUSED, description.focused ? GLFW_TRUE : GLFW_FALSE);
  clearGlfwError();
  GLFWwindow *window =
      glfwCreateWindow(static_cast<int>(description.width), static_cast<int>(description.height),
                       description.title.c_str(), nullptr, nullptr);
  glfwDefaultWindowHints();
  if (window == nullptr)
    return std::unexpected(detail::takeGlfwError(PlatformError::native_failure, operation));

  impl->window = window;
  impl->runtime = runtime.implementation_.get();
  impl->id = WindowId{next_window_id.fetch_add(1, std::memory_order_relaxed)};
  ++impl->runtime->live_windows;
  registerCallbacks(window, impl.get());

  // The effective configuration is part of the result, so its reads are
  // checked like any query. On failure `impl` destroys the new window.
  clearGlfwError();
  int width = 0;
  int height = 0;
  glfwGetWindowSize(window, &width, &height);
  auto &effective = impl->configuration.effective;
  const auto size = extent(width, height);
  effective.width = size.width;
  effective.height = size.height;
  effective.resizable = glfwGetWindowAttrib(window, GLFW_RESIZABLE) == GLFW_TRUE;
  effective.visible = glfwGetWindowAttrib(window, GLFW_VISIBLE) == GLFW_TRUE;
  effective.decorated = glfwGetWindowAttrib(window, GLFW_DECORATED) == GLFW_TRUE;
  effective.focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;
  if (auto called = afterCall(operation); !called)
    return std::unexpected(std::move(called.error()));
  return Window{std::move(impl)};
}

Window::Window(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Window::Window(Window &&other) noexcept = default;
Window &Window::operator=(Window &&other) noexcept = default;
Window::~Window() = default;

bool Window::correlatedWith(const Runtime &runtime) const noexcept {
  return valid() && runtime.valid() && implementation_->runtime == runtime.implementation_.get();
}

WindowId Window::id() const noexcept {
  return implementation_ == nullptr ? WindowId{} : implementation_->id;
}

const WindowConfiguration *Window::configuration() const noexcept {
  return implementation_ == nullptr ? nullptr : &implementation_->configuration;
}

GLFWwindow *Window::nativeHandle() const noexcept {
  return implementation_ == nullptr ? nullptr : implementation_->window;
}

auto Window::size() const -> terreate::Result<WindowExtent> {
  constexpr auto operation = "Window::size";
  if (auto checked = checkWindow(implementation_.get(), operation); !checked)
    return std::unexpected(std::move(checked.error()));
  clearGlfwError();
  int width = 0;
  int height = 0;
  glfwGetWindowSize(implementation_->window, &width, &height);
  if (auto called = afterCall(operation); !called)
    return std::unexpected(std::move(called.error()));
  return extent(width, height);
}

auto Window::framebufferSize() const -> terreate::Result<WindowExtent> {
  constexpr auto operation = "Window::framebufferSize";
  if (auto checked = checkWindow(implementation_.get(), operation); !checked)
    return std::unexpected(std::move(checked.error()));
  clearGlfwError();
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(implementation_->window, &width, &height);
  if (auto called = afterCall(operation); !called)
    return std::unexpected(std::move(called.error()));
  return extent(width, height);
}

auto Window::contentScale() const -> terreate::Result<ContentScale> {
  constexpr auto operation = "Window::contentScale";
  if (auto checked = checkWindow(implementation_.get(), operation); !checked)
    return std::unexpected(std::move(checked.error()));
  clearGlfwError();
  ContentScale scale{};
  glfwGetWindowContentScale(implementation_->window, &scale.x, &scale.y);
  if (auto called = afterCall(operation); !called)
    return std::unexpected(std::move(called.error()));
  return scale;
}

auto Window::shouldClose() const -> terreate::Result<bool> {
  if (auto checked = checkWindow(implementation_.get(), "Window::shouldClose"); !checked)
    return std::unexpected(std::move(checked.error()));
  return glfwWindowShouldClose(implementation_->window) == GLFW_TRUE;
}

auto Window::setShouldClose(bool value) const -> terreate::Result<void> {
  if (auto checked = checkWindow(implementation_.get(), "Window::setShouldClose"); !checked)
    return checked;
  glfwSetWindowShouldClose(implementation_->window, value ? GLFW_TRUE : GLFW_FALSE);
  return {};
}

} // namespace terreate::platform
