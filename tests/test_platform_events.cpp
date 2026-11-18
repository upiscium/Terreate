// #348: explicit event processing and the raw Window event surface.
//
// `null` mode runs on GLFW's null backend, which needs no display. GLFW has no
// API to inject input, so the test drives events with public GLFW only:
// - window events through native calls the null backend reports (it calls the
//   callbacks synchronously inside them);
// - input events by reading Terreate's registered callback back from the GLFW
//   setter (which returns the previous callback), calling it with synthetic
//   values, and restoring it. This runs the real translation path.
// `native` mode uses the automatic backend and reports SKIP (never PASS) when
// no display is configured. No linker- or platform-specific mechanism is used.
#include <terreate/platform/events.hpp>
#include <terreate/platform/runtime.hpp>
#include <terreate/platform/window.hpp>

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::platform;
using Error = PlatformError;

const char *test_name = "platform.events";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

template <typename T> bool hasError(const terreate::Result<T> &result, Error error) {
  return !result && result.error().code() == make_error_code(error);
}

// Events of one type, in order, for one window.
template <typename T> std::vector<T> of(const EventBatch &batch, WindowId window) {
  std::vector<T> found;
  for (const auto &event : batch.events)
    if (event.window == window)
      if (const auto *data = std::get_if<T>(&event.data))
        found.push_back(*data);
  return found;
}

// Read Terreate's callback back from a GLFW setter, call it, and restore it.
template <typename Callback, typename Setter, typename... Args>
bool invokeRegistered(GLFWwindow *window, Setter setter, Args... args) {
  const Callback registered = setter(window, nullptr);
  if (registered == nullptr)
    return false;
  registered(window, args...);
  setter(window, registered);
  return true;
}

