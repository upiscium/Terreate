// #363: Graphics Surface ownership, with no Platform and no Device.
//
// Surfaces come from VK_EXT_headless_surface through the SurfaceCreator escape
// hatch, so the test needs no window system. Destruction is observed exactly:
// the test supplies the SurfaceDestroyer, which counts and then forwards to
// vkDestroySurfaceKHR. No linker or platform mechanism is used. Without the
// headless-surface extension the GPU test reports SKIP, never PASS.
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/surface.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;
using Error = SurfaceError;

const char *test_name = "graphics.surface.cpu";
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

// A headless-surface creator that records what Graphics asks of it, and a
// destroyer that counts before forwarding to the real vkDestroySurfaceKHR.
struct Hooks {
  PFN_vkCreateHeadlessSurfaceEXT create = nullptr;
  PFN_vkDestroySurfaceKHR destroy = nullptr;
  std::size_t create_calls = 0;
  std::size_t destroy_calls = 0;
  VkInstance last_instance = VK_NULL_HANDLE;
  VkSurfaceKHR last_destroyed = VK_NULL_HANDLE;
  VkResult forced_result = VK_SUCCESS;
  bool return_null_handle = false;
};

VkResult createHeadless(VkInstance instance, VkSurfaceKHR *surface, void *user) {
  auto *hooks = static_cast<Hooks *>(user);
  ++hooks->create_calls;
  hooks->last_instance = instance;
  if (hooks->forced_result != VK_SUCCESS)
    return hooks->forced_result;
  if (hooks->return_null_handle) {
    *surface = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }
  VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
  return hooks->create(instance, &info, nullptr, surface);
}

void destroyCounting(VkInstance instance, VkSurfaceKHR surface, void *user) {
  auto *hooks = static_cast<Hooks *>(user);
  ++hooks->destroy_calls;
  hooks->last_destroyed = surface;
  hooks->destroy(instance, surface, nullptr);
}

