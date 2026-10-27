#include <array>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>

#include <terreate/graphics/allocator.hpp>

// The native test target links with --wrap=vmaCreateAllocator and
// --wrap=vmaDestroyAllocator (GNU ld/lld).
// Only native failures are injected; successful calls always reach real VMA.
// These wrappers do not replace any Terreate-owned API symbol.
namespace {
std::optional<VkResult> injected_creation_failure;
unsigned creation_calls = 0;
unsigned destruction_calls = 0;
VmaAllocatorCreateFlags observed_flags = 0;
std::uint32_t observed_api_version = 0;
} // namespace

// GNU ld --wrap requires these reserved spellings; suppress only here.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vmaCreateAllocator(const VmaAllocatorCreateInfo *, VmaAllocator *);
extern "C" void __real_vmaDestroyAllocator(VmaAllocator);

extern "C" VkResult __wrap_vmaCreateAllocator(const VmaAllocatorCreateInfo *info,
                                              VmaAllocator *allocator) {
  ++creation_calls;
  observed_flags = info->flags;
  observed_api_version = info->vulkanApiVersion;
  if (injected_creation_failure) {
    *allocator = VK_NULL_HANDLE;
    const auto injected = injected_creation_failure.value_or(VK_ERROR_INITIALIZATION_FAILED);
    injected_creation_failure.reset();
    return injected;
  }
  return __real_vmaCreateAllocator(info, allocator);
}

extern "C" void __wrap_vmaDestroyAllocator(VmaAllocator allocator) {
  if (allocator != VK_NULL_HANDLE) {
    ++destruction_calls;
  }
  __real_vmaDestroyAllocator(allocator);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
using namespace terreate::graphics;

static_assert(!std::default_initializable<Allocator>);
static_assert(!std::copy_constructible<Allocator>);
static_assert(!std::is_copy_assignable_v<Allocator>);
static_assert(std::movable<Allocator>);
static_assert(std::is_nothrow_move_constructible_v<Allocator>);
static_assert(std::is_nothrow_move_assignable_v<Allocator>);

[[nodiscard]] bool check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "graphics.allocator: assertion=FAIL: %s\n", message);
  }
  return condition;
}

