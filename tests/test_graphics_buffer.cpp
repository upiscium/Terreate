#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include <terreate/graphics/buffer.hpp>

// Observe the real VMA boundary. Successful creation and every destruction
// always reach VMA; only explicit native error results may be injected.
namespace {
struct NativeOwner {
  VmaAllocator allocator = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
};
std::array<NativeOwner, 16> live_owners{};
unsigned creation_calls = 0;
unsigned destruction_calls = 0;
bool ownership_violation = false;
std::optional<VkResult> injected_failure;
VkBufferCreateInfo observed_buffer_info{};
VmaAllocationCreateInfo observed_allocation_info{};
} // namespace

// GNU ld's --wrap protocol requires these reserved spellings.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vmaCreateBuffer(VmaAllocator, const VkBufferCreateInfo *,
                                           const VmaAllocationCreateInfo *, VkBuffer *,
                                           VmaAllocation *, VmaAllocationInfo *);
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);

extern "C" VkResult __wrap_vmaCreateBuffer(VmaAllocator allocator,
                                           const VkBufferCreateInfo *buffer_info,
                                           const VmaAllocationCreateInfo *allocation_info,
                                           VkBuffer *buffer, VmaAllocation *allocation,
                                           VmaAllocationInfo *info) {
  ++creation_calls;
  observed_buffer_info = *buffer_info;
  observed_allocation_info = *allocation_info;
  if (injected_failure) {
    const auto result = injected_failure.value_or(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    injected_failure.reset();
    *buffer = VK_NULL_HANDLE;
    *allocation = VK_NULL_HANDLE;
    return result;
  }
  const auto result =
      __real_vmaCreateBuffer(allocator, buffer_info, allocation_info, buffer, allocation, info);
  if (result == VK_SUCCESS) {
    for (auto &owner : live_owners) {
      if (owner.buffer == VK_NULL_HANDLE) {
        owner = {allocator, *buffer, *allocation};
        return result;
      }
    }
    ownership_violation = true;
  }
  return result;
}

extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  if (buffer != VK_NULL_HANDLE || allocation != VK_NULL_HANDLE) {
    ++destruction_calls;
    bool found = false;
    for (auto &owner : live_owners) {
      if (owner.buffer == buffer && owner.allocation == allocation &&
          owner.allocator == allocator && buffer != VK_NULL_HANDLE) {
        owner = {};
        found = true;
        break;
      }
    }
    ownership_violation |= !found;
  }
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
using namespace terreate::graphics;

static_assert(!std::copy_constructible<Buffer>);
static_assert(!std::is_copy_assignable_v<Buffer>);
static_assert(std::movable<Buffer>);
static_assert(std::is_nothrow_move_constructible_v<Buffer>);
static_assert(std::is_nothrow_move_assignable_v<Buffer>);

[[nodiscard]] bool check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "graphics.buffer: assertion=FAIL: %s\n", message);
  }
  return condition;
}

[[nodiscard]] bool blocked(const char *message) {
  std::fprintf(stderr, "graphics.buffer: validation=BLOCKED: %s\n", message);
  return false;
}

