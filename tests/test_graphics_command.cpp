#include <bit>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <utility>

#include <terreate/graphics/command_pool.hpp>

namespace {
using namespace terreate::graphics;

std::size_t pool_creates = 0;
std::size_t pool_destroys = 0;
std::size_t buffer_allocations = 0;
std::size_t buffer_begins = 0;
std::size_t buffer_ends = 0;
std::size_t buffer_resets = 0;
std::size_t pool_resets = 0;
std::size_t device_waits = 0;
std::optional<VkResult> create_failure{};
std::optional<VkResult> allocation_failure{};
bool create_null_success = false;
bool allocation_null_success = false;
std::optional<VkResult> begin_failure{};
std::optional<VkResult> end_failure{};
std::optional<VkResult> buffer_reset_failure{};
std::optional<VkResult> pool_reset_failure{};
VkCommandPoolCreateInfo observed_pool{};

static_assert(!std::default_initializable<CommandPool>);
static_assert(!std::copy_constructible<CommandPool>);
static_assert(std::movable<CommandPool>);
static_assert(std::is_nothrow_move_constructible_v<CommandPool>);
static_assert(std::is_nothrow_move_assignable_v<CommandPool>);
static_assert(!std::default_initializable<CommandBuffer>);
static_assert(!std::copy_constructible<CommandBuffer>);
static_assert(std::movable<CommandBuffer>);
static_assert(std::is_nothrow_move_constructible_v<CommandBuffer>);
static_assert(std::is_nothrow_move_assignable_v<CommandBuffer>);
static_assert(!std::is_convertible_v<CommandBuffer, VkCommandBuffer>);
static_assert(!std::is_convertible_v<CommandPool, VkCommandPool>);

bool check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "graphics.command: assertion=FAIL: %s\n", message);
  }
  return value;
}
bool blocked(const char *message) {
  std::fprintf(stderr, "graphics.command: validation=BLOCKED: %s\n", message);
  return false;
}
bool nativeCode(const terreate::Error &error, VkResult result) {
  return error.code() == vk::make_error_code(static_cast<vk::Result>(result));
}
} // namespace

// GNU ld --wrap intercepts real native calls without creating fake successful handles.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" VkResult __real_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *,
                                               const VkAllocationCallbacks *, VkCommandPool *);
extern "C" void __real_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);
extern "C" VkResult __real_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *,
                                                    VkCommandBuffer *);
extern "C" VkResult __real_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
extern "C" VkResult __real_vkEndCommandBuffer(VkCommandBuffer);
extern "C" VkResult __real_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);
extern "C" VkResult __real_vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);

extern "C" VkResult __wrap_vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *info,
                                               const VkAllocationCallbacks *allocator,
                                               VkCommandPool *pool) {
  ++pool_creates;
  observed_pool = *info;
  if (create_failure) {
    const auto result = *create_failure;
    create_failure.reset();
    *pool = VK_NULL_HANDLE;
    return result;
  }
  if (create_null_success) {
    create_null_success = false;
    *pool = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }
  return __real_vkCreateCommandPool(device, info, allocator, pool);
}
extern "C" void __wrap_vkDestroyCommandPool(VkDevice device, VkCommandPool pool,
                                            const VkAllocationCallbacks *allocator) {
  ++pool_destroys;
  __real_vkDestroyCommandPool(device, pool, allocator);
}
extern "C" VkResult __wrap_vkAllocateCommandBuffers(VkDevice device,
                                                    const VkCommandBufferAllocateInfo *info,
                                                    VkCommandBuffer *buffers) {
  ++buffer_allocations;
  if (allocation_failure) {
    const auto result = *allocation_failure;
    allocation_failure.reset();
    *buffers = VK_NULL_HANDLE;
    return result;
  }
  if (allocation_null_success) {
    allocation_null_success = false;
    *buffers = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }
  return __real_vkAllocateCommandBuffers(device, info, buffers);
}
extern "C" VkResult __wrap_vkBeginCommandBuffer(VkCommandBuffer buffer,
                                                const VkCommandBufferBeginInfo *info) {
  ++buffer_begins;
  if (begin_failure) {
    const auto result = *begin_failure;
    begin_failure.reset();
    return result;
  }
  return __real_vkBeginCommandBuffer(buffer, info);
}
extern "C" VkResult __wrap_vkEndCommandBuffer(VkCommandBuffer buffer) {
  ++buffer_ends;
  if (end_failure) {
    const auto result = *end_failure;
    end_failure.reset();
    return result;
  }
  return __real_vkEndCommandBuffer(buffer);
}
extern "C" VkResult __wrap_vkResetCommandBuffer(VkCommandBuffer buffer,
                                                VkCommandBufferResetFlags flags) {
  ++buffer_resets;
  if (buffer_reset_failure) {
    const auto result = *buffer_reset_failure;
    buffer_reset_failure.reset();
    return result;
  }
  return __real_vkResetCommandBuffer(buffer, flags);
}
extern "C" VkResult __wrap_vkResetCommandPool(VkDevice device, VkCommandPool pool,
                                              VkCommandPoolResetFlags flags) {
  ++pool_resets;
  if (pool_reset_failure) {
    const auto result = *pool_reset_failure;
    pool_reset_failure.reset();
    return result;
  }
  return __real_vkResetCommandPool(device, pool, flags);
}
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++device_waits;
  return __real_vkDeviceWaitIdle(device);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
