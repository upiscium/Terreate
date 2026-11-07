#include <terreate/graphics/shader_module.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;

// Compiled offline with glslangValidator -V -S comp and validated with
// spirv-val --target-env vulkan1.3. Not compiled or reflected at test runtime.
// GLSL: #version 450
//       layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
//       void main() {}
constexpr std::array<std::uint32_t, 70> compute_words{
    0x07230203, 0x00010000, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00040047, 0x00000009, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
    0x00000007, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001, 0x0006002c,
    0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038,
};

static_assert(!std::default_initializable<SpirvBinary>);
static_assert(std::copy_constructible<SpirvBinary>);
static_assert(std::is_nothrow_move_constructible_v<SpirvBinary>);
static_assert(!std::default_initializable<ShaderModule>);
static_assert(!std::copy_constructible<ShaderModule>);
static_assert(!std::is_copy_assignable_v<ShaderModule>);
static_assert(std::is_nothrow_move_constructible_v<ShaderModule>);
static_assert(std::is_nothrow_move_assignable_v<ShaderModule>);
static_assert(std::is_nothrow_destructible_v<ShaderModule>);

int failures = 0;
std::size_t create_calls = 0;
std::size_t destroy_calls = 0;
std::optional<VkResult> injected_create_result{};
bool return_null_on_success = false;
VkShaderModuleCreateInfo last_create_info{};
std::array<std::uint32_t, compute_words.size()> observed_code{};
bool observed_aligned_code = false;
std::array<VkShaderModule, 8> live_modules{};
bool double_destroy_or_leak = false;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "graphics.shader_module: assertion=FAIL: %s\n", message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "graphics.shader_module: validation=BLOCKED: %s\n", message);
  return EXIT_FAILURE;
}

bool hasSpirvError(const terreate::Result<SpirvBinary> &result, SpirvBinaryError code) {
  return !result && result.error().code() == make_error_code(code);
}
template <typename T>
bool hasShaderError(const terreate::Result<T> &result, ShaderModuleError code) {
  return !result && result.error().code() == make_error_code(code);
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity == terreate::DiagnosticSeverity::warning ||
        event.severity == terreate::DiagnosticSeverity::error) {
      for (const auto &category : event.categories) {
        if (category == "VALIDATION") {
          warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
    }
  }
};
} // namespace

// Observe real Vulkan calls through the existing GCC linker-wrapper convention.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *,
                                                const VkAllocationCallbacks *, VkShaderModule *);
extern "C" void __real_vkDestroyShaderModule(VkDevice, VkShaderModule,
                                             const VkAllocationCallbacks *);