[[nodiscard]] bool test_buffers(Allocator &allocator) {
  bool passed = true;
  BufferDescription request;
  request.size = 256;
  request.usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
  request.allocation.usage = VMA_MEMORY_USAGE_AUTO;
  request.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  request.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;

  const auto before_rejection = creation_calls;
  auto invalid_request = request;
  invalid_request.size = 0;
  passed &= check(!createBuffer(allocator, invalid_request), "zero size accepted");
  invalid_request = request;
  invalid_request.usage = {};
  passed &= check(!createBuffer(allocator, invalid_request), "empty usage accepted");
  passed &=
      check(creation_calls == before_rejection, "invalid descriptions reached native creation");

  // Native failures are injected at the external VMA boundary, never success.
  for (const auto result : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
    injected_failure = result;
    const auto before_destroy = destruction_calls;
    const auto failed = createBuffer(allocator, request);
    const auto code = vk::make_error_code(static_cast<vk::Result>(result));
    passed &= check(!failed && failed.error().code() == code &&
                        failed.error().code().value() == static_cast<int>(result) &&
                        failed.error().code().category() == code.category(),
                    "production creation lost structured native VkResult");
    passed &= check(!injected_failure && destruction_calls == before_destroy,
                    "failed creation destroyed a nonexistent owner");
    injected_failure.reset();
  }

  VmaTotalStatistics baseline{};
  vmaCalculateStatistics(allocator.nativeHandle(), &baseline);
  const auto before_destroy = destruction_calls;
  {
    auto first = createBuffer(allocator, request);
    auto second = createBuffer(allocator, request);
    if (!first || !second) {
      return blocked("real host-visible Buffer allocation unavailable");
    }
    passed &= check(first->valid() && second->valid() && first->nativeHandle() != vk::Buffer{} &&
                        first->nativeHandle() != second->nativeHandle(),
                    "explicit creation did not produce distinct native owners");
    const auto *configuration = first->configuration();
    if (!check(configuration != nullptr, "live Buffer has no configuration")) {
      return false;
    }
    passed &=
        check(configuration->requested.size == request.size &&
                  configuration->requested.usage == request.usage &&
                  configuration->size == request.size && configuration->usage == request.usage,
              "requested/effective size or usage differs");
    passed &=
        check(observed_buffer_info.size == request.size &&
                  observed_buffer_info.usage == static_cast<VkBufferUsageFlags>(request.usage) &&
                  observed_allocation_info.usage == request.allocation.usage &&
                  observed_allocation_info.flags == request.allocation.flags &&
                  observed_allocation_info.requiredFlags ==
                      static_cast<VkMemoryPropertyFlags>(request.allocation.required_flags),
              "native creation changed requested allocation policy");
    request.size = 128;
    passed &= check(configuration->requested.size == 256 && configuration->size == 256,
                    "configuration borrowed mutable caller description");
    request.size = 256;

    const auto allocation = first->nativeAllocation();
    VmaAllocationInfo native_info{};
    vmaGetAllocationInfo(allocator.nativeHandle(), allocation, &native_info);
    VkMemoryPropertyFlags properties = 0;
    vmaGetAllocationMemoryProperties(allocator.nativeHandle(), allocation, &properties);
    passed &= check(allocation != VK_NULL_HANDLE && native_info.deviceMemory != VK_NULL_HANDLE &&
                        native_info.size >= request.size &&
                        (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
                    "borrowed allocation/memory observations are invalid");
    const auto observed = first->allocationInfo();
    passed &= check(observed && observed->native.deviceMemory == native_info.deviceMemory &&
                        observed->native.offset == native_info.offset &&
                        observed->native.size == native_info.size &&
                        observed->memory_properties == vk::MemoryPropertyFlags{properties},
                    "borrowed allocation information differs from native VMA memory");
    VmaTotalStatistics live{};
    vmaCalculateStatistics(allocator.nativeHandle(), &live);
    passed &= check(live.total.statistics.allocationCount ==
                        baseline.total.statistics.allocationCount + 2,
                    "two Buffer owners did not own two live allocations");

    // A mapping is a borrowed CPU view, not an upload or GPU synchronization.
    // All accesses below happen while the owner and every parent remain live.
    auto mapping = first->map();
    if (!mapping) {
      return blocked("genuine host-visible mapping unavailable");
    }
    auto *bytes = static_cast<unsigned char *>(*mapping);
    if (!check(bytes != nullptr, "successful map returned null")) {
      return false;
    }
    for (std::size_t index = 0; index < 256; ++index) {
      bytes[index] = static_cast<unsigned char>(index ^ 0xA5U);
    }
    passed &= check(first->flush(3, 17).has_value(), "unaligned allocation-relative flush failed");
    passed &= check(first->flush().has_value(), "whole allocation flush failed");
    passed &= check(first->invalidate(3, 17).has_value(),
                    "unaligned allocation-relative invalidate failed");
    passed &= check(first->invalidate().has_value(), "whole allocation invalidate failed");
    for (std::size_t index = 0; index < 256; ++index) {
      passed &= check(bytes[index] == static_cast<unsigned char>(index ^ 0xA5U),
                      "genuine mapped read/write round trip differs");
    }
    passed &= check(!first->flush(257, 1) && !first->invalidate(257, 1) && !first->flush(255, 2) &&
                        !first->invalidate(255, 2) &&
                        !first->flush(3, std::numeric_limits<vk::DeviceSize>::max() - 1) &&
                        !first->invalidate(3, std::numeric_limits<vk::DeviceSize>::max() - 1),
                    "out-of-bounds/overflowing mapped range accepted");
    passed &= check(first->unmap().has_value(), "explicit unmap failed");
    // bytes is expired now and is never read again, including after moves.
    passed &= check(!first->unmap(), "unmap without a mapping accepted");
    auto remapping = first->map();
    if (!check(remapping.has_value(), "remap after explicit unmap failed")) {
      return false;
    }
    passed &= check(static_cast<unsigned char *>(*remapping)[42] ==
                        static_cast<unsigned char>(42U ^ 0xA5U),
                    "remap did not retain CPU data");

    // Transfer CPU ownership while mapped. Do not reuse the borrowed remapping
    // view after the move; the new owner must be able to release that mapping.
    const auto handle = first->nativeHandle();
    Buffer moved = std::move(*first);
    passed &=
        check(!first->valid() && first->nativeHandle() == vk::Buffer{} &&
                  first->nativeAllocation() == VK_NULL_HANDLE &&
                  first->configuration() == nullptr && moved.nativeHandle() == handle &&
                  moved.nativeAllocation() == allocation && destruction_calls == before_destroy,
              "move construction duplicated/destroyed coupled ownership");
    passed &= check(moved.unmap().has_value(), "mapped ownership did not follow move construction");
    passed &= check(!first->map() && !first->unmap() && !first->flush() && !first->invalidate(),
                    "moved-from operations did not return errors");
    *second = std::move(moved);
    // Moved-from observations/operations are explicitly part of the contract.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!moved.valid() && second->nativeHandle() == handle &&
                        second->nativeAllocation() == allocation &&
                        destruction_calls == before_destroy + 1,
                    "move assignment failed to destroy only the displaced owner");
  }
  VmaTotalStatistics after{};
  vmaCalculateStatistics(allocator.nativeHandle(), &after);
  passed &= check(
      !ownership_violation && destruction_calls == before_destroy + 2 &&
          after.total.statistics.allocationCount == baseline.total.statistics.allocationCount &&
          after.total.statistics.allocationBytes == baseline.total.statistics.allocationBytes,
      "exactly-once destruction did not restore live allocation statistics");

  // Some UMA devices expose only host-visible types. Such a topology cannot
  // exercise this optional negative branch; it is reported, never fabricated.
  bool tested_non_host_visible = false;
  const auto types = allocator.memoryTypes();
  for (std::size_t index = 0; index < types.size(); ++index) {
    if (static_cast<bool>(types[index].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible)) {
      continue;
    }
    auto device_request = request;
    device_request.allocation.flags = 0;
    device_request.allocation.required_flags = vk::MemoryPropertyFlags{};
    device_request.allocation.memory_type_bits = std::uint32_t{1} << index;
    auto device_buffer = createBuffer(allocator, device_request);
    if (!device_buffer) {
      continue; // A physical type need not be compatible with this buffer.
    }
    tested_non_host_visible = true;
    passed &= check(!device_buffer->map() && !device_buffer->flush() &&
                        !device_buffer->invalidate() && !device_buffer->unmap(),
                    "non-host-visible allocation accepted host access");
    break;
  }
  if (!tested_non_host_visible) {
    std::puts("graphics.buffer: non-host-visible=UNAVAILABLE (optional memory topology)");
  }
  passed &= check(!ownership_violation, "native Buffer/allocation ownership pair mismatch");
  // Moving a parent is only exercised after every child and mapping is gone.
  // A borrowed, moved-from allocator must be rejected before reaching VMA.
  Allocator moved_allocator = std::move(allocator);
  const auto before_invalid_parent = creation_calls;
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  passed &= check(!createBuffer(allocator, request) && creation_calls == before_invalid_parent,
                  "moved-from Allocator accepted Buffer creation");
  allocator = std::move(moved_allocator);
  return passed;
}