SurfaceCreator creatorFor(Hooks &hooks) {
  return {&createHeadless, &hooks, {&destroyCounting, &hooks}};
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

int cpuTest() {
  static_assert(!std::is_copy_constructible_v<Surface> &&
                std::is_nothrow_move_constructible_v<Surface> &&
                std::is_nothrow_move_assignable_v<Surface>);
  static_assert(std::is_trivially_copyable_v<SurfaceCreator> &&
                std::is_trivially_copyable_v<SurfaceDestroyer>);
  const SurfaceCreator empty;
  check(empty.create == nullptr && empty.destroyer.destroy == nullptr,
        "a default SurfaceCreator has no functions");
  check(std::string{surface_error_category().name()} == "terreate.graphics.surface" &&
            make_error_code(Error::surface_extension_not_enabled).message() ==
                "VK_KHR_surface is not enabled on the Instance",
        "error category name and message");
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.surface.cpu: cpu=PASS");
  return EXIT_SUCCESS;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  const bool has_surface =
      hasName(capabilities->available_extensions, VK_KHR_SURFACE_EXTENSION_NAME);
  const bool has_headless =
      hasName(capabilities->available_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
  const bool has_debug_utils =
      hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  const bool has_validation =
      hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation");

  // An Instance without VK_KHR_surface: an explicit failure, never a silent
  // no-op. This part needs no surface extension at all.
  {
    InstanceDescription plain_description;
    plain_description.api_version = VK_API_VERSION_1_3;
    plain_description.application_name = "terreate-surface-plain";
    const auto plain_plan = resolveInstance(plain_description, *capabilities);
    if (!plain_plan)
      return blocked("Vulkan 1.3 Instance without surface extensions");
    auto plain = createInstance(*plain_plan);
    if (!plain)
      return blocked("plain Vulkan Instance");
    Hooks hooks;
    check(
        hasError(createSurface(*plain, creatorFor(hooks)), Error::surface_extension_not_enabled) &&
            hooks.create_calls == 0,
        "an Instance without VK_KHR_surface is rejected before the creator runs");
    check(hasError(adoptSurface(*plain, vk::SurfaceKHR{reinterpret_cast<VkSurfaceKHR>(0x1)}),
                   Error::surface_extension_not_enabled),
          "adoptSurface checks the extension too");
  }

  if (!has_surface || !has_headless) {
    std::puts("graphics.surface: surface=SKIP reason=VK_EXT_headless_surface unavailable");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-surface";
  description.required_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_validation && has_debug_utils)
    description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance with surface extensions");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  auto other = createInstance(*plan);
  if (!instance || !other)
    return blocked("two Instances with surface extensions");

  Hooks hooks;
  hooks.create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(
      static_cast<VkInstance>(instance->nativeHandle()), "vkCreateHeadlessSurfaceEXT"));
  hooks.destroy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(vkGetInstanceProcAddr(
      static_cast<VkInstance>(instance->nativeHandle()), "vkDestroySurfaceKHR"));
  if (hooks.create == nullptr || hooks.destroy == nullptr)
    return blocked("headless-surface entry points");

  // Creator contract.
  check(hasError(createSurface(*instance, {}), Error::invalid_creator) && hooks.create_calls == 0,
        "a creator without a create function is rejected");
  hooks.forced_result = VK_ERROR_OUT_OF_HOST_MEMORY;
  const auto failed = createSurface(*instance, creatorFor(hooks));
  check(!failed &&
            failed.error().code() == vk::make_error_code(vk::Result::eErrorOutOfHostMemory) &&
            hooks.create_calls == 1 && hooks.destroy_calls == 0,
        "a native creator failure is returned as that native error and destroys nothing");
  hooks.forced_result = VK_SUCCESS;
  hooks.return_null_handle = true;
  check(hasError(createSurface(*instance, creatorFor(hooks)), Error::invalid_surface) &&
            hooks.create_calls == 2 && hooks.destroy_calls == 0,
        "VK_SUCCESS with a null handle is invalid_surface");
  hooks.return_null_handle = false;
  hooks.create_calls = 0;

  // A Surface exists before any Device: none is created in this test.
  {
    auto surface = createSurface(*instance, creatorFor(hooks));
    if (!check(surface.has_value(), "createSurface through the headless creator"))
      return EXIT_FAILURE;
    check(hooks.create_calls == 1 &&
              hooks.last_instance == static_cast<VkInstance>(instance->nativeHandle()),
          "the creator runs once, with the Instance's native handle");
    check(surface->valid() && surface->nativeSurface() != VK_NULL_HANDLE &&
              static_cast<VkSurfaceKHR>(surface->nativeHandle()) == surface->nativeSurface() &&
              surface->nativeInstance() == instance->nativeHandle() &&
              surface->correlatedWith(*instance) && !surface->correlatedWith(*other),
          "native handles and correlation");
    const auto handle = surface->nativeSurface();
    Surface moved = std::move(*surface);
    check(!surface->valid() && surface->nativeSurface() == VK_NULL_HANDLE &&
              !surface->nativeInstance() && !surface->correlatedWith(*instance) &&
              moved.nativeSurface() == handle && hooks.destroy_calls == 0,
          "a move transfers ownership and destroys nothing");
    // Move assignment destroys the surface the target owned.
    auto second = createSurface(*instance, creatorFor(hooks));
    if (!check(second.has_value(), "a second Surface"))
      return EXIT_FAILURE;
    const auto second_handle = second->nativeSurface();
    check(second_handle != handle, "two live Surfaces have distinct handles");
    moved = std::move(*second);
    check(hooks.destroy_calls == 1 && hooks.last_destroyed == handle &&
              moved.nativeSurface() == second_handle,
          "move assignment destroys the target's old surface exactly once");
  }
  check(hooks.destroy_calls == 2, "leaving scope destroys the remaining surface exactly once");

  // adoptSurface, with and without a destroyer.
  {
    hooks.destroy_calls = 0;
    VkSurfaceKHR raw = VK_NULL_HANDLE;
    VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    if (hooks.create(static_cast<VkInstance>(instance->nativeHandle()), &info, nullptr, &raw) !=
        VK_SUCCESS)
      return blocked("native headless surface");
    check(hasError(adoptSurface(*instance, vk::SurfaceKHR{}), Error::invalid_surface),
          "a null handle cannot be adopted");
    {
      auto adopted = adoptSurface(*instance, vk::SurfaceKHR{raw}, {&destroyCounting, &hooks});
      if (!check(adopted.has_value(), "adoptSurface"))
        return EXIT_FAILURE;
      check(adopted->nativeSurface() == raw && adopted->correlatedWith(*instance),
            "the adopted surface keeps its handle");
    }
    check(hooks.destroy_calls == 1 && hooks.last_destroyed == raw,
          "an adopted surface is destroyed through its destroyer");
    // Default destroyer: no hook is called and nothing leaks (validation).
    VkSurfaceKHR default_raw = VK_NULL_HANDLE;
    if (hooks.create(static_cast<VkInstance>(instance->nativeHandle()), &info, nullptr,
                     &default_raw) != VK_SUCCESS)
      return blocked("second native headless surface");
    {
      auto adopted = adoptSurface(*instance, vk::SurfaceKHR{default_raw});
      check(adopted.has_value() && adopted->nativeSurface() == default_raw,
            "adoptSurface with the default destroyer");
    }
    check(hooks.destroy_calls == 1, "the default destroyer calls no hook");
  }

  // The Surface is usable by Vulkan before a Device exists: a physical-device
  // query against it needs no logical Device.
  {
    const auto candidates = queryPhysicalDevices(*instance);
    if (!candidates || candidates->empty())
      return blocked("PhysicalDevice inventory");
    auto surface = createSurface(*instance, creatorFor(hooks));
    if (!check(surface.has_value(), "Surface before any Device"))
      return EXIT_FAILURE;
    VkBool32 supported = VK_FALSE;
    const auto result = vkGetPhysicalDeviceSurfaceSupportKHR(
        static_cast<VkPhysicalDevice>(candidates->front().device().nativeHandle()), 0,
        surface->nativeSurface(), &supported);
    check(result == VK_SUCCESS, "a physical-device surface query works before any Device exists");
  }

  if (has_validation) {
    check(recorder.warnings_or_errors.load() == 0, "no validation warnings or errors");
    std::puts("graphics.surface: validation-layer=PASS");
  } else {
    std::puts(
        "graphics.surface: validation-layer=SKIP reason=VK_LAYER_KHRONOS_validation unavailable");
  }
  if (failures != 0)
    return EXIT_FAILURE;
  std::puts("graphics.surface: surface=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu") {
    test_name = "graphics.surface";
    return gpuTest();
  }
  return blocked("unknown mode");
}