extern "C" VkResult __wrap_vkCreateShaderModule(VkDevice device,
                                                const VkShaderModuleCreateInfo *info,
                                                const VkAllocationCallbacks *alloc,
                                                VkShaderModule *result) {
  ++create_calls;
  if (info) {
    last_create_info = *info;
    observed_aligned_code =
        info->pCode && reinterpret_cast<std::uintptr_t>(info->pCode) % alignof(std::uint32_t) == 0;
    if (observed_aligned_code && info->codeSize == sizeof(observed_code))
      std::memcpy(observed_code.data(), info->pCode, sizeof(observed_code));
  }
  if (injected_create_result) {
    const auto failure = *injected_create_result;
    injected_create_result.reset();
    *result = VK_NULL_HANDLE;
    return failure;
  }
  if (return_null_on_success) {
    return_null_on_success = false;
    *result = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }
  const auto status = __real_vkCreateShaderModule(device, info, alloc, result);
  if (status == VK_SUCCESS && *result != VK_NULL_HANDLE) {
    bool inserted = false;
    for (auto &handle : live_modules) {
      if (handle == VK_NULL_HANDLE) {
        handle = *result;
        inserted = true;
        break;
      }
    }
    double_destroy_or_leak |= !inserted;
  }
  return status;
}
extern "C" void __wrap_vkDestroyShaderModule(VkDevice device, VkShaderModule shader,
                                             const VkAllocationCallbacks *alloc) {
  if (shader != VK_NULL_HANDLE) {
    ++destroy_calls;
    bool removed = false;
    for (auto &handle : live_modules) {
      if (handle == shader && !removed) {
        handle = VK_NULL_HANDLE;
        removed = true;
      }
    }
    double_destroy_or_leak |= !removed;
  }
  __real_vkDestroyShaderModule(device, shader, alloc);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
bool cpuTest() {
  bool passed = true;
  const auto binary = SpirvBinary::fromWords(compute_words);
  if (!binary)
    return check(false, "offline-validated SPIR-V fixture must parse");
  passed &=
      check(binary->valid() && binary->wordCount() == compute_words.size() &&
                binary->byteSize() == compute_words.size() * sizeof(std::uint32_t) &&
                binary->versionWord() == 0x00010000 &&
                std::memcmp(binary->words().data(), compute_words.data(), binary->byteSize()) == 0,
            "fromWords snapshots exact valid binary header and words");

  auto input = std::vector<std::uint32_t>(compute_words.begin(), compute_words.end());
  auto snapshot = SpirvBinary::fromWords(input);
  std::fill(input.begin(), input.end(), 0U);
  passed &= check(snapshot && snapshot->words()[0] == compute_words[0],
                  "input words are snapshotted before caller changes");

  auto serialized = binary->toBytes();
  passed &= check(serialized && serialized->size() == binary->byteSize() &&
                      (*serialized)[0] == std::byte{0x03} && (*serialized)[1] == std::byte{0x02} &&
                      (*serialized)[2] == std::byte{0x23} && (*serialized)[3] == std::byte{0x07},
                  "toBytes explicitly serializes little-endian SPIR-V");
  if (!serialized)
    return false;
  const auto reparsed = SpirvBinary::fromBytes(*serialized);
  passed &= check(
      reparsed && reparsed->wordCount() == binary->wordCount() &&
          std::memcmp(reparsed->words().data(), compute_words.data(), binary->byteSize()) == 0,
      "little-endian byte parser round-trips the words");

  std::array<std::uint32_t, 5> header{0x07230203, 0x00010600, 0, 2, 0};
  passed &= check(SpirvBinary::fromWords(header).has_value(),
                  "SPIR-V 1.6 structural header accepted without semantic inference");
  header[0] = 0x03022307;
  passed &= check(hasSpirvError(SpirvBinary::fromWords(header), SpirvBinaryError::invalid_magic),
                  "swapped-endian magic rejected, not silently byte-swapped");
  header[0] = 0x07230203;
  header[1] = 0x00010700;
  passed &= check(hasSpirvError(SpirvBinary::fromWords(header), SpirvBinaryError::invalid_version),
                  "future SPIR-V version rejected");
  header[1] = 0x00010600;
  header[3] = 0;
  passed &= check(hasSpirvError(SpirvBinary::fromWords(header), SpirvBinaryError::invalid_header),
                  "zero bound rejected");
  header[3] = 2;
  header[4] = 1;
  passed &= check(hasSpirvError(SpirvBinary::fromWords(header), SpirvBinaryError::invalid_header),
                  "nonzero schema rejected");
  passed &= check(hasSpirvError(SpirvBinary::fromWords({}), SpirvBinaryError::invalid_size),
                  "empty SPIR-V rejected");
  const std::array<std::byte, 19> incomplete{};
  passed &=
      check(!SpirvBinary::fromBytes(incomplete) &&
                hasSpirvError(SpirvBinary::fromBytes(incomplete), SpirvBinaryError::invalid_size),
            "truncated bytes rejected");
  auto odd = *serialized;
  odd.push_back(std::byte{0});
  passed &= check(hasSpirvError(SpirvBinary::fromBytes(odd), SpirvBinaryError::invalid_size),
                  "non-word-aligned bytes rejected");

  SpirvBinary copy = *binary;
  SpirvBinary moved = std::move(copy);
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(moved.valid() && !copy.valid() && copy.words().empty() &&
                      moved.versionWord() == binary->versionWord() && !copy.toBytes().has_value(),
                  "move preserves unique CPU buffer state");

  const auto bad_magic = make_error_code(SpirvBinaryError::invalid_magic);
  const auto bad_device = make_error_code(ShaderModuleError::invalid_device);
  passed &= check(bad_magic.value() != 0 && bad_device.value() != 0 &&
                      bad_magic.category() != bad_device.category(),
                  "public structured error category names and codes are separate");
  passed &= check(create_calls == 0 && destroy_calls == 0,
                  "CPU-only SPIR-V representation never requires native Vulkan work");
  if (passed && failures == 0)
    std::puts("graphics.spirv_binary: cpu=PASS");
  return passed && failures == 0;
}

bool nativeTest(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device) {
  const auto binary = SpirvBinary::fromWords(compute_words);
  if (!binary)
    return check(false, "precompiled SPIR-V fixture rejected");
  bool passed = true;
  const auto creates_before = create_calls;
  const auto destroys_before = destroy_calls;
  {
    auto moved_code = SpirvBinary::fromWords(compute_words);
    if (!moved_code)
      return check(false, "second SPIR-V fixture parse");
    SpirvBinary value = std::move(*moved_code);
    passed &= check(hasShaderError(createShaderModule(device, *moved_code),
                                   ShaderModuleError::invalid_binary) &&
                        create_calls == creates_before,
                    "moved-from SPIR-V cannot reach native creation");
  }

  injected_create_result = VK_ERROR_OUT_OF_HOST_MEMORY;
  const auto failed = createShaderModule(device, *binary);
  passed &= check(
      !failed && failed.error().code() == vk::make_error_code(vk::Result::eErrorOutOfHostMemory) &&
          create_calls == creates_before + 1 && destroy_calls == destroys_before &&
          !injected_create_result,
      "native VkResult is preserved without manufacturing an owner");

  return_null_on_success = true;
  passed &= check(
      hasShaderError(createShaderModule(device, *binary), ShaderModuleError::native_null_handle) &&
          create_calls == creates_before + 2 && destroy_calls == destroys_before &&
          !return_null_on_success,
      "native success with null handle is not a valid owner");

  {
    auto first = createShaderModule(device, *binary);
    auto second = createShaderModule(device, *binary);
    if (!first || !second)
      return blocked("real Vulkan ShaderModule creation");
    const auto first_native = first->nativeShaderModule();
    const auto second_native = second->nativeShaderModule();
    passed &= check(first->valid() && second->valid() &&
                        first->nativeHandle() == vk::ShaderModule{first_native} &&
                        first->correlatedWith(device) && second->correlatedWith(device) &&
                        first->configuration() &&
                        first->configuration()->code_size_bytes == binary->byteSize() &&
                        first->configuration()->spirv_version_word == binary->versionWord(),
                    "native handle, Device correlation and effective code size observable");

    passed &= check(create_calls == creates_before + 4 &&
                        last_create_info.sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO &&
                        !last_create_info.pNext && last_create_info.flags == 0 &&
                        last_create_info.codeSize == compute_words.size() * sizeof(std::uint32_t) &&
                        observed_aligned_code && observed_code == compute_words,
                    "vkCreateShaderModule receives exact CPU-owned words and no inferred options");

    ShaderModule moved = std::move(*first);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!first->valid() && first->nativeShaderModule() == VK_NULL_HANDLE &&
                        !first->configuration() && moved.nativeShaderModule() == first_native,
                    "move constructor transfers exclusive ShaderModule ownership");
    *second = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!moved.valid() && second->nativeShaderModule() == first_native &&
                        second_native != first_native && destroy_calls == destroys_before + 1,
                    "move-assignment destroys displaced Module exactly once");
  }
  passed &= check(destroy_calls == destroys_before + 2 && !double_destroy_or_leak,
                  "both native ShaderModules are destroyed exactly once");
  for (auto module : live_modules)
    passed &= check(module == VK_NULL_HANDLE, "no live ShaderModules after test");
  static_cast<void>(instance);
  static_cast<void>(candidate);
  return passed && failures == 0;
}