[[nodiscard]] bool test_headless() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities) {
    return blocked("Vulkan loader capabilities unavailable");
  }
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_0;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan) {
    return blocked("headless Instance resolution unavailable");
  }
  auto instance = createInstance(*plan);
  if (!instance) {
    return blocked("headless Instance creation unavailable");
  }
  const auto inventory = queryPhysicalDevices(*instance);
  if (!inventory || inventory->empty()) {
    return blocked("no physical device for genuine native coverage");
  }
  // Prefer a physical device with host-visible memory rather than assuming
  // discrete, unified, coherent, or non-coherent memory topology.
  for (const auto &candidate : *inventory) {
    const auto &memory = candidate.capabilities().memory_properties;
    bool host_visible = false;
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
      host_visible |= static_cast<bool>(memory.memoryTypes[index].propertyFlags &
                                        vk::MemoryPropertyFlagBits::eHostVisible);
    }
    if (!host_visible) {
      continue;
    }
    DeviceDescription device_description;
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      const auto flags =
          families[index].queueFlags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                                        vk::QueueFlagBits::eTransfer);
      if (families[index].queueCount != 0 && static_cast<bool>(flags)) {
        DeviceQueueRequest queue;
        queue.caller_id = "buffer-test";
        queue.required_flags = flags;
        queue.allowed_family_indices = {index};
        device_description.queue_requests.push_back(std::move(queue));
        break;
      }
    }
    if (device_description.queue_requests.empty()) {
      continue;
    }
    const auto device_plan = resolveDevice(candidate, device_description);
    if (!device_plan) {
      continue;
    }
    auto device = createDevice(*instance, *device_plan);
    if (!device) {
      continue;
    }
    auto allocator = createAllocator(*instance, candidate, *device);
    if (!allocator) {
      continue;
    }
    // No commands are submitted. Every mapping and Buffer is released inside
    // the callee before Allocator, Device, inventory and Instance die here.
    // A CPU move/destructor cannot establish completion of application GPU use;
    // callers must establish that separately. No waitIdle/retirement is needed.
    return test_buffers(*allocator);
  }
  return blocked("no usable device with essential host-visible memory");
}
} // namespace

int main() {
  const bool passed = test_headless();
  if (passed) {
    std::puts("graphics.buffer: headless-buffer=PASS");
  } else {
    std::fputs("graphics.buffer: headless-buffer=BLOCKED; native coverage not established\n",
               stderr);
  }
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