[[nodiscard]] bool test_allocator(const Instance &instance,
                                  const PhysicalDeviceCandidate &candidate, const Device &device,
                                  const Device &second_device) {
  bool passed = true;
  const auto before_rejections = creation_calls;
  // Synthetic requests against genuine parents: neither support alone nor an
  // allocator request may enable extensions or features on the existing Device.
  constexpr std::array rejected_flags{
      VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT, VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
      VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT,  VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT,
      VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT,
  };
  for (const auto flags : rejected_flags) {
    AllocatorDescription request;
    request.flags = flags;
    request.api_version = VK_API_VERSION_1_0;
    const auto rejected = createAllocator(instance, candidate, device, request);
    passed &= check(!rejected && rejected.error().code() ==
                                     make_error_code(AllocatorError::incompatible_flags),
                    "missing enabled prerequisites were not rejected");
    passed &= check(request.flags == flags && request.api_version == VK_API_VERSION_1_0,
                    "rejection mutated caller intent");
  }
  AllocatorDescription invalid_version;
  invalid_version.api_version = 0;
  const auto rejected_version = createAllocator(instance, candidate, device, invalid_version);
  passed &= check(!rejected_version && rejected_version.error().code() ==
                                           make_error_code(AllocatorError::unsupported_api_version),
                  "invalid explicit API version was accepted");
  passed &=
      check(creation_calls == before_rejections, "prerequisite rejection reached native creation");
  passed &= check(device.plan()->enabledExtensions().empty() &&
                      !device.plan()->enabledFeatures12().bufferDeviceAddress &&
                      !device.plan()->enabledFeatures13().maintenance4,
                  "allocator enabled unrequested parent capabilities");

  // Exercise the production createAllocator error conversion, not a locally
  // constructed Error. Injection ends after one call and never returns success.
  constexpr std::array failures{VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                VK_ERROR_INITIALIZATION_FAILED};
  for (const auto native_result : failures) {
    const auto before_create = creation_calls;
    const auto before_destroy = destruction_calls;
    injected_creation_failure = native_result;
    const auto failed = createAllocator(instance, candidate, device);
    const auto native_code = vk::make_error_code(static_cast<vk::Result>(native_result));
    passed &= check(!failed && failed.error().code() == native_code &&
                        failed.error().code().value() == static_cast<int>(native_result) &&
                        failed.error().code().category() == native_code.category(),
                    "native creation failure lost structured VkResult");
    passed &= check(!injected_creation_failure && creation_calls == before_create + 1 &&
                        destruction_calls == before_destroy,
                    "failure injection was not consumed or destroyed a nonexistent owner");
    injected_creation_failure.reset();
  }

  const auto before_destroy = destruction_calls;
  {
    AllocatorDescription first_request;
    AllocatorDescription second_request;
    second_request.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
    second_request.api_version = VK_API_VERSION_1_0;
    auto first = createAllocator(instance, candidate, device, first_request);
    auto second = createAllocator(instance, candidate, second_device, second_request);
    if (!check(first.has_value() && second.has_value(), "real VMA creation failed")) {
      return false;
    }
    passed &=
        check(first->valid() && second->valid() && first->nativeHandle() != second->nativeHandle(),
              "independent construction did not produce independent native owners");
    const auto *first_config = first->configuration();
    const auto *second_config = second->configuration();
    passed &= check(first_config && second_config && first_config != second_config &&
                        !first_config->requested.api_version &&
                        first_config->api_version == default_allocator_api_version &&
                        second_config->requested.api_version == VK_API_VERSION_1_0 &&
                        second_config->api_version == VK_API_VERSION_1_0 &&
                        first_config->flags == 0 && second_config->flags == second_request.flags,
                    "independent requested/effective configurations were not retained");
    passed &=
        check(first->effectiveFlags() == 0 && second->effectiveFlags() == second_request.flags &&
                  observed_flags == second->effectiveFlags() &&
                  observed_api_version == VK_API_VERSION_1_0,
              "observed effective flags differ from accepted requests");
    second_request.flags = 0;
    passed &= check(second->effectiveFlags() == VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT,
                    "configuration borrowed mutable request storage");
    VmaAllocatorInfo native_info{};
    vmaGetAllocatorInfo(first->nativeHandle(), &native_info);
    passed &= check(native_info.instance == static_cast<VkInstance>(instance.nativeHandle()) &&
                        native_info.device == static_cast<VkDevice>(device.nativeHandle()) &&
                        native_info.physicalDevice ==
                            static_cast<VkPhysicalDevice>(candidate.device().nativeHandle()),
                    "VMA native parent observation differs from borrowed parents");
    VmaAllocatorInfo second_native_info{};
    vmaGetAllocatorInfo(second->nativeHandle(), &second_native_info);
    passed &= check(second_native_info.instance == native_info.instance &&
                        second_native_info.physicalDevice == native_info.physicalDevice &&
                        second_native_info.device ==
                            static_cast<VkDevice>(second_device.nativeHandle()) &&
                        second_native_info.device != native_info.device,
                    "independent allocator does not match its distinct logical Device");
    const auto budgets = first->budgets();
    const auto statistics = first->statistics();
    passed &= check(budgets && statistics && !budgets->memory_budget_enabled &&
                        budgets->heap_count == first->memoryHeaps().size() &&
                        first->memoryTypes().size() ==
                            candidate.capabilities().memory_properties.memoryTypeCount,
                    "budget/statistics or physical memory visibility failed");
    for (const auto &type : first->memoryTypes()) {
      passed &=
          check(type.heapIndex < first->memoryHeaps().size(), "invalid memory type heap index");
    }

    // Native escape-hatch resources remain caller-owned and are released before
    // either allocator is moved or destroyed. No commands are submitted.
    VmaTotalStatistics before_allocation{};
    VmaTotalStatistics second_before_allocation{};
    vmaCalculateStatistics(first->nativeHandle(), &before_allocation);
    vmaCalculateStatistics(second->nativeHandle(), &second_before_allocation);
    passed &= check(before_allocation.total.statistics.allocationCount == 0 &&
                        second_before_allocation.total.statistics.allocationCount == 0,
                    "fresh allocators unexpectedly contain live allocations");
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = 256;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    const auto buffer_result = vmaCreateBuffer(first->nativeHandle(), &buffer_info,
                                               &allocation_info, &buffer, &allocation, nullptr);
    passed &= check(buffer_result == VK_SUCCESS && buffer != VK_NULL_HANDLE &&
                        allocation != VK_NULL_HANDLE,
                    "real VMA buffer/allocation creation failed");
    if (buffer_result == VK_SUCCESS) {
      VmaTotalStatistics during_allocation{};
      VmaTotalStatistics second_during_allocation{};
      vmaCalculateStatistics(first->nativeHandle(), &during_allocation);
      vmaCalculateStatistics(second->nativeHandle(), &second_during_allocation);
      passed &= check(during_allocation.total.statistics.allocationCount ==
                              before_allocation.total.statistics.allocationCount + 1 &&
                          during_allocation.total.statistics.allocationBytes >=
                              before_allocation.total.statistics.allocationBytes + buffer_info.size,
                      "native statistics did not observe the real buffer allocation");
      passed &= check(second_during_allocation.total.statistics.allocationCount ==
                              second_before_allocation.total.statistics.allocationCount &&
                          second_during_allocation.total.statistics.allocationBytes ==
                              second_before_allocation.total.statistics.allocationBytes,
                      "allocation affected the other Device's allocator statistics");
      vmaDestroyBuffer(first->nativeHandle(), buffer, allocation);
      VmaTotalStatistics after_release{};
      vmaCalculateStatistics(first->nativeHandle(), &after_release);
      passed &= check(after_release.total.statistics.allocationCount ==
                              before_allocation.total.statistics.allocationCount &&
                          after_release.total.statistics.allocationBytes ==
                              before_allocation.total.statistics.allocationBytes &&
                          destruction_calls == before_destroy,
                      "buffer release did not restore live allocation statistics "
                      "before owner destruction");
    }
    const auto first_handle = first->nativeHandle();
    Allocator moved = std::move(*first);
    passed &= check(!first->valid() && first->nativeHandle() == VK_NULL_HANDLE &&
                        first->configuration() == nullptr && moved.nativeHandle() == first_handle &&
                        destruction_calls == before_destroy,
                    "move construction duplicated or destroyed ownership");
    const auto moved_budget = first->budgets();
    passed &= check(!moved_budget && moved_budget.error().code() ==
                                         make_error_code(AllocatorError::invalid_allocator),
                    "moved-from observation did not report invalid allocator");
    *second = std::move(moved);
    // moved-from Allocator::valid() is explicitly a guaranteed observer.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!moved.valid() && second->nativeHandle() == first_handle &&
                        second->effectiveFlags() == 0 && destruction_calls == before_destroy + 1,
                    "move assignment did not destroy exactly the displaced owner");
  }
  passed &= check(destruction_calls == before_destroy + 2,
                  "scope exit did not destroy exactly two independently created allocators");
  return passed;
}