int gpuTest() {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan instance capabilities unavailable");
  bool has_validation = false;
  bool has_debug = false;
  for (const auto &layer : caps->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : caps->available_extensions)
    has_debug |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (has_validation && !has_debug)
    return blocked("Validation Layer available but Debug Utils unavailable");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  if (has_debug)
    description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *caps);
  if (!plan)
    return blocked("Vulkan 1.3 Instance resolution");
  if (has_validation) {
    bool layer_enabled = false;
    for (const auto &layer : plan->enabledLayers())
      layer_enabled |= layer == "VK_LAYER_KHRONOS_validation";
    if (!layer_enabled || !plan->debugUtilsEnabled())
      return blocked("Validation Layer / Debug Utils not enabled in effective configuration");
  }

  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("headless Vulkan Instance creation");
  if (has_validation) {
    // A synthetic warning proves the real Debug Utils -> DiagnosticSinkView
    // transport is live, independently of actual validation cleanliness.
    const auto raw = static_cast<VkInstance>(instance->nativeHandle());
    const auto send = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(raw, "vkSubmitDebugUtilsMessageEXT"));
    if (!send)
      return blocked("native validation callback entry point unavailable");
    VkDebugUtilsMessengerCallbackDataEXT data{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    data.pMessageIdName = "terreate-shader-module-validation-probe";
    data.messageIdNumber = 260;
    data.pMessage = "synthetic validation warning to verify native sink";
    const auto prior = recorder.warnings_or_errors.load(std::memory_order_relaxed);
    send(raw, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
    if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == prior + 1,
               "native Debug Utils warning must reach the application recorder"))
      return EXIT_FAILURE;
    recorder.warnings_or_errors.fetch_sub(1, std::memory_order_relaxed);
  }

  const auto devices = queryPhysicalDevices(*instance);
  if (!devices || devices->empty())
    return blocked("no Vulkan PhysicalDevice");
  for (const auto &candidate : *devices) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount ||
          !(families[index].queueFlags & vk::QueueFlagBits::eTransfer))
        continue;
      DeviceDescription request;
      DeviceQueueRequest queue_request;
      queue_request.caller_id = "shader-module";
      queue_request.required_flags = vk::QueueFlagBits::eTransfer;
      queue_request.allowed_family_indices = {index};
      request.queue_requests.push_back(queue_request);
      auto device_plan = resolveDevice(candidate, request);
      if (!device_plan)
        continue;
      bool passed = false;
      {
        auto device = createDevice(*instance, *device_plan);
        if (!device)
          return blocked("native ShaderModule Device creation");
        passed = nativeTest(*instance, candidate, *device);
      }
      if (!passed)
        return EXIT_FAILURE;
      if (has_validation) {
        if (recorder.warnings_or_errors.load(std::memory_order_relaxed) != 0)
          return blocked("real Vulkan Validation Layer warning/error during ShaderModule test");
        std::puts("graphics.shader_module: validation-layer=PASS");
      } else {
        std::puts("graphics.shader_module: validation-layer=SKIP reason=layer-unavailable");
      }
      if (!failures) {
        std::puts("graphics.shader_module: headless-native=PASS");
        return EXIT_SUCCESS;
      }
      return EXIT_FAILURE;
    }
  }
  return blocked("no transferable queue with usable native Device");
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest() ? EXIT_SUCCESS : EXIT_FAILURE;
  if (mode == "gpu")
    return gpuTest();
  return blocked("unknown test mode");
}
