#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_set.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace terreate::graphics;
static_assert(!std::default_initializable<DescriptorPool>);
static_assert(!std::copy_constructible<DescriptorPool>);
static_assert(!std::is_copy_assignable_v<DescriptorPool>);
static_assert(std::is_nothrow_move_constructible_v<DescriptorPool>);
static_assert(std::is_nothrow_move_assignable_v<DescriptorPool>);
static_assert(std::is_nothrow_destructible_v<DescriptorPool>);
static_assert(!std::copy_constructible<DescriptorSet>);
static_assert(!std::is_copy_assignable_v<DescriptorSet>);
static_assert(std::is_nothrow_move_constructible_v<DescriptorSet>);
static_assert(std::is_nothrow_move_assignable_v<DescriptorSet>);
static_assert(std::is_trivially_destructible_v<DescriptorSet>);
static_assert(std::is_aggregate_v<DescriptorPoolSizeDescription>);

const char *test_name = "graphics.descriptor_pool.cpu";
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
  return EXIT_FAILURE;
}
template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code code) {
  return !result && result.error().code() == make_error_code(code);
}
constexpr auto free_bit = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
constexpr auto uniform = vk::DescriptorType::eUniformBuffer;
constexpr std::array supported_types{uniform,
                                     vk::DescriptorType::eStorageBuffer,
                                     vk::DescriptorType::eSampler,
                                     vk::DescriptorType::eSampledImage,
                                     vk::DescriptorType::eStorageImage,
                                     vk::DescriptorType::eCombinedImageSampler,
                                     vk::DescriptorType::eUniformBufferDynamic,
                                     vk::DescriptorType::eStorageBufferDynamic};
constexpr std::array unsupported_types{
    vk::DescriptorType::eInputAttachment,
    vk::DescriptorType::eUniformTexelBuffer,
    vk::DescriptorType::eStorageTexelBuffer,
    vk::DescriptorType::eInlineUniformBlock,
    vk::DescriptorType::eAccelerationStructureKHR,
    vk::DescriptorType::eMutableEXT,
    static_cast<vk::DescriptorType>(-1),
    static_cast<vk::DescriptorType>(0x7ffffffe),
    static_cast<vk::DescriptorType>(VK_DESCRIPTOR_TYPE_MAX_ENUM)};

enum class Injection : std::uint8_t {
  none,
  error,
  partial_error,
  null_success,
  mixed_success,
  duplicate_success
};
struct Tracker {
  struct Record {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
  };
  std::array<Record, 16> live{};
  std::size_t creates = 0, destroys = 0, allocates = 0, frees = 0, resets = 0, handles = 0;
  VkDescriptorPool last_destroyed = VK_NULL_HANDLE;
  Injection create = Injection::none, allocate = Injection::none;
  Injection free = Injection::none, reset = Injection::none;
  bool forbid = true;
  const DescriptorPoolDescription *expected_description = nullptr;
  DescriptorPoolDescription *release_description = nullptr;
  std::span<const DescriptorSetLayout *const> expected_layouts{};
  std::array<VkDescriptorSet, 8> allocated{};
  std::array<VkDescriptorSet, 8> expected_free{};
  std::size_t expected_free_count = 0;
  VkDevice expected_device = VK_NULL_HANDLE;
  VkDescriptorPool expected_pool = VK_NULL_HANDLE;
  std::size_t calls() const { return creates + destroys + allocates + frees + resets; }
  bool empty() const {
    for (const auto &record : live)
      if (record.pool != VK_NULL_HANDLE)
        return false;
    return true;
  }
} native;

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity == terreate::DiagnosticSeverity::warning ||
        event.severity == terreate::DiagnosticSeverity::error)
      for (const auto &category : event.categories)
        if (category == "VALIDATION") {
          warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
          break;
        }
  }
};
} // namespace

// C linker wrappers observe real calls; fault injection never publishes a fake
// object to the driver. Mixed/duplicate success retains real allocations in the
// pool until the application's explicit reset. No test-side hidden cleanup.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *,
                                                  const VkAllocationCallbacks *,
                                                  VkDescriptorPool *);