[[nodiscard]] bool test_headless_native_allocator() {
  const auto capabilities = queryInstanceCapabilities();
  if (!check(capabilities.has_value(), "Vulkan loader unavailable; native test cannot pass")) {
    return false;
  }
  InstanceDescription request;
  request.api_version = VK_API_VERSION_1_0;
  const auto plan = resolveInstance(request, *capabilities);
  if (!check(plan.has_value(), "headless instance resolution failed")) {
    return false;
  }
  auto instance = createInstance(*plan);
  if (!check(instance.has_value(), "headless instance creation failed")) {
    return false;
  }
  const auto inventory = queryPhysicalDevices(*instance);
  if (!check(inventory && !inventory->empty(), "no physical device; native test cannot pass")) {
    return false;
  }
  const auto &candidate = inventory->front();
  const auto &families = candidate.capabilities().queue_families;
  DeviceDescription device_request;
  for (std::size_t index = 0; index < families.size(); ++index) {
    const auto usable =
        families[index].queueFlags &
        (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer);
    if (families[index].queueCount != 0 && static_cast<bool>(usable)) {
      DeviceQueueRequest queue;
      queue.caller_id = "allocator-test";
      queue.required_flags = usable;
      queue.allowed_family_indices = {index};
      device_request.queue_requests.push_back(std::move(queue));
      break;
    }
  }
  const auto device_plan = resolveDevice(candidate, device_request);
  if (!check(device_plan.has_value(), "no usable headless device plan")) {
    return false;
  }
  auto device = createDevice(*instance, *device_plan);
  if (!check(device.has_value(), "headless Device creation failed")) {
    return false;
  }
  auto second_device = createDevice(*instance, *device_plan);
  if (!check(second_device.has_value(), "second headless Device creation failed")) {
    return false;
  }
  if (!check(device->nativeHandle() != second_device->nativeHandle(),
             "concurrently live logical Devices are not distinct")) {
    return false;
  }
  // Both Devices use the same selected physical GPU. No work is submitted:
  // no waitIdle is needed. All allocator children die in the callee while both
  // Devices are still live, then Devices here, then inventory and Instance.
  const auto before_destroy = destruction_calls;
  bool passed = test_allocator(*instance, candidate, *device, *second_device);
  passed &= check(device->nativeHandle() != vk::Device{} &&
                      second_device->nativeHandle() != vk::Device{} &&
                      destruction_calls == before_destroy + 2,
                  "allocator children were not destroyed before their live Device parents");
  return passed;
}
} // namespace

int main() {
  const bool passed = test_headless_native_allocator();
  if (passed) {
    std::puts("graphics.allocator: headless-allocator=PASS");
  } else {
    std::fputs("graphics.allocator: headless-allocator=BLOCKED; native coverage not established\n",
               stderr);
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