int nullTest() {
  static_assert(std::is_nothrow_move_constructible_v<EventBatch>);
  auto runtime = createRuntime({.backend = PlatformBackend::null});
  if (!runtime)
    return blocked("null-backend Runtime (GLFW 3.4)");
  auto first = createWindow(*runtime, {.width = 100, .height = 80, .title = "first"});
  auto second = createWindow(*runtime, {.width = 60, .height = 40, .title = "second"});
  if (!first || !second)
    return blocked("two null-backend Windows");
  const WindowId first_id = first->id();
  const WindowId second_id = second->id();
  check(first_id && second_id && first_id != second_id, "each Window has its own nonzero id");

  // Start from an empty store.
  if (!check(runtime->pollEvents().has_value(), "initial poll"))
    return EXIT_FAILURE;
  const auto empty = runtime->pollEvents();
  check(empty && empty->events.empty() && empty->dropped == 0,
        "nothing is handed over twice and a quiet poll is empty");

  // Window events reported by the null backend inside native calls. They are
  // stored, and handed over only by the next poll.
  glfwSetWindowSize(first->nativeHandle(), 200, 150);
  glfwSetWindowPos(second->nativeHandle(), 10, 20);
  glfwIconifyWindow(first->nativeHandle());
  glfwRestoreWindow(first->nativeHandle());
  glfwMaximizeWindow(second->nativeHandle());
  glfwFocusWindow(first->nativeHandle());
  const auto window_events = runtime->pollEvents();
  if (!check(window_events.has_value(), "poll after native window changes"))
    return EXIT_FAILURE;
  check(window_events->dropped == 0, "no event dropped");
  check(of<WindowResized>(*window_events, first_id) == std::vector{WindowResized{200, 150}} &&
            of<FramebufferResized>(*window_events, first_id) ==
                std::vector{FramebufferResized{200, 150}},
        "resize reaches the first window as size and framebuffer events");
  check(of<WindowMoved>(*window_events, second_id) == std::vector{WindowMoved{10, 20}} &&
            of<WindowMoved>(*window_events, first_id).empty(),
        "a move reaches only the window that moved");
  check(of<IconifiedChanged>(*window_events, first_id) ==
            std::vector{IconifiedChanged{true}, IconifiedChanged{false}},
        "iconify and restore arrive in order");
  check(of<MaximizedChanged>(*window_events, second_id) == std::vector{MaximizedChanged{true}},
        "maximize reaches the second window");
  const auto focus = of<FocusChanged>(*window_events, first_id);
  check(!focus.empty() && focus.back() == FocusChanged{true}, "focus ends gained on the first");
  // Arrival order: the resize events come before the move event.
  std::optional<std::size_t> resize_at;
  std::optional<std::size_t> move_at;
  for (std::size_t index = 0; index < window_events->events.size(); ++index) {
    const auto &event = window_events->events[index];
    if (!resize_at && std::holds_alternative<WindowResized>(event.data))
      resize_at = index;
    if (!move_at && std::holds_alternative<WindowMoved>(event.data))
      move_at = index;
  }
  check(resize_at && move_at && *resize_at < *move_at, "events keep arrival order");

  // Raw input through Terreate's registered callbacks.
  GLFWwindow *native = first->nativeHandle();
  const bool invoked =
      invokeRegistered<GLFWkeyfun>(native, &glfwSetKeyCallback, GLFW_KEY_A, 38, GLFW_PRESS,
                                   GLFW_MOD_SHIFT) &&
      invokeRegistered<GLFWkeyfun>(native, &glfwSetKeyCallback, GLFW_KEY_A, 38, GLFW_REPEAT, 0) &&
      invokeRegistered<GLFWkeyfun>(native, &glfwSetKeyCallback, GLFW_KEY_A, 38, GLFW_RELEASE, 0) &&
      invokeRegistered<GLFWmousebuttonfun>(native, &glfwSetMouseButtonCallback,
                                           GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, GLFW_MOD_CONTROL) &&
      invokeRegistered<GLFWcursorposfun>(native, &glfwSetCursorPosCallback, 12.5, 7.25) &&
      invokeRegistered<GLFWcursorenterfun>(native, &glfwSetCursorEnterCallback, GLFW_TRUE) &&
      invokeRegistered<GLFWscrollfun>(native, &glfwSetScrollCallback, 0.0, -1.5) &&
      invokeRegistered<GLFWcharfun>(native, &glfwSetCharCallback, 0x3042U) &&
      invokeRegistered<GLFWwindowclosefun>(native, &glfwSetWindowCloseCallback) &&
      invokeRegistered<GLFWwindowrefreshfun>(native, &glfwSetWindowRefreshCallback) &&
      invokeRegistered<GLFWwindowcontentscalefun>(native, &glfwSetWindowContentScaleCallback, 2.0F,
                                                  1.5F) &&
      // An action GLFW does not define is counted, not stored.
      invokeRegistered<GLFWkeyfun>(native, &glfwSetKeyCallback, GLFW_KEY_B, 39, 7, 0);
  if (!check(invoked, "Terreate registered every callback the test reads back"))
    return EXIT_FAILURE;
  const auto input = runtime->pollEvents();
  if (!check(input.has_value(), "poll after input"))
    return EXIT_FAILURE;
  const std::vector<WindowEvent> expected{
      {first_id, KeyInput{GLFW_KEY_A, 38, InputAction::press, GLFW_MOD_SHIFT}},
      {first_id, KeyInput{GLFW_KEY_A, 38, InputAction::repeat, 0}},
      {first_id, KeyInput{GLFW_KEY_A, 38, InputAction::release, 0}},
      {first_id, MouseButtonInput{GLFW_MOUSE_BUTTON_RIGHT, InputAction::press, GLFW_MOD_CONTROL}},
      {first_id, CursorMoved{12.5, 7.25}},
      {first_id, CursorEntered{true}},
      {first_id, Scrolled{0.0, -1.5}},
      {first_id, CharacterInput{U'あ'}},
      {first_id, CloseRequested{}},
      {first_id, RefreshRequested{}},
      {first_id, ContentScaleChanged{2.0F, 1.5F}},
  };
  check(input->events == expected, "input events keep their values and arrival order");
  check(input->dropped == 1, "the undefined key action is counted as dropped");

  // GLFW sets the close flag itself, before it calls the close callback
  // (glfw3.h, glfwSetWindowCloseCallback), and has no API to simulate a user's
  // close request. What Terreate controls is its callback: it records the
  // request and leaves the flag exactly as GLFW and the application set it.
  bool flag_kept = true;
  for (const bool value : {false, true}) {
    flag_kept &= first->setShouldClose(value).has_value() &&
                 invokeRegistered<GLFWwindowclosefun>(native, &glfwSetWindowCloseCallback);
    const auto flag = first->shouldClose();
    flag_kept &= flag.has_value() && *flag == value;
  }
  const auto closes = runtime->pollEvents();
  check(flag_kept, "Terreate's close callback does not change the close flag");
  check(closes && of<CloseRequested>(*closes, first_id).size() == 2,
        "each close request is recorded as an event");

  // Calls from another thread: poll and wait are refused; post is allowed.
  bool other_ok = false;
  std::thread other([&] {
    other_ok = hasError(runtime->pollEvents(), Error::wrong_thread) &&
               hasError(runtime->waitEvents(), Error::wrong_thread) &&
               hasError(runtime->waitEvents(std::chrono::milliseconds{1}), Error::wrong_thread) &&
               runtime->postEmptyEvent().has_value();
  });
  other.join();
  check(other_ok, "poll and wait fail with wrong_thread elsewhere; postEmptyEvent works");

  // Timed wait and invalid timeouts.
  check(runtime->waitEvents(std::chrono::milliseconds{10}).has_value(),
        "a timed wait returns after its timeout");
  check(
      hasError(runtime->waitEvents(std::chrono::duration<double>{0.0}), Error::invalid_argument) &&
          hasError(runtime->waitEvents(std::chrono::duration<double>{-1.0}),
                   Error::invalid_argument) &&
          hasError(runtime->waitEvents(
                       std::chrono::duration<double>{std::numeric_limits<double>::infinity()}),
                   Error::invalid_argument) &&
          hasError(runtime->waitEvents(
                       std::chrono::duration<double>{std::numeric_limits<double>::quiet_NaN()}),
                   Error::invalid_argument),
      "zero, negative, infinite and NaN timeouts are rejected");

  // postEmptyEvent from another thread wakes a wait long before its timeout.
  const auto started = std::chrono::steady_clock::now();
  bool posted = false;
  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    posted = runtime->postEmptyEvent().has_value();
  });
  const auto woken = runtime->waitEvents(std::chrono::seconds{10});
  waker.join();
  check(posted && woken.has_value() &&
            std::chrono::steady_clock::now() - started < std::chrono::seconds{5},
        "postEmptyEvent from another thread wakes waitEvents");

  // Moves keep the id; events of a destroyed Window keep its id.
  Window moved = std::move(*second);
  check(moved.id() == second_id && !second->id(), "a moved Window keeps its id");
  glfwSetWindowPos(moved.nativeHandle(), 30, 40);
  {
    Window destroyed = std::move(moved);
  }
  const auto after_destroy = runtime->pollEvents();
  check(after_destroy &&
            of<WindowMoved>(*after_destroy, second_id) == std::vector{WindowMoved{30, 40}},
        "an event stored before its Window was destroyed is still handed over with its id");
  const auto later_window = createWindow(*runtime, {.width = 10, .height = 10});
  check(later_window && later_window->id() != first_id && later_window->id() != second_id,
        "ids are never reused");

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("platform.events: null-events=PASS");
  return EXIT_SUCCESS;
}