bool testCommandLifecycle(Device &device, std::uint32_t family) {
  bool passed = true;
  CommandPoolDescription description;
  description.queue_family_index = family;
  description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;

  const auto invalidFamily = createCommandPool(device, CommandPoolDescription{});
  passed &= check(!invalidFamily && invalidFamily.error().code() ==
                                        make_error_code(CommandError::invalid_queue_family),
                  "an unspecified queue family was accepted");

  auto invalidFlags = description;
  invalidFlags.flags = std::bit_cast<vk::CommandPoolCreateFlags>(std::uint32_t{0x80000000u});
  const auto countBeforeFlags = pool_creates;
  const auto unknownFlags = createCommandPool(device, invalidFlags);
  passed &=
      check(!unknownFlags &&
                unknownFlags.error().code() == make_error_code(CommandError::unsupported_flags) &&
                pool_creates == countBeforeFlags,
            "unknown pool flags reached Vulkan");

  create_failure = VK_ERROR_OUT_OF_HOST_MEMORY;
  const auto failedCreate = createCommandPool(device, description);
  passed &= check(!failedCreate && nativeCode(failedCreate.error(), VK_ERROR_OUT_OF_HOST_MEMORY) &&
                      !create_failure,
                  "native create error was not preserved");

  // Malformed synthetic native success is not a valid owning resource.
  // Conforming Vulkan implementations must return a non-null handle.
  const auto destroysBeforeNull = pool_destroys;
  create_null_success = true;
  const auto nullSuccessPool = createCommandPool(device, description);
  passed &= check(!nullSuccessPool &&
                      nullSuccessPool.error().code() ==
                          make_error_code(CommandError::invalid_native_handle) &&
                      !create_null_success && pool_destroys == destroysBeforeNull,
                  "malformed success published a null CommandPool");

  const auto destroysBeforeScope = pool_destroys;
  {
    auto pool = createCommandPool(device, description);
    if (!pool)
      return blocked("real vkCreateCommandPool unavailable");
    passed &=
        check(pool->valid() && pool->nativeHandle() != vk::CommandPool{} && pool->requested() &&
                  pool->effective() && pool->requested()->queue_family_index == family &&
                  pool->effective()->flags == description.flags &&
                  observed_pool.queueFamilyIndex == family &&
                  observed_pool.flags == static_cast<VkCommandPoolCreateFlags>(description.flags),
              "pool native handle or requested/effective flags/family lost");

    allocation_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto failedAllocate = pool->allocatePrimary();
    passed &= check(!failedAllocate &&
                        nativeCode(failedAllocate.error(), VK_ERROR_OUT_OF_DEVICE_MEMORY) &&
                        !allocation_failure,
                    "native command allocation error was not preserved");

    allocation_null_success = true;
    const auto nullSuccessBuffer = pool->allocatePrimary();
    passed &= check(!nullSuccessBuffer &&
                        nullSuccessBuffer.error().code() ==
                            make_error_code(CommandError::invalid_native_handle) &&
                        !allocation_null_success,
                    "malformed success published a null CommandBuffer");

    auto first = pool->allocatePrimary();
    auto second = pool->allocatePrimary();
    if (!first || !second)
      return blocked("real vkAllocateCommandBuffers unavailable");
    passed &=
        check(first->valid() && second->valid() && first->nativeHandle() != vk::CommandBuffer{} &&
                  first->nativeHandle() != second->nativeHandle() &&
                  first->state() == CommandBufferState::initial,
              "primary allocation/ownership or initial state invalid");

    const auto beforeEnd = buffer_ends;
    auto invalidEnd = first->end();
    passed &=
        check(!invalidEnd &&
                  invalidEnd.error().code() == make_error_code(CommandError::invalid_transition) &&
                  buffer_ends == beforeEnd,
              "end before begin called Vulkan");
    const auto beforeBegin = buffer_begins;
    const auto invalidBegin = first->begin(vk::CommandBufferUsageFlagBits::eRenderPassContinue);
    passed &=
        check(!invalidBegin &&
                  invalidBegin.error().code() == make_error_code(CommandError::unsupported_flags) &&
                  buffer_begins == beforeBegin,
              "unsupported begin flag called Vulkan");

    auto begun = first->begin();
    if (!begun)
      return blocked("native vkBeginCommandBuffer unavailable");
    passed &= check(first->state() == CommandBufferState::recording,
                    "begin failed to transition to recording");
    const auto repeat = first->begin();
    passed &= check(
        !repeat && repeat.error().code() == make_error_code(CommandError::invalid_transition) &&
            buffer_begins == beforeBegin + 1,
        "double begin called Vulkan");
    auto ended = first->end();
    passed &= check(ended && first->state() == CommandBufferState::executable,
                    "end failed to transition to executable");
    const auto endAgain = first->end();
    passed &= check(!endAgain && endAgain.error().code() ==
                                     make_error_code(CommandError::invalid_transition),
                    "double end accepted");

    auto secondBegin = second->begin();
    passed &= check(secondBegin && second->state() == CommandBufferState::recording,
                    "secondary primary buffer begin failed");
    const auto beforeReset = buffer_resets;
    const auto invalidReset = second->reset();
    passed &= check(!invalidReset &&
                        invalidReset.error().code() ==
                            make_error_code(CommandError::invalid_transition) &&
                        buffer_resets == beforeReset,
                    "reset while recording called Vulkan");

    // A real pool reset changes every tracked allocated buffer, including recording.
    auto poolReset = pool->reset();
    passed &=
        check(poolReset && first->state() == CommandBufferState::initial &&
                  second->state() == CommandBufferState::initial && buffer_resets == beforeReset,
              "pool reset did not synchronize all buffer state observations");
    passed &= check(first->reset() && first->state() == CommandBufferState::initial,
                    "individual reset on reset-enabled pool failed");

    // A native failure makes wrapper observation invalid; a later successful
    // caller-controlled pool reset may restore its tracked initial state.
    begin_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto failedBegin = first->begin();
    passed &=
        check(!failedBegin && nativeCode(failedBegin.error(), VK_ERROR_OUT_OF_DEVICE_MEMORY) &&
                  first->state() == CommandBufferState::invalid && !begin_failure,
              "native begin failure did not invalidate wrapper");
    passed &= check(pool->reset() && first->state() == CommandBufferState::initial,
                    "pool reset did not recover after injected begin failure");

    passed &=
        check(static_cast<bool>(first->begin()), "begin before injected end failure unavailable");
    end_failure = VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto failedEnd = first->end();
    passed &= check(!failedEnd && nativeCode(failedEnd.error(), VK_ERROR_OUT_OF_HOST_MEMORY) &&
                        first->state() == CommandBufferState::invalid && !end_failure,
                    "native end failure did not invalidate wrapper");
    passed &= check(pool->reset() && first->state() == CommandBufferState::initial,
                    "pool reset did not recover after injected end failure");

    buffer_reset_failure = VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto failedReset = first->reset();
    passed &= check(!failedReset && nativeCode(failedReset.error(), VK_ERROR_OUT_OF_HOST_MEMORY) &&
                        first->state() == CommandBufferState::invalid && !buffer_reset_failure,
                    "native buffer reset failure did not invalidate wrapper");
    passed &= check(pool->reset() && first->state() == CommandBufferState::initial,
                    "pool reset failed to recover after buffer reset failure");

    pool_reset_failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto failedPoolReset = pool->reset();
    passed &= check(!failedPoolReset &&
                        nativeCode(failedPoolReset.error(), VK_ERROR_OUT_OF_DEVICE_MEMORY) &&
                        first->state() == CommandBufferState::invalid &&
                        second->state() == CommandBufferState::invalid && !pool_reset_failure,
                    "native pool reset failure did not invalidate all records");
    passed &= check(pool->reset() && first->state() == CommandBufferState::initial,
                    "pool reset failure did not permit caller-controlled recovery");

    const auto raw = first->nativeHandle();
    CommandBuffer movedBuffer = std::move(*first);
    passed &= check(!first->valid() && first->nativeHandle() == vk::CommandBuffer{} &&
                        movedBuffer.valid() && movedBuffer.nativeHandle() == raw,
                    "move did not transfer borrowed buffer view");
    auto abandonedRecord = pool->allocatePrimary();
    if (!abandonedRecord)
      return blocked("could not allocate replacement command view");
    *abandonedRecord = std::move(movedBuffer);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!movedBuffer.valid() && abandonedRecord->nativeHandle() == raw,
                    "buffer move assignment did not abandon destination view correctly");

    CommandPool movedPool = std::move(*pool);
    passed &= check(!pool->valid() && pool->nativeHandle() == vk::CommandPool{} &&
                        movedPool.valid() && abandonedRecord->valid(),
                    "pool move broke stable allocated buffer views");
    const auto invalidMovedPool = pool->reset();
    passed &= check(!invalidMovedPool && invalidMovedPool.error().code() ==
                                             make_error_code(CommandError::invalid_pool),
                    "moved-from pool accepted native reset");

    auto replacementPool = createCommandPool(device, description);
    if (!replacementPool)
      return blocked("could not create replacement command pool");
    const auto destructBeforeMove = pool_destroys;
    *replacementPool = std::move(movedPool);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    passed &= check(!movedPool.valid() && pool_destroys == destructBeforeMove + 1 &&
                        replacementPool->valid() && abandonedRecord->valid(),
                    "pool move assignment failed to destroy only displaced pool");
    passed &=
        check(replacementPool->reset() && abandonedRecord->state() == CommandBufferState::initial,
              "source views did not survive owning pool displacement");

    CommandPoolDescription noIndividualReset;
    noIndividualReset.queue_family_index = family;
    auto noResetPool = createCommandPool(device, noIndividualReset);
    if (!noResetPool)
      return blocked("could not create non-resettable command pool");
    auto noResetBuffer = noResetPool->allocatePrimary();
    if (!noResetBuffer)
      return blocked("could not allocate non-resettable command buffer");
    const auto beforeDisabledReset = buffer_resets;
    const auto unsupportedReset = noResetBuffer->reset();
    passed &= check(!unsupportedReset &&
                        unsupportedReset.error().code() ==
                            make_error_code(CommandError::reset_not_enabled) &&
                        beforeDisabledReset == buffer_resets,
                    "pool without reset flag permitted individual buffer reset");
    passed &= check(noResetPool->reset() && noResetBuffer->state() == CommandBufferState::initial,
                    "whole-pool reset improperly required buffer reset flag");
  }

  // Includes the main pool, one displaced destination and the separate
  // non-resettable pool; native buffers are owned and freed by their pools.
  passed &= check(pool_destroys == destroysBeforeScope + 3,
                  "native pool ownership leaked/double-destroyed");
  passed &= check(device_waits == 0, "library unexpectedly waited for device idle");
  std::printf("graphics.command: native-calls=create:%zu destroy:%zu allocate:%zu begin:%zu "
              "end:%zu reset-buffer:%zu reset-pool:%zu waits:%zu\n",
              pool_creates, pool_destroys, buffer_allocations, buffer_begins, buffer_ends,
              buffer_resets, pool_resets, device_waits);
  return passed;
}

