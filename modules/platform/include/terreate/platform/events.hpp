#ifndef TERREATE_PLATFORM_EVENTS_HPP
#define TERREATE_PLATFORM_EVENTS_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace terreate::platform {

/// A Window's identity in events. Values are unique for the life of the
/// process and never reused, so an event stays meaningful after its Window is
/// destroyed. Zero is never a valid id.
struct WindowId {
  std::uint64_t value = 0;
  [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const WindowId &) const = default;
};

/// The user asked to close the window. GLFW sets the Window's close flag
/// itself before Terreate records this event; Terreate does not change it.
struct CloseRequested {
  bool operator==(const CloseRequested &) const = default;
};
/// New window size in screen coordinates.
struct WindowResized {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool operator==(const WindowResized &) const = default;
};
/// New framebuffer size in pixels: the extent a swapchain needs.
struct FramebufferResized {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool operator==(const FramebufferResized &) const = default;
};
struct ContentScaleChanged {
  float x = 1.0F;
  float y = 1.0F;
  bool operator==(const ContentScaleChanged &) const = default;
};
struct FocusChanged {
  bool focused = false;
  bool operator==(const FocusChanged &) const = default;
};
struct IconifiedChanged {
  bool iconified = false;
  bool operator==(const IconifiedChanged &) const = default;
};
struct MaximizedChanged {
  bool maximized = false;
  bool operator==(const MaximizedChanged &) const = default;
};
/// New position of the window's content area, in screen coordinates.
struct WindowMoved {
  std::int32_t x = 0;
  std::int32_t y = 0;
  bool operator==(const WindowMoved &) const = default;
};
/// The window's contents need to be drawn again.
struct RefreshRequested {
  bool operator==(const RefreshRequested &) const = default;
};

/// The state change of a key or button. The values equal GLFW's.
enum class InputAction : std::uint8_t {
  release = 0,
  press = 1,
  repeat = 2,
};

/// A raw physical key event (#296 builds on it). `key` and `mods` are GLFW's
/// values passed through unchanged (GLFW_KEY_*, GLFW_MOD_* bits); `scancode`
/// is the platform-specific scancode. Text input is CharacterInput, not this.
struct KeyInput {
  std::int32_t key = 0;
  std::int32_t scancode = 0;
  InputAction action = InputAction::release;
  std::int32_t mods = 0;
  bool operator==(const KeyInput &) const = default;
};
/// A raw mouse button event; `button` is GLFW_MOUSE_BUTTON_*.
struct MouseButtonInput {
  std::int32_t button = 0;
  InputAction action = InputAction::release;
  std::int32_t mods = 0;
  bool operator==(const MouseButtonInput &) const = default;
};
/// Cursor position relative to the content area, in screen coordinates.
struct CursorMoved {
  double x = 0.0;
  double y = 0.0;
  bool operator==(const CursorMoved &) const = default;
};
struct CursorEntered {
  bool entered = false;
  bool operator==(const CursorEntered &) const = default;
};
struct Scrolled {
  double x = 0.0;
  double y = 0.0;
  bool operator==(const Scrolled &) const = default;
};
/// One Unicode code point of text input. IME composition is #298.
struct CharacterInput {
  char32_t codepoint = 0;
  bool operator==(const CharacterInput &) const = default;
};

using WindowEventData =
    std::variant<CloseRequested, WindowResized, FramebufferResized, ContentScaleChanged,
                 FocusChanged, IconifiedChanged, MaximizedChanged, WindowMoved, RefreshRequested,
                 KeyInput, MouseButtonInput, CursorMoved, CursorEntered, Scrolled, CharacterInput>;

struct WindowEvent {
  WindowId window{};
  WindowEventData data{};
  bool operator==(const WindowEvent &) const = default;
};

/// Everything collected since the previous poll or wait, in arrival order.
/// `dropped` counts events that could not be stored (allocation failure or an
/// action value GLFW does not define); none is lost silently.
struct EventBatch {
  std::vector<WindowEvent> events{};
  std::size_t dropped = 0;
};

} // namespace terreate::platform

#endif // TERREATE_PLATFORM_EVENTS_HPP