int nativeTest() {
  const char *display = std::getenv("DISPLAY");
  const char *wayland = std::getenv("WAYLAND_DISPLAY");
  const bool has_display =
      (display != nullptr && *display != '\0') || (wayland != nullptr && *wayland != '\0');
#if defined(_WIN32) || defined(__APPLE__)
  const bool needs_display_variable = false;
#else
  const bool needs_display_variable = true;
#endif
  if (needs_display_variable && !has_display) {
    std::puts("platform.native_events: native-events=SKIP reason=no DISPLAY or WAYLAND_DISPLAY");
    return EXIT_SUCCESS;
  }
  auto runtime = createRuntime();
  if (!runtime)
    return blocked("automatic-backend Runtime with a configured display");
  auto window = createWindow(*runtime, {.width = 320, .height = 240, .title = "terreate"});
  if (!window)
    return blocked("native Window");
  // A real window system reports the resize asynchronously; wait for it.
  glfwSetWindowSize(window->nativeHandle(), 400, 300);
  bool resized = false;
  for (int attempt = 0; attempt < 50 && !resized; ++attempt) {
    const auto batch = runtime->waitEvents(std::chrono::milliseconds{100});
    if (!check(batch.has_value(), "native wait"))
      return EXIT_FAILURE;
    for (const auto &event : batch->events)
      resized |= event.window == window->id() && std::holds_alternative<WindowResized>(event.data);
  }
  check(resized, "the window system reports the resize through waitEvents");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("platform.native_events: native-events=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify null or native mode");
  const std::string_view mode{argv[1]};
  if (mode == "null")
    return nullTest();
  if (mode == "native") {
    test_name = "platform.native_events";
    return nativeTest();
  }
  return blocked("unknown mode");
}