bool runHeadless() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader unavailable");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_0;
  const auto instancePlan = resolveInstance(description, *capabilities);
  if (!instancePlan)
    return blocked("headless Vulkan Instance resolution unavailable");
  auto instance = createInstance(*instancePlan);
  if (!instance)
    return blocked("headless Vulkan Instance unavailable");
  const auto inventory = queryPhysicalDevices(*instance);
  if (!inventory || inventory->empty())
    return blocked("headless physical device unavailable");

  for (const auto &candidate : *inventory) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t family = 0; family < families.size(); ++family) {
      const auto flags = families[family].queueFlags &
                         (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                          vk::QueueFlagBits::eTransfer);
      if (!families[family].queueCount || !flags)
        continue;
      DeviceDescription deviceDescription;
      DeviceQueueRequest request;
      request.caller_id = "command-test";
      request.required_flags = flags;
      request.allowed_family_indices = {family};
      deviceDescription.queue_requests.push_back(std::move(request));
      const auto plan = resolveDevice(candidate, deviceDescription);
      if (!plan)
        continue;
      auto device = createDevice(*instance, *plan);
      if (!device)
        continue;
      const auto beforeWait = device_waits;
      const auto passed = testCommandLifecycle(*device, static_cast<std::uint32_t>(family));
      return check(device_waits == beforeWait, "command cleanup invoked vkDeviceWaitIdle") &&
             passed;
    }
  }
  return blocked("no Device with command-capable native Queue");
}
} // namespace

int main() {
  const bool passed = runHeadless();
  if (passed)
    std::puts("graphics.command: headless-command=PASS");
  else
    std::fputs(
        "graphics.command: headless-command=BLOCKED; assertions or native capability unavailable\n",
        stderr);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