extern "C" void __real_vkDestroyDescriptorPool(VkDevice, VkDescriptorPool,
                                               const VkAllocationCallbacks *);
extern "C" VkResult __real_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *,
                                                    VkDescriptorSet *);
extern "C" VkResult __real_vkFreeDescriptorSets(VkDevice, VkDescriptorPool, std::uint32_t,
                                                const VkDescriptorSet *);
extern "C" VkResult __real_vkResetDescriptorPool(VkDevice, VkDescriptorPool,
                                                 VkDescriptorPoolResetFlags);

extern "C" VkResult __wrap_vkCreateDescriptorPool(VkDevice device,
                                                  const VkDescriptorPoolCreateInfo *info,
                                                  const VkAllocationCallbacks *allocator,
                                                  VkDescriptorPool *out) {
  ++native.creates;
  if (!check(!native.forbid && info && out, "only valid native create calls allowed"))
    return VK_ERROR_INITIALIZATION_FAILED;
  if (info->poolSizeCount > 0 &&
      !check(info->pPoolSizes != nullptr, "nonempty pool sizes require pointer"))
    return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_NULL_HANDLE;
  check(info->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO && !info->pNext && !allocator,
        "create header, pNext and allocator exact");
  check((info->poolSizeCount == 0) == (info->pPoolSizes == nullptr),
        "empty pool sizes use null pointer");
  if (native.release_description) {
    auto *input = std::exchange(native.release_description, nullptr);
    check(static_cast<const void *>(info->pPoolSizes) !=
              static_cast<const void *>(input->sizes.data()),
          "native sizes do not alias caller vector");
    std::vector<DescriptorPoolSizeDescription>{}.swap(input->sizes);
  }
  if (native.expected_description) {
    const auto &expected = *native.expected_description;
    check(device == native.expected_device && info->maxSets == expected.max_sets &&
              info->flags == static_cast<VkDescriptorPoolCreateFlags>(expected.flags) &&
              info->poolSizeCount == expected.sizes.size(),
          "exact create Device, flags and capacities");
    for (std::size_t index = 0; index < expected.sizes.size() && index < info->poolSizeCount;
         ++index)
      check(info->pPoolSizes[index].type ==
                    static_cast<VkDescriptorType>(expected.sizes[index].type) &&
                info->pPoolSizes[index].descriptorCount == expected.sizes[index].descriptor_count,
            "native sizes preserve order and duplicates");
  }
  const auto injection = std::exchange(native.create, Injection::none);
  if (injection == Injection::error)
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  if (injection == Injection::null_success)
    return VK_SUCCESS;
  const auto result = __real_vkCreateDescriptorPool(device, info, allocator, out);
  if (result == VK_SUCCESS && *out) {
    ++native.handles;
    for (auto &record : native.live)
      if (!record.pool) {
        record = {*out, device};
        return result;
      }
    check(false, "pool tracker capacity");
  }
  return result;
}
extern "C" void __wrap_vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                               const VkAllocationCallbacks *allocator) {
  ++native.destroys;
  native.last_destroyed = pool;
  check(!native.forbid && !allocator, "destruction permitted and null allocator");
  for (auto &record : native.live)
    if (record.pool == pool && pool) {
      check(record.device == device, "destruction uses original native Device");
      __real_vkDestroyDescriptorPool(record.device, pool, allocator);
      record = {};
      return;
    }
  check(false, "double destruction or unknown pool");
}
extern "C" VkResult __wrap_vkAllocateDescriptorSets(VkDevice device,
                                                    const VkDescriptorSetAllocateInfo *info,
                                                    VkDescriptorSet *out) {
  ++native.allocates;
  if (!check(!native.forbid && info && out, "invalid allocation never reaches Vulkan"))
    return VK_ERROR_INITIALIZATION_FAILED;
  if (!check(info->descriptorSetCount > 0 && info->pSetLayouts != nullptr,
             "nonempty allocation layouts require pointer"))
    return VK_ERROR_INITIALIZATION_FAILED;
  check(info->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO && !info->pNext &&
            info->pSetLayouts && info->descriptorSetCount > 0 &&
            info->descriptorPool == native.expected_pool && device == native.expected_device,
        "exact allocation header, pool and Device");
  if (!native.expected_layouts.empty()) {
    check(info->descriptorSetCount == native.expected_layouts.size(), "exact allocation count");
    for (std::size_t index = 0;
         index < native.expected_layouts.size() && index < info->descriptorSetCount; ++index)
      check(info->pSetLayouts[index] == native.expected_layouts[index]->nativeDescriptorSetLayout(),
            "allocation layout order exact");
  }
  const auto injection = std::exchange(native.allocate, Injection::none);
  if (injection == Injection::error)
    return VK_ERROR_OUT_OF_POOL_MEMORY;
  if (injection == Injection::partial_error) {
    // Undefined failed output must never be adopted or freed; no driver call.
    out[0] =
        std::bit_cast<VkDescriptorSet>(std::uintptr_t{1}); // deliberately synthetic error output
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  if (injection == Injection::null_success) {
    for (std::size_t index = 0; index < info->descriptorSetCount; ++index)
      out[index] = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }
  const auto result = __real_vkAllocateDescriptorSets(device, info, out);
  if (result == VK_SUCCESS) {
    if (injection == Injection::mixed_success)
      out[info->descriptorSetCount - 1] = VK_NULL_HANDLE;
    if (injection == Injection::duplicate_success && info->descriptorSetCount > 1)
      out[1] = out[0];
    for (std::size_t index = 0; index < info->descriptorSetCount && index < native.allocated.size();
         ++index)
      native.allocated[index] = out[index];
  }
  return result;
}
extern "C" VkResult __wrap_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool,
                                                std::uint32_t count, const VkDescriptorSet *sets) {
  ++native.frees;
  if (!check(!native.forbid && count && sets, "invalid free never reaches Vulkan"))
    return VK_ERROR_INITIALIZATION_FAILED;
  check(device == native.expected_device && pool == native.expected_pool,
        "free Device and pool exact");
  if (native.expected_free_count) {
    check(count == native.expected_free_count, "free count exact");
    for (std::size_t index = 0; index < count && index < native.expected_free_count; ++index)
      check(sets[index] == native.expected_free[index], "free handle ordering exact");
  }
  const auto injection = std::exchange(native.free, Injection::none);
  if (injection == Injection::partial_error) {
    check(__real_vkFreeDescriptorSets(device, pool, 1, sets) == VK_SUCCESS, "partial free fixture");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  if (injection == Injection::error)
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  return __real_vkFreeDescriptorSets(device, pool, count, sets);
}
extern "C" VkResult __wrap_vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                 VkDescriptorPoolResetFlags flags) {
  ++native.resets;
  if (!check(!native.forbid, "invalid reset never reaches Vulkan"))
    return VK_ERROR_INITIALIZATION_FAILED;
  check(device == native.expected_device && pool == native.expected_pool && flags == 0,
        "reset Device, pool and zero flags exact");
  const auto injection = std::exchange(native.reset, Injection::none);
  if (injection == Injection::partial_error) {
    check(__real_vkResetDescriptorPool(device, pool, flags) == VK_SUCCESS,
          "uncertain reset fixture");
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  if (injection == Injection::error)
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  return __real_vkResetDescriptorPool(device, pool, flags);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
bool cpuTest() {
  check(hasError(validateDescriptorPoolDescription({}), DescriptorPoolError::invalid_description),
        "zero max_sets rejected");
  check(validateDescriptorPoolDescription({1, {}, {}}).has_value(), "zero pool sizes accepted");
  check(validateDescriptorPoolDescription({1, free_bit, {}}).has_value(), "FREE bit accepted");
  for (const auto flags : {2U, 4U, 0x80000000U, 0xffffffffU})
    check(hasError(validateDescriptorPoolDescription({1, vk::DescriptorPoolCreateFlags{flags}, {}}),
                   DescriptorPoolError::unsupported_configuration),
          "extension/unknown flag bits rejected");
  for (const auto type : supported_types) {
    check(validateDescriptorPoolDescription({1, {}, {{type, 1}, {type, 2}}}).has_value(),
          "all eight types and duplicates accepted");
    check(hasError(validateDescriptorPoolDescription({1, {}, {{type, 0}}}),
                   DescriptorPoolError::invalid_description),
          "zero size count rejected");
  }
  for (const auto type : unsupported_types)
    check(hasError(validateDescriptorPoolDescription({1, {}, {{type, 1}}}),
                   DescriptorPoolError::unsupported_configuration),
          "unsupported/signed/unknown types rejected");
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  const DescriptorPoolDescription large{maximum, {}, {{uniform, maximum}, {uniform, maximum}}};
  check(validateDescriptorPoolDescription(large).has_value(),
        "no signed narrowing, wrapping duplicate sum or artificial capacity cap");
  // A vector/span beyond UINT32_MAX requires real backing storage. Do not forge
  // an invalid range to test representability; production checks precede access.
  const std::error_code code = DescriptorPoolError::invalid_description;
  check(code.category() == descriptor_pool_error_category() && code.value() != 0 &&
            std::string_view{code.category().name()} == "terreate.graphics.descriptor_pool",
        "structured error category");
  DescriptorSet empty;
  check(!empty.valid() && !empty.nativeDescriptorSet() && !empty.nativeDescriptorPool() &&
            !empty.nativeVkDevice() && !empty.nativeDescriptorSetLayout(),
        "empty borrowed view");
  check(native.calls() == 0, "CPU validation makes no Vulkan calls");
  if (!failures)
    std::puts("graphics.descriptor_pool.cpu: cpu=PASS");
  return failures == 0;
}

void expectPool(const DescriptorPool &pool) {
  native.expected_device = pool.nativeVkDevice();
  native.expected_pool = pool.nativeDescriptorPool();
}
// Moved-from views are explicitly observable by the public API.
// NOLINTBEGIN(clang-analyzer-cplusplus.Move)
bool emptyView(const DescriptorSet &set) {
  return !set.valid() && !static_cast<bool>(set) && !set.nativeDescriptorSet() &&
         !set.nativeDescriptorPool() && !set.nativeVkDevice() && !set.nativeDescriptorSetLayout() &&
         set.nativeHandle() == vk::DescriptorSet{} && set.nativePool() == vk::DescriptorPool{} &&
         set.nativeDevice() == vk::Device{} && set.nativeLayout() == vk::DescriptorSetLayout{};
}
// NOLINTEND(clang-analyzer-cplusplus.Move)

bool poolTest(Device &device, Device &moved_from, Device &other) {
  auto empty = createDescriptorSetLayout(device, {});
  auto layout =
      createDescriptorSetLayout(device, {{{3, uniform, 1, vk::ShaderStageFlagBits::eCompute}}});
  auto foreign = createDescriptorSetLayout(other, {});
  if (!check(empty && layout && foreign, "real layouts on two Devices"))
    return false;
  const auto before = native.calls();
  native.forbid = true;
  check(
      hasError(createDescriptorPool(moved_from, {1, {}, {}}), DescriptorPoolError::invalid_device),
      "moved-from parent rejected");
  check(hasError(createDescriptorPool(device, {}), DescriptorPoolError::invalid_description),
        "factory validates max_sets");
  check(hasError(createDescriptorPool(device, {1, {}, {{uniform, 0}}}),
                 DescriptorPoolError::invalid_description),
        "factory validates sizes");
  check(hasError(createDescriptorPool(device, {1, vk::DescriptorPoolCreateFlags{2U}, {}}),
                 DescriptorPoolError::unsupported_configuration),
        "factory validates flags");
  for (const auto type : unsupported_types)
    check(hasError(createDescriptorPool(device, {1, {}, {{type, 1}}}),
                   DescriptorPoolError::unsupported_configuration),
          "factory validates type");
  check(native.calls() == before, "invalid factories never call Vulkan");
  native.forbid = false;
  native.create = Injection::error;
  const auto failed = createDescriptorPool(device, {1, {}, {}});
  check(hasError(failed, vk::Result::eErrorOutOfHostMemory) &&
            failed.error().context() == "vkCreateDescriptorPool",
        "create VkResult and context preserved");
  native.create = Injection::null_success;
  check(
      hasError(createDescriptorPool(device, {1, {}, {}}), DescriptorPoolError::native_null_handle),
      "null create success rejected");
  const auto destroy_before = native.destroys;
  {
    DescriptorPoolDescription description{
        8, free_bit, {{uniform, 4}, {vk::DescriptorType::eSampler, 1}, {uniform, 4}}};
    const auto requested = description;
    native.expected_description = &requested;
    native.expected_device = static_cast<VkDevice>(device.nativeHandle());
    native.release_description = &description;
    auto result = createDescriptorPool(device, description);
    native.expected_description = nullptr;
    native.release_description = nullptr;
    if (!check(result.has_value(), "real FREE pool creation"))
      return false;
    auto &pool = *result;
    expectPool(pool);
    check(description.sizes.empty() && pool.configuration()->requested == requested &&
              pool.configuration()->effective == requested,
          "independent exact configuration snapshots");
    check(pool.valid() && static_cast<bool>(pool) && pool.state() == DescriptorPoolState::ready &&
              pool.nativeHandle() == vk::DescriptorPool{pool.nativeDescriptorPool()} &&
              pool.nativeDevice() == device.nativeHandle() && pool.correlatedWith(device) &&
              !pool.correlatedWith(other) && !pool.correlatedWith(moved_from),
          "pool native observers/correlation");
    const std::array<const DescriptorSetLayout *, 3> ordered{&*layout, &*empty, &*layout};
    const std::array<const DescriptorSetLayout *, 1> null_layout{nullptr},
        foreign_layout{&*foreign};
    const auto invalid_before = native.calls();
    native.forbid = true;
    check(hasError(pool.allocate(std::span<const DescriptorSetLayout *const>{}),
                   DescriptorPoolError::invalid_count),
          "empty allocation rejected");
    check(hasError(pool.allocate(null_layout), DescriptorPoolError::invalid_layout),
          "null layout rejected");
    check(hasError(pool.allocate(foreign_layout), DescriptorPoolError::invalid_layout),
          "foreign layout rejected");
    DescriptorSetLayout moved_layout = std::move(*foreign);
    check(hasError(pool.allocate(*foreign), DescriptorPoolError::invalid_layout),
          "moved-from layout rejected");
    DescriptorSet invalid;
    check(hasError(pool.free(invalid), DescriptorPoolError::invalid_set), "empty view rejected");
    check(hasError(pool.free(std::span<DescriptorSet>{}), DescriptorPoolError::invalid_count),
          "empty free rejected");
    check(native.calls() == invalid_before, "all bad batches rejected before Vulkan");
    native.forbid = false;

    native.expected_layouts = ordered;
    const auto allocations_before = native.allocates;
    auto sets = pool.allocate(ordered);
    if (!check(sets && sets->size() == ordered.size(), "real ordered batch allocation"))
      return false;
    check(native.allocates == allocations_before + 1, "batch uses exactly one native allocate");
    for (std::size_t index = 0; index < sets->size(); ++index) {
      const auto &set = (*sets)[index];
      check(set.valid() && set.nativeDescriptorSet() == native.allocated[index] &&
                set.nativeHandle() == vk::DescriptorSet{native.allocated[index]} &&
                set.nativeLayout() == ordered[index]->nativeHandle() && set.correlatedWith(pool) &&
                set.correlatedWith(device) && !set.correlatedWith(other) &&
                set.nativePool() == pool.nativeHandle(),
            "ordered native sets and cached provenance");
      native.expected_free[index] = set.nativeDescriptorSet();
    }
    native.expected_layouts = {};
    native.expected_free_count = sets->size();
    check(pool.free(*sets).has_value(), "explicit batch free");
    native.expected_free_count = 0;
    for (const auto &set : *sets)
      check(emptyView(set), "successful free clears every cached identity");
    const auto frees_before = native.frees;
    check(hasError(pool.free(sets->front()), DescriptorPoolError::invalid_set) &&
              native.frees == frees_before,
          "double free rejected");

    auto single = pool.allocate(*empty);
    if (!check(single.has_value(), "single-layout convenience"))
      return false;
    // Deliberate contract assertions on a moved-from non-owning view.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    DescriptorSet moved_set = std::move(*single);
    check(emptyView(*single) && moved_set.valid(), "set move construction clears source");
    const auto moved_handle = moved_set.nativeDescriptorSet();
    auto displaced = pool.allocate(*empty);
    if (!displaced)
      return check(false, "set displacement fixture");
    *displaced = std::move(moved_set);
    check(emptyView(moved_set) && displaced->nativeDescriptorSet() == moved_handle &&
              native.frees == frees_before,
          "set move assignment never frees displaced native set");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    check(pool.reset().has_value() && displaced->valid(),
          "reset invalidates native sets, not unpassed cached views");
    *displaced = DescriptorSet{}; // discard stale view, never pass it back to Vulkan

    // Layout object need not survive allocation. Its identity is only a token.
    DescriptorSet retained;
    VkDescriptorSetLayout identity = VK_NULL_HANDLE;
    {
      auto temporary = createDescriptorSetLayout(device, {});
      if (!temporary)
        return check(false, "temporary layout");
      identity = temporary->nativeDescriptorSetLayout();
      auto allocated = pool.allocate(*temporary);
      if (!allocated)
        return check(false, "temporary layout allocation");
      retained = std::move(*allocated);
    }
    check(retained.valid() && retained.nativeDescriptorSetLayout() == identity &&
              pool.free(retained).has_value(),
          "layout destruction leaves cached provenance and set allocation usable");

    for (const auto injection : {Injection::error, Injection::partial_error}) {
      native.allocate = injection;
      const auto calls = native.calls();
      const auto failure = pool.allocate(ordered);
      check(hasError(failure, injection == Injection::error ? vk::Result::eErrorOutOfPoolMemory
                                                            : vk::Result::eErrorOutOfHostMemory) &&
                failure.error().context() == "vkAllocateDescriptorSets" &&
                pool.state() == DescriptorPoolState::ready && native.calls() == calls + 1,
            "atomic allocation failure preserves VkResult, ready state, no partial publication or "
            "cleanup");
    }
    for (const auto injection : {Injection::null_success, Injection::mixed_success}) {
      native.allocate = injection;
      const auto calls = native.calls();
      check(hasError(pool.allocate(ordered), DescriptorPoolError::native_null_handle) &&
                pool.state() == DescriptorPoolState::degraded && pool.valid() &&
                native.calls() == calls + 1,
            "any null success degrades without hidden cleanup");
      check(hasError(pool.allocate(*empty), DescriptorPoolError::degraded_pool) &&
                hasError(pool.free(invalid), DescriptorPoolError::degraded_pool) &&
                native.calls() == calls + 1,
            "degraded pool refuses allocate/free before native call");
      check(pool.reset().has_value() && pool.state() == DescriptorPoolState::ready,
            "explicit reset recovers null-success pool");
    }
    native.allocate = Injection::duplicate_success;
    auto duplicate = pool.allocate(ordered);
    if (!duplicate)
      return check(false, "duplicate-output test fixture");
    const auto duplicate_before = native.calls();
    check(hasError(pool.free(*duplicate), DescriptorPoolError::invalid_set) &&
              native.calls() == duplicate_before,
          "duplicate native handles rejected before free");
    check(pool.reset().has_value(), "discard duplicate fixture with explicit reset");
    duplicate->clear();

    for (const auto injection : {Injection::error, Injection::partial_error}) {
      auto batch = pool.allocate(ordered);
      if (!batch)
        return check(false, "free error fixture");
      const auto first = batch->front().nativeDescriptorSet();
      native.free = injection;
      const auto calls = native.calls();
      const auto failure = pool.free(*batch);
      check(hasError(failure, vk::Result::eErrorOutOfHostMemory) &&
                failure.error().context() == "vkFreeDescriptorSets" &&
                pool.state() == DescriptorPoolState::degraded &&
                batch->front().nativeDescriptorSet() == first && native.calls() == calls + 1,
            "uncertain/partial free preserves views and degrades without cleanup");
      check(hasError(pool.free(*batch), DescriptorPoolError::degraded_pool) &&
                hasError(pool.allocate(*empty), DescriptorPoolError::degraded_pool) &&
                native.calls() == calls + 1,
            "no retries on degraded pool");
      native.reset = injection;
      const auto reset_failure = pool.reset();
      check(hasError(reset_failure, vk::Result::eErrorOutOfHostMemory) &&
                reset_failure.error().context() == "vkResetDescriptorPool" &&
                pool.state() == DescriptorPoolState::degraded && batch->front().valid(),
            "uncertain reset preserves views and degraded state");
      check(pool.reset().has_value() && pool.state() == DescriptorPoolState::ready,
            "explicit recovery after failed reset");
      batch->clear();
    }
    native.reset = Injection::error;
    check(!pool.reset() && pool.state() == DescriptorPoolState::degraded,
          "reset failure also degrades ready pool");
    check(pool.reset().has_value(), "ready-state failure recovery");

    auto other_pool = createDescriptorPool(device, {1, free_bit, {}});
    auto foreign_pool = createDescriptorPool(other, {1, free_bit, {}});
    if (!other_pool || !foreign_pool)
      return check(false, "origin fixtures");
    auto own_set = pool.allocate(*empty);
    if (!own_set)
      return check(false, "origin set");
    const auto origin_before = native.calls();
    check(hasError(other_pool->free(*own_set), DescriptorPoolError::invalid_set) &&
              hasError(foreign_pool->free(*own_set), DescriptorPoolError::invalid_set) &&
              !own_set->correlatedWith(*other_pool) && native.calls() == origin_before,
          "exact native pool and Device origin required");
    check(pool.free(*own_set).has_value(), "origin set freed through allocating pool");

    const auto original = pool.nativeDescriptorPool();
    const auto displaced_pool = foreign_pool->nativeDescriptorPool();
    const auto destroys = native.destroys;
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    DescriptorPool moved = std::move(pool);
    check(!pool.valid() && !pool.configuration() && !pool.nativeDescriptorPool() &&
              !pool.nativeVkDevice() && pool.nativeHandle() == vk::DescriptorPool{} &&
              pool.nativeDevice() == vk::Device{} && !pool.correlatedWith(device) &&
              pool.state() == DescriptorPoolState::invalid_moved_from &&
              native.destroys == destroys,
          "pool move construction nulls source without destruction");
    const auto calls = native.calls();
    check(hasError(pool.allocate(*empty), DescriptorPoolError::invalid_pool) &&
              hasError(pool.free(invalid), DescriptorPoolError::invalid_pool) &&
              hasError(pool.reset(), DescriptorPoolError::invalid_pool) && native.calls() == calls,
          "moved-from operations make no native calls");
    *foreign_pool = std::move(moved);
    check(!moved.valid() && moved.state() == DescriptorPoolState::invalid_moved_from &&
              foreign_pool->nativeDescriptorPool() == original &&
              foreign_pool->correlatedWith(device) &&
              foreign_pool->configuration()->effective == requested &&
              native.destroys == destroys + 1 && native.last_destroyed == displaced_pool,
          "cross-Device pool move assignment destroys displaced owner exactly once");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  }
  check(native.destroys == destroy_before + 3 && native.empty(),
        "all three pool owners destroyed exactly once");
  {
    const DescriptorPoolDescription description{2, {}, {}};
    native.expected_description = &description;
    native.expected_device = static_cast<VkDevice>(device.nativeHandle());
    auto pool = createDescriptorPool(device, description);
    native.expected_description = nullptr;
    if (!pool)
      return check(false, "zero-size pool creation");
    expectPool(*pool);
    auto set = pool->allocate(*empty);
    if (!set)
      return check(false, "empty layout allocation from zero-size pool");
    const auto calls = native.calls();
    check(hasError(pool->free(*set), DescriptorPoolError::free_not_supported) &&
              native.calls() == calls && set->valid(),
          "free requires explicit FREE bit");
    check(pool->reset().has_value() && set->valid(),
          "reset works without FREE bit, caller discards stale view");
  }
  return failures == 0;
}

int exerciseDevices(Instance &instance) {
  const auto devices = queryPhysicalDevices(instance);
  if (!devices || devices->empty())
    return blocked("no Vulkan PhysicalDevice");
  for (const auto &candidate : *devices) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount)
        continue;
      vk::QueueFlags required;
      const auto flags = families[index].queueFlags;
      if (flags & vk::QueueFlagBits::eTransfer)
        required = vk::QueueFlagBits::eTransfer;
      else if (flags & vk::QueueFlagBits::eCompute)
        required = vk::QueueFlagBits::eCompute;
      else if (flags & vk::QueueFlagBits::eGraphics)
        required = vk::QueueFlagBits::eGraphics;
      else
        continue;
      DeviceDescription description;
      DeviceQueueRequest queue;
      queue.caller_id = "descriptor-pool-test";
      queue.required_flags = required;
      queue.allowed_family_indices = {index};
      description.queue_requests.push_back(queue);
      const auto plan = resolveDevice(candidate, description);
      if (!plan)
        continue;
      auto device = createDevice(instance, *plan);
      auto other = createDevice(instance, *plan);
      if (!device || !other)
        return blocked("two real logical Devices required");
      native.forbid = false;
      auto pool = createDescriptorPool(*device, {1, {}, {}});
      if (!pool) {
        const auto &error = pool.error();
        std::fprintf(stderr,
                     "%s: descriptor pool creation failed: code=%d category=%s context=%s\n",
                     test_name, error.code().value(), error.code().category().name(),
                     error.context().c_str());
        check(false, "real descriptor pool creation failed");
        return EXIT_FAILURE;
      }
      bool passed = false;
      // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      Device moved = std::move(*device);
      {
        DescriptorPool retained = std::move(*pool);
        check(retained.correlatedWith(moved) && !retained.correlatedWith(*device),
              "pool parent correlation follows Device wrapper move");
      }
      passed = poolTest(moved, *device, *other);
      // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      check(native.empty() && native.handles == native.destroys,
            "all native pools balanced before Device teardown");
      return passed && failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }
  return blocked("no usable queue family");
}

