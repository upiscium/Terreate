// #363 part 2: creating a Vulkan Surface from a Platform Window.
//
// `null` mode runs on GLFW's null backend, which needs no display and names no
// surface extensions, so it checks the explicit error paths: a surface is
// never reported as created when it could not be. `native` mode uses the
// automatic backend with a real Window, and reports SKIP (never PASS) when no
// display is configured or the Vulkan loader lacks the window-system
// extensions. No linker- or platform-specific mechanism is used.
#include <terreate/integration/platform_graphics/surface.hpp>

#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/platform/runtime.hpp>
#include <terreate/platform/window.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace integration = terreate::integration::platform_graphics;
namespace graphics = terreate::graphics;
namespace platform = terreate::platform;

const char *test_name = "integration.platform_graphics";
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

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  for (const auto &name : names)
    if (name == wanted)
      return true;
  return false;
}

int nullTest() {
  static_assert(std::is_same_v<decltype(integration::createWindowSurface(
                                   std::declval<const graphics::Instance &>(),
                                   std::declval<const platform::Runtime &>(),
                                   std::declval<const platform::Window &>())),
                               terreate::Result<graphics::Surface>>);
  check(std::string{integration::surface_error_category().name()} ==
                "terreate.integration.platform_graphics" &&
            make_error_code(integration::SurfaceError::parent_mismatch).message() ==
                "the Window was not created by this Runtime",
        "error category name and message");

  const auto capabilities = graphics::queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  graphics::InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-integration-null";
  const auto plan = graphics::resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance");
  auto instance = graphics::createInstance(*plan);
  if (!instance)
    return blocked("Vulkan Instance");

  auto runtime = platform::createRuntime({.backend = platform::PlatformBackend::null});
  if (!runtime)
    return blocked("null-backend Runtime (GLFW 3.4)");
  auto window = platform::createWindow(*runtime, {.width = 64, .height = 48, .title = "null"});
  if (!window)
    return blocked("null-backend Window");

  // The null backend names no surface extensions: an explicit Platform error,
  // never a silent success and never a surface.
  const auto unavailable = integration::createWindowSurface(*instance, *runtime, *window);
  check(hasError(unavailable, platform::PlatformError::vulkan_unavailable),
        "without surface extensions the Platform's vulkan_unavailable is returned unchanged");

  // From another thread the Runtime thread rule applies, before any native call.
  bool other_thread_ok = false;
  std::thread other([&] {
    other_thread_ok = hasError(integration::createWindowSurface(*instance, *runtime, *window),
                               platform::PlatformError::wrong_thread);
  });
  other.join();
  check(other_thread_ok, "another thread gets the Platform's wrong_thread");

  // The Window must belong to the Runtime. This check comes first, so it holds
  // even though this backend could not create a surface anyway.
  platform::Window moved_window = std::move(*window);
  check(hasError(integration::createWindowSurface(*instance, *runtime, *window),
                 integration::SurfaceError::parent_mismatch),
        "a moved-from Window is a parent_mismatch");
  check(hasError(integration::createWindowSurface(*instance, *runtime, moved_window),
                 platform::PlatformError::vulkan_unavailable),
        "the moved-to Window is accepted and reaches the extension query");
  platform::Runtime moved_runtime = std::move(*runtime);
  check(hasError(integration::createWindowSurface(*instance, *runtime, moved_window),
                 integration::SurfaceError::parent_mismatch),
        "a moved-from Runtime is a parent_mismatch");
  check(moved_window.correlatedWith(moved_runtime) &&
            hasError(integration::createWindowSurface(*instance, moved_runtime, moved_window),
                     platform::PlatformError::vulkan_unavailable),
        "a moved Runtime still matches its Window");

  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("integration.platform_graphics: null-integration=PASS");
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
    std::puts("integration.native_platform_graphics: native-integration=SKIP "
              "reason=no DISPLAY or WAYLAND_DISPLAY");
    return EXIT_SUCCESS;
  }
  // With a display configured, a failure here is a real failure.
  auto runtime = platform::createRuntime();
  if (!runtime)
    return blocked("automatic-backend Runtime with a configured display");
  auto window =
      platform::createWindow(*runtime, {.width = 320, .height = 240, .title = "terreate"});
  if (!window)
    return blocked("native Window");
  const auto extensions = runtime->requiredInstanceExtensions();
  if (!extensions) {
    std::puts("integration.native_platform_graphics: native-integration=SKIP "
              "reason=GLFW names no surface extensions");
    return EXIT_SUCCESS;
  }

  const auto capabilities = graphics::queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  for (const auto &name : *extensions) {
    if (!hasName(capabilities->available_extensions, name)) {
      std::printf("integration.native_platform_graphics: native-integration=SKIP "
                  "reason=the Vulkan loader lacks %s\n",
                  name.c_str());
      return EXIT_SUCCESS;
    }
  }
  const bool has_validation =
      hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation");
  const bool has_debug_utils =
      hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  // An Instance that did not enable what GLFW needs is refused with the name
  // of the missing extension, before the native call.
  {
    graphics::InstanceDescription plain_description;
    plain_description.api_version = VK_API_VERSION_1_3;
    plain_description.application_name = "terreate-integration-plain";
    const auto plain_plan = graphics::resolveInstance(plain_description, *capabilities);
    if (!plain_plan)
      return blocked("Vulkan 1.3 Instance without surface extensions");
    auto plain = graphics::createInstance(*plain_plan);
    if (!plain)
      return blocked("plain Vulkan Instance");
    const auto refused = integration::createWindowSurface(*plain, *runtime, *window);
    check(hasError(refused, integration::SurfaceError::required_extension_not_enabled) &&
              refused.error().detail().find(extensions->front()) != std::string::npos,
          "an Instance without GLFW's extensions is refused, naming the missing one");
  }

  graphics::InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-integration";
  description.required_extensions = *extensions;
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_validation && has_debug_utils)
    description.debug_utils = graphics::DebugUtilsMode::required;
  const auto plan = graphics::resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance with GLFW's extensions");
  ValidationRecorder recorder;
  auto instance = graphics::createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance with GLFW's extensions");

  // No logical Device is created anywhere in this test: the Surface exists
  // before it. Order of destruction: Surface, then Window, then Instance.
  {
    auto surface = integration::createWindowSurface(*instance, *runtime, *window);
    if (!check(surface.has_value(), "createWindowSurface with a real Window"))
      return EXIT_FAILURE;
    check(surface->valid() && surface->nativeSurface() != VK_NULL_HANDLE &&
              surface->correlatedWith(*instance),
          "the Surface is valid and belongs to the Instance");
  }
  if (has_validation) {
    check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
    std::puts("integration.native_platform_graphics: validation-layer=PASS");
  } else {
    std::puts("integration.native_platform_graphics: validation-layer=SKIP "
              "reason=VK_LAYER_KHRONOS_validation unavailable");
  }
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("integration.native_platform_graphics: native-integration=PASS");
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
    test_name = "integration.native_platform_graphics";
    return nativeTest();
  }
  return blocked("unknown mode");
}
