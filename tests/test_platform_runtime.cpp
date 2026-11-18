// #347: explicit GLFW Runtime and Window ownership.
//
// `null` mode runs on GLFW's null backend, which needs no display, so it runs
// everywhere. `native` mode uses the automatic backend and reports SKIP (never
// PASS) when no display is configured. The test wraps glfwCreateWindow
// (-Wl,--wrap) to count native window creations; GLFW is otherwise called
// directly only to trigger an error through the native handle.
#include <terreate/platform/runtime.hpp>
#include <terreate/platform/window.hpp>

#if defined(TERREATE_TEST_WITH_GRAPHICS)
#include <terreate/graphics/instance.hpp>
#endif

#include <GLFW/glfw3.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::platform;
using Error = PlatformError;

const char *test_name = "platform.runtime";
int failures = 0;
std::size_t created_windows = 0;

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

bool is(const terreate::Result<bool> &result, bool value) {
  return result.has_value() && *result == value;
}

template <typename T> bool hasError(const terreate::Result<T> &result, Error error) {
  return !result && result.error().code() == make_error_code(error);
}

std::size_t previous_callback_calls = 0;
// When set, the earlier callback causes another GLFW error each time it runs,
// which would recurse without the Runtime's nesting bound.
GLFWwindow *reentrant_window = nullptr;
void previousCallback(int /*code*/, const char * /*description*/) {
  ++previous_callback_calls;
  if (reentrant_window != nullptr)
    (void)glfwGetInputMode(reentrant_window, 0);
}

struct EventRecorder {
  std::vector<terreate::DiagnosticEvent> events{};
  bool dropped = false;
  // When set, the sink itself causes another GLFW error for every event.
  GLFWwindow *reentrant_window = nullptr;
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    try {
      events.push_back(event);
    } catch (...) {
      dropped = true;
    }
    if (reentrant_window != nullptr)
      (void)glfwGetInputMode(reentrant_window, 0);
  }
};
} // namespace

// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" GLFWwindow *__real_glfwCreateWindow(int, int, const char *, GLFWmonitor *, GLFWwindow *);
extern "C" GLFWwindow *__wrap_glfwCreateWindow(int width, int height, const char *title,
                                               GLFWmonitor *monitor, GLFWwindow *share) {
  ++created_windows;
  return __real_glfwCreateWindow(width, height, title, monitor, share);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
// The application hands the Platform's extension list to the Graphics
// Instance request itself; Platform does not depend on Graphics. Prints
// "<test>: vulkan-instance=PASS" or "=SKIP reason=...".
void vulkanInstanceHandOff(const Runtime &runtime) {
  const auto supported = runtime.vulkanSupported();
  if (!check(supported.has_value(), "vulkanSupported on the Runtime thread"))
    return;
  const auto extensions = runtime.requiredInstanceExtensions();
  if (!extensions) {
    // GLFW names no surface extensions: no loader, or a backend without
    // surfaces (GLFW 3.4's null backend). The failure must be explicit.
    check(hasError(extensions, Error::vulkan_unavailable),
          "without surface extensions, the query fails with vulkan_unavailable");
    std::printf("%s: vulkan-instance=SKIP reason=GLFW names no surface extensions "
                "(loader found: %s)\n",
                test_name, *supported ? "yes" : "no");
    return;
  }
  if (!check(*supported && !extensions->empty() &&
                 std::ranges::find(*extensions, "VK_KHR_surface") != extensions->end(),
             "a surface extension list implies a loader and includes VK_KHR_surface"))
    return;
#if defined(TERREATE_TEST_WITH_GRAPHICS)
  const auto capabilities = terreate::graphics::queryInstanceCapabilities();
  if (!check(capabilities.has_value(), "Vulkan loader capability query"))
    return;
  terreate::graphics::InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-platform-runtime";
  description.required_extensions = *extensions;
  const auto plan = terreate::graphics::resolveInstance(description, *capabilities);
  if (!check(plan.has_value(), "the Platform's extensions resolve as required extensions"))
    return;
  for (const auto &name : *extensions)
    check(std::ranges::find(plan->enabledExtensions(), name) != plan->enabledExtensions().end(),
          "every Platform extension is enabled in the effective plan");
  const auto instance = terreate::graphics::createInstance(*plan);
  if (!check(instance.has_value(), "Instance with the Platform's surface extensions"))
    return;
  std::printf("%s: vulkan-instance=PASS\n", test_name);
#else
  std::printf("%s: vulkan-instance=SKIP reason=Graphics not built\n", test_name);
#endif
}

int nullTest() {
  static_assert(!std::is_copy_constructible_v<Runtime> &&
                std::is_nothrow_move_constructible_v<Runtime>);
  // Assigning over a Runtime would terminate GLFW under its live Windows.
  static_assert(!std::is_move_assignable_v<Runtime>);
  static_assert(!std::is_copy_constructible_v<Window> &&
                std::is_nothrow_move_constructible_v<Window>);

  // A callback installed before the Runtime must keep working and come back.
  glfwSetErrorCallback(&previousCallback);
  EventRecorder recorder;
  auto runtime = createRuntime({.backend = PlatformBackend::null},
                               terreate::DiagnosticSinkView::bind(recorder));
  if (!runtime)
    return blocked("null-backend Runtime (GLFW 3.4)");
  check(runtime->valid() && runtime->backend() == PlatformBackend::null &&
            runtime->requested().backend == PlatformBackend::null &&
            runtime->liveWindowCount() == 0,
        "the Runtime reports the requested and effective null backend");
  check(hasError(createRuntime({.backend = PlatformBackend::null}), Error::already_initialized),
        "a second live Runtime is refused");

  vulkanInstanceHandOff(*runtime);

  // Invalid descriptions are rejected before GLFW creates anything.
  const auto created_before = created_windows;
  check(
      hasError(createWindow(*runtime, {.width = 0, .height = 10}), Error::invalid_description) &&
          hasError(createWindow(*runtime, {.width = 10, .height = 0}),
                   Error::invalid_description) &&
          hasError(createWindow(*runtime,
                                {.width = static_cast<std::uint32_t>(INT_MAX) + 1U, .height = 10}),
                   Error::invalid_description) &&
          hasError(createWindow(*runtime,
                                {.width = 10, .height = 10, .title = std::string{"bad\0title", 9}}),
                   Error::invalid_description),
      "zero, oversized and NUL-titled descriptions are rejected");
  check(created_windows == created_before, "no native window for a rejected description");

  // Two windows on one Runtime.
  const WindowDescription first_description{.width = 640, .height = 480, .title = "first"};
  const WindowDescription second_description{
      .width = 320, .height = 200, .title = "second", .resizable = false};
  auto first = createWindow(*runtime, first_description);
  auto second = createWindow(*runtime, second_description);
  if (!first || !second)
    return blocked("two null-backend Windows");
  check(created_windows == created_before + 2 && runtime->liveWindowCount() == 2,
        "two native windows are created and counted");
  check(first->nativeHandle() != nullptr && second->nativeHandle() != nullptr &&
            first->nativeHandle() != second->nativeHandle() && first->correlatedWith(*runtime) &&
            second->correlatedWith(*runtime),
        "each Window has its own native handle and belongs to the Runtime");
  const auto first_size = first->size();
  const auto first_framebuffer = first->framebufferSize();
  const auto first_scale = first->contentScale();
  check(first_size && *first_size == WindowExtent{640, 480}, "window size is observed");
  check(first_framebuffer && first_framebuffer->width > 0 && first_framebuffer->height > 0,
        "framebuffer size is observed");
  check(first_scale && first_scale->x > 0.0F && first_scale->y > 0.0F, "content scale is observed");
  const auto *configuration = second->configuration();
  check(configuration != nullptr && configuration->requested == second_description &&
            configuration->effective.width == 320 && configuration->effective.height == 200 &&
            !configuration->effective.resizable && configuration->effective.title == "second",
        "requested and effective configuration are observable");

  // The close flag belongs to each window.
  check(is(first->shouldClose(), false) && first->setShouldClose(true).has_value() &&
            is(first->shouldClose(), true) && is(second->shouldClose(), false),
        "the close flag is set and read per window");

  // A GLFW error reaches the Runtime's sink as a PLATFORM event.
  const auto events_before = recorder.events.size();
  const auto previous_before = previous_callback_calls;
  (void)glfwGetInputMode(first->nativeHandle(), 0);
  if (check(!recorder.dropped && recorder.events.size() == events_before + 1,
            "one GLFW error event")) {
    const auto &event = recorder.events.back();
    check(event.severity == terreate::DiagnosticSeverity::error &&
              event.categories == std::vector<std::string>{"PLATFORM"} && event.source == "glfw" &&
              event.code && event.code->name == "GLFW_INVALID_ENUM" &&
              event.code->value == GLFW_INVALID_ENUM && !event.message.empty(),
          "the event carries severity, category, source, code and message");
  }
  check(previous_callback_calls == previous_before + 1,
        "the callback installed before the Runtime still receives the error");

  // Errors raised while delivering an error are bounded: the nested one
  // reaches the sink once and is not passed to the earlier callback again.
  const auto nested_events_before = recorder.events.size();
  const auto nested_previous_before = previous_callback_calls;
  reentrant_window = first->nativeHandle();
  (void)glfwGetInputMode(first->nativeHandle(), 0);
  reentrant_window = nullptr;
  check(recorder.events.size() == nested_events_before + 2 &&
            previous_callback_calls == nested_previous_before + 1,
        "a nested error reaches the sink once more and does not recurse");
  // The same bound holds when the sink is the one that fails again.
  const auto sink_events_before = recorder.events.size();
  const auto sink_previous_before = previous_callback_calls;
  recorder.reentrant_window = first->nativeHandle();
  (void)glfwGetInputMode(first->nativeHandle(), 0);
  recorder.reentrant_window = nullptr;
  check(recorder.events.size() == sink_events_before + 2 &&
            previous_callback_calls == sink_previous_before + 1,
        "an error raised by the sink is delivered once more and does not recurse");
  // The error the native call left behind does not leak into the next query.
  check(first->size().has_value(), "a later query is not failed by an earlier native error");

  // Every call from another thread fails before touching GLFW.
  bool other_thread_ok = false;
  std::thread other([&] {
    other_thread_ok = hasError(runtime->vulkanSupported(), Error::wrong_thread) &&
                      hasError(runtime->requiredInstanceExtensions(), Error::wrong_thread) &&
                      hasError(first->size(), Error::wrong_thread) &&
                      hasError(first->framebufferSize(), Error::wrong_thread) &&
                      hasError(first->contentScale(), Error::wrong_thread) &&
                      hasError(first->shouldClose(), Error::wrong_thread) &&
                      hasError(first->setShouldClose(false), Error::wrong_thread) &&
                      hasError(createWindow(*runtime, first_description), Error::wrong_thread);
  });
  other.join();
  check(other_thread_ok && created_windows == created_before + 2,
        "every Runtime and Window call on another thread fails with wrong_thread");
  check(is(first->shouldClose(), true), "the rejected call from another thread changed nothing");

  // Moves keep ownership and the count.
  Window moved = std::move(*second);
  check(!second->valid() && second->nativeHandle() == nullptr &&
            hasError(second->size(), Error::invalid_window) && moved.valid() &&
            runtime->liveWindowCount() == 2,
        "a moved-from Window is invalid and the count is unchanged");
  Runtime moved_runtime = std::move(*runtime);
  check(!runtime->valid() && hasError(runtime->vulkanSupported(), Error::invalid_runtime) &&
            moved_runtime.valid() && first->correlatedWith(moved_runtime) &&
            moved_runtime.liveWindowCount() == 2 && first->size().has_value(),
        "a moved Runtime keeps its windows");
  {
    Window destroyed = std::move(moved);
  }
  check(moved_runtime.liveWindowCount() == 1, "destroying a Window updates the count");
  {
    Window destroyed = std::move(*first);
  }
  check(moved_runtime.liveWindowCount() == 0, "every Window is destroyed before the Runtime");
  {
    Runtime released = std::move(moved_runtime);
  }
  check(glfwSetErrorCallback(nullptr) == &previousCallback,
        "destroying the Runtime restores the earlier error callback");
  auto again = createRuntime({.backend = PlatformBackend::null});
  check(again.has_value(), "a new Runtime is allowed after the previous one is destroyed");

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("platform.runtime: null-backend=PASS");
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
    std::puts("platform.native_window: native-window=SKIP reason=no DISPLAY or WAYLAND_DISPLAY");
    return EXIT_SUCCESS;
  }
  // With a display configured, a failure here is a real failure.
  auto runtime = createRuntime();
  if (!runtime)
    return blocked("automatic-backend Runtime with a configured display");
  check(runtime->backend() != PlatformBackend::null &&
            runtime->backend() != PlatformBackend::automatic,
        "the automatic request resolves to a real backend");
  auto window =
      createWindow(*runtime, {.width = 320, .height = 240, .title = "terreate", .visible = false});
  if (!window)
    return blocked("native Window");
  vulkanInstanceHandOff(*runtime);
  const auto size = window->size();
  const auto framebuffer = window->framebufferSize();
  const auto scale = window->contentScale();
  check(size && size->width > 0 && size->height > 0 && framebuffer && framebuffer->width > 0 &&
            framebuffer->height > 0 && scale && scale->x > 0.0F,
        "native window size, framebuffer size and content scale are observed");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("platform.native_window: native-window=PASS");
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
    test_name = "platform.native_window";
    return nativeTest();
  }
  return blocked("unknown mode");
}