bool validationProbe(Instance &instance, ValidationRecorder &recorder) {
  const auto raw = static_cast<VkInstance>(instance.nativeHandle());
  const auto send = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
      vkGetInstanceProcAddr(raw, "vkSubmitDebugUtilsMessageEXT"));
  if (!check(send != nullptr, "validation callback entry point"))
    return false;
  VkDebugUtilsMessengerCallbackDataEXT data{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
  data.pMessageIdName = "terreate-descriptor-pool-validation-probe";
  data.messageIdNumber = 266;
  data.pMessage = "synthetic warning verifies native validation transport";
  const auto before = recorder.warnings_or_errors.load(std::memory_order_relaxed);
  send(raw, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
  if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == before + 1,
             "actual validation sink receives warning"))
    return false;
  recorder.warnings_or_errors.fetch_sub(1, std::memory_order_relaxed);
  return true;
}
int nativeTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan instance capabilities unavailable");
  bool has_validation = false, has_debug = false;
  for (const auto &layer : capabilities->available_layers)
    has_validation |= layer == "VK_LAYER_KHRONOS_validation";
  for (const auto &extension : capabilities->available_extensions)
    has_debug |= extension == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  if (!has_validation || !has_debug)
    return blocked("Validation Layer and Debug Utils required, never skipped");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan || !plan->debugUtilsEnabled())
    return blocked("Vulkan 1.3 validation-enabled Instance resolution");
  bool enabled = false;
  for (const auto &layer : plan->enabledLayers())
    enabled |= layer == "VK_LAYER_KHRONOS_validation";
  if (!enabled)
    return blocked("required validation layer absent from effective plan");
  ValidationRecorder recorder;
  {
    auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
    if (!instance)
      return blocked("headless Vulkan Instance creation");
    if (!validationProbe(*instance, recorder))
      return EXIT_FAILURE;
    const auto status = exerciseDevices(*instance);
    if (status != EXIT_SUCCESS)
      return status;
  }
  check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == 0,
        "zero validation warnings/errors including complete teardown");
  if (failures)
    return EXIT_FAILURE;
  std::puts("graphics.descriptor_pool: headless-native=PASS validation-layer=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return blocked("specify cpu or gpu mode");
  const std::string_view mode{argv[1]};
  if (mode == "cpu")
    return cpuTest() ? EXIT_SUCCESS : EXIT_FAILURE;
  if (mode == "gpu") {
    test_name = "graphics.descriptor_pool";
    return nativeTest();
  }
  return blocked("unknown test mode");
}
