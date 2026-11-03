#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/sampler.hpp>

#include <array>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
using namespace terreate::graphics;
static_assert(!std::is_copy_constructible_v<RetirementQueue>);
static_assert(!std::is_move_constructible_v<RetirementQueue>);
static_assert(!std::is_copy_constructible_v<Buffer>);
static_assert(!std::is_copy_constructible_v<Image>);
static_assert(!std::is_copy_constructible_v<ImageView>);
static_assert(!std::is_copy_constructible_v<Sampler>);

int failures = 0;
std::size_t native_queries = 0;
std::size_t idle_calls = 0;
std::size_t native_buffer_destroys = 0;
VkFence forced_fence = VK_NULL_HANDLE;
std::optional<VkResult> forced_result;
VkSemaphore forced_semaphore = VK_NULL_HANDLE;
std::optional<std::uint64_t> forced_counter;

bool check(bool good, const char *label) {
  if (!good) {
    ++failures;
    std::fprintf(stderr, "graphics.retirement: assertion=FAIL: %s\n", label);
  }
  return good;
}
int blocked(const char *reason) {
  std::fprintf(stderr, "graphics.retirement: validation=BLOCKED: %s\n", reason);
  return EXIT_FAILURE;
}
struct Events {
  std::array<int, 24> destroyed{};
  std::size_t count = 0;
  void record(int id) noexcept {
    if (count < destroyed.size())
      destroyed[count++] = id;
  }
};
struct Owner {
  Events *events = nullptr;
  int id = 0;
  bool live = false;
  Owner(Events &event_log, int number) noexcept : events(&event_log), id(number), live(true) {}
  Owner(const Owner &) = delete;
  Owner &operator=(const Owner &) = delete;
  Owner(Owner &&rhs) noexcept
      : events(rhs.events), id(rhs.id), live(std::exchange(rhs.live, false)) {}
  Owner &operator=(Owner &&) = delete;
  ~Owner() {
    if (live)
      events->record(id);
  }
  bool valid() const noexcept { return live; }
};
struct ImageAndViewGroup {
  Owner image;
  Owner view;
  ImageAndViewGroup(Events &events) noexcept : image(events, 11), view(events, 12) {}
  ImageAndViewGroup(const ImageAndViewGroup &) = delete;
  ImageAndViewGroup(ImageAndViewGroup &&) noexcept = default;
  ~ImageAndViewGroup() = default;
  bool valid() const noexcept { return image.valid() && view.valid(); }
};
static_assert(std::is_nothrow_move_constructible_v<Owner>);
static_assert(std::is_nothrow_move_constructible_v<ImageAndViewGroup>);

// std::move is only a cast: an rvalue may bind to a noexcept copy constructor.
struct CopyOnlyOwner {
  CopyOnlyOwner() = default;
  CopyOnlyOwner(const CopyOnlyOwner &) noexcept = default;
  CopyOnlyOwner &operator=(const CopyOnlyOwner &) noexcept = default;
  bool valid() const noexcept { return true; }
};
struct CopyAndMoveOwner {
  CopyAndMoveOwner() = default;
  CopyAndMoveOwner(const CopyAndMoveOwner &) noexcept = default;
  CopyAndMoveOwner &operator=(const CopyAndMoveOwner &) noexcept = default;
  CopyAndMoveOwner(CopyAndMoveOwner &&) noexcept = default;
  CopyAndMoveOwner &operator=(CopyAndMoveOwner &&) noexcept = default;
  bool valid() const noexcept { return true; }
};
struct CopyAssignableMoveOwner {
  CopyAssignableMoveOwner() = default;
  CopyAssignableMoveOwner(const CopyAssignableMoveOwner &) = delete;
  CopyAssignableMoveOwner &operator=(const CopyAssignableMoveOwner &) noexcept = default;
  CopyAssignableMoveOwner(CopyAssignableMoveOwner &&) noexcept = default;
  CopyAssignableMoveOwner &operator=(CopyAssignableMoveOwner &&) noexcept = default;
  bool valid() const noexcept { return true; }
};
template <class Resource>
concept CanRetireOwner =
    requires(RetirementQueue &queue, Resource &owner, std::span<const SubmissionReceipt> receipts) {
      { queue.retire(owner, receipts) } -> std::same_as<terreate::Result<void>>;
    };
// The old nothrow-move trait alone incorrectly accepts this copy-only type.
static_assert(std::is_copy_constructible_v<CopyOnlyOwner>);
static_assert(std::is_nothrow_move_constructible_v<CopyOnlyOwner>);
static_assert(!CanRetireOwner<CopyOnlyOwner>);
static_assert(!CanRetireOwner<CopyAndMoveOwner>);
static_assert(std::is_copy_assignable_v<CopyAssignableMoveOwner>);
static_assert(!std::is_copy_constructible_v<CopyAssignableMoveOwner>);
static_assert(!CanRetireOwner<CopyAssignableMoveOwner>);
// Existing move-only resources and caller-owned aggregates remain supported.
static_assert(CanRetireOwner<Owner>);
static_assert(CanRetireOwner<ImageAndViewGroup>);
static_assert(CanRetireOwner<Buffer>);
static_assert(CanRetireOwner<Image>);
static_assert(CanRetireOwner<ImageView>);
static_assert(CanRetireOwner<Sampler>);

struct Fences {
  VkDevice device = VK_NULL_HANDLE;
  VkFence first = VK_NULL_HANDLE;
  VkFence second = VK_NULL_HANDLE;
  ~Fences() {
    if (second)
      vkDestroyFence(device, second, nullptr);
    if (first)
      vkDestroyFence(device, first, nullptr);
  }
};
struct Timeline {
  VkDevice device = VK_NULL_HANDLE;
  VkSemaphore semaphore = VK_NULL_HANDLE;
  ~Timeline() {
    if (semaphore)
      vkDestroySemaphore(device, semaphore, nullptr);
  }
};
bool expectRetirementError(const terreate::Result<void> &result, RetirementError error) {
  return !result && result.error().code() == make_error_code(error);
}
} // namespace

// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);
extern "C" void __wrap_vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer,
                                        VmaAllocation allocation) {
  if (buffer)
    ++native_buffer_destroys;
  __real_vmaDestroyBuffer(allocator, buffer, allocation);
}
extern "C" VkResult __real_vkGetFenceStatus(VkDevice, VkFence);
extern "C" VkResult __wrap_vkGetFenceStatus(VkDevice device, VkFence fence) {
  ++native_queries;
  if (forced_result && forced_fence == fence)
    return *forced_result;
  return __real_vkGetFenceStatus(device, fence);
}
extern "C" VkResult __real_vkGetSemaphoreCounterValue(VkDevice, VkSemaphore, std::uint64_t *);
extern "C" VkResult __wrap_vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                                                      std::uint64_t *counter) {
  ++native_queries;
  if (forced_counter && forced_semaphore == semaphore) {
    *counter = *forced_counter;
    return VK_SUCCESS;
  }
  return __real_vkGetSemaphoreCounterValue(device, semaphore, counter);
}
extern "C" VkResult __real_vkDeviceWaitIdle(VkDevice);
extern "C" VkResult __wrap_vkDeviceWaitIdle(VkDevice device) {
  ++idle_calls;
  return __real_vkDeviceWaitIdle(device);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
int exercise(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             Queue first_queue, Queue second_queue, bool timeline_mode) {
  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  const auto initial_idle_calls = idle_calls;
  Fences fences{native_device};
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (vkCreateFence(native_device, &fence_info, nullptr, &fences.first) != VK_SUCCESS ||
      vkCreateFence(native_device, &fence_info, nullptr, &fences.second) != VK_SUCCESS)
    return blocked("create two native fences");

  CommandPoolDescription p1;
  p1.queue_family_index = first_queue.familyIndex();
  p1.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto pool_one = createCommandPool(device, p1);
  if (!pool_one)
    return blocked("command pool first");
  CommandPoolDescription p2 = p1;
  p2.queue_family_index = second_queue.familyIndex();
  auto pool_two = createCommandPool(device, p2);
  if (!pool_two)
    return blocked("command pool second");
  auto command_one = pool_one->allocatePrimary();
  auto command_two = pool_two->allocatePrimary();
  if (!command_one || !command_two || !command_one->begin() || !command_one->end() ||
      !command_two->begin() || !command_two->end())
    return blocked("record executable command buffers");

  int identity_domain = 0;
  auto submit_fence = [&](Queue queue, const CommandBuffer &buffer, VkFence fence,
                          std::uint64_t serial) -> terreate::Result<SubmissionReceipt> {
    const std::array<const CommandBuffer *, 1> commands{&buffer};
    CompletionDescription completion;
    completion.device = &device;
    completion.queue = queue;
    completion.requested = {&identity_domain, serial};
    completion.effective = {&identity_domain, serial + 100};
    completion.condition = FenceCompletionCondition{vk::Fence{fence}};
    SubmissionDescription description;
    description.command_buffers = commands;
    description.fence = vk::Fence{fence};
    description.completion = completion;
    return submit(device, queue, description);
  };
  auto first = submit_fence(first_queue, *command_one, fences.first, 1);
  auto second = submit_fence(second_queue, *command_two, fences.second, 2);
  if (!first || !second)
    return blocked("real successful queue submissions");
  const auto completed_first = first->wait(std::numeric_limits<std::uint64_t>::max());
  const auto completed_second = second->wait(std::numeric_limits<std::uint64_t>::max());
  if (!completed_first || !completed_second ||
      completed_first->state != SubmissionCompletionState::completed ||
      completed_second->state != SubmissionCompletionState::completed)
    return blocked("native fence GPU completion before any cleanup");

  // Two native logical Devices may be usable at once. A retirement queue
  // must never combine completion evidence across different Devices, because
  // a lost-device reclaim must not free uses pending on a healthy Device.
  if (!device.plan())
    return blocked("live Device plan");
  auto second_device = createDevice(instance, *device.plan());
  if (!second_device)
    return blocked("independent second logical Device");
  const auto other_queue = second_device->queue("retirement-main");
  if (!other_queue.valid())
    return blocked("second Device Queue");
  Fences other_fences{static_cast<VkDevice>(second_device->nativeHandle())};
  if (vkCreateFence(other_fences.device, &fence_info, nullptr, &other_fences.first) != VK_SUCCESS)
    return blocked("second Device fence");
  CommandPoolDescription other_description = p1;
  auto other_pool = createCommandPool(*second_device, other_description);
  if (!other_pool)
    return blocked("second Device CommandPool");
  auto other_command = other_pool->allocatePrimary();
  if (!other_command || !other_command->begin() || !other_command->end())
    return blocked("second Device recorded CommandBuffer");
  const std::array<const CommandBuffer *, 1> other_commands{&*other_command};
  CompletionDescription other_completion;
  other_completion.device = &*second_device;
  other_completion.queue = other_queue;
  other_completion.requested = {&identity_domain, 103};
  other_completion.effective = {&identity_domain, 104};
  other_completion.condition = FenceCompletionCondition{vk::Fence{other_fences.first}};
  SubmissionDescription other_submission;
  other_submission.command_buffers = other_commands;
  other_submission.fence = vk::Fence{other_fences.first};
  other_submission.completion = other_completion;
  auto other_receipt = submit(*second_device, other_queue, other_submission);
  if (!other_receipt)
    return blocked("real second Device submission");
  const auto other_completed = other_receipt->wait(std::numeric_limits<std::uint64_t>::max());
  if (!other_completed || other_completed->state != SubmissionCompletionState::completed)
    return blocked("real second Device completion");
  {
    RetirementQueue mixed;
    Events events;
    Owner owner{events, 101};
    const std::array<SubmissionReceipt, 2> incompatible{*first, *other_receipt};
    const auto rejected = mixed.retire(owner, incompatible);
    check(expectRetirementError(rejected, RetirementError::device_mismatch) && owner.valid() &&
              mixed.pendingCount() == 0,
          "mixed-Device receipts rejected before owner transfer");
  }

  {
    RetirementQueue empty;
    Events events;
    Owner value{events, 1};
    const auto missing = empty.retire(value, std::span<const SubmissionReceipt>{});
    check(expectRetirementError(missing, RetirementError::missing_completion) && value.valid(),
          "empty completion set preserves ownership");
    check(static_cast<bool>(empty.requireEmpty()) && empty.pendingCount() == 0,
          "empty queue requireEmpty");
    const auto report = empty.collect();
    check(report && report->collected == 0 && report->pending == 0,
          "empty collection should be no-op");
    check(!empty.deviceLossObserved(), "loss not inferred from empty queue");
  }
  {
    Events events;
    ImageAndViewGroup group{events};
    RetirementQueue retirement;
    const std::array<SubmissionReceipt, 2> uses{*first, *second};
    const auto retired = retirement.retire(group, uses);
    // Moved-from status is deliberately observable through valid().
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
    check(retired && !group.valid() && retirement.pendingCount() == 1,
          "move-only ImageView/Image aggregate transferred");
    check(!retirement.requireEmpty() && retirement.requireEmpty().error().code() ==
                                            make_error_code(RetirementError::pending_entries),
          "pending owners require explicit collection");
    // One already completed fence and one synthetic NOT_READY status.
    forced_fence = fences.second;
    forced_result = VK_NOT_READY;
    const auto before_poll = native_queries;
    const auto not_ready = retirement.collect();
    check(not_ready && not_ready->collected == 0 && not_ready->pending == 1 && events.count == 0 &&
              native_queries == before_poll + 2,
          "AND retains aggregate until every declared submission completes");
    forced_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto error = retirement.collect();
    check(
        !error && error.error().code() == vk::make_error_code(vk::Result::eErrorOutOfHostMemory) &&
            retirement.pendingCount() == 1 && events.count == 0 && !retirement.deviceLossObserved(),
        "native query failure is not success and retains the owner");
    forced_result.reset();
    forced_fence = VK_NULL_HANDLE;
    const auto collected = retirement.collect();
    check(collected && collected->collected == 1 && collected->pending == 0 && events.count == 2 &&
              events.destroyed[0] == 12 && events.destroyed[1] == 11,
          "native completion reclaims and destroys View before Image");
    check(static_cast<bool>(retirement.requireEmpty()), "queue empty after collection");
  }
  {
    // Two independent entries, but no partial destructive actions if a later
    // receipt returns an error: observation phase precedes all ownership release.
    Events events;
    Owner a{events, 21};
    Owner b{events, 22};
    RetirementQueue retirement;
    check(static_cast<bool>(retirement.retire(a, std::span{&*first, 1})), "retire owner A");
    check(static_cast<bool>(retirement.retire(b, std::span{&*second, 1})), "retire owner B");
    forced_fence = fences.second;
    forced_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    auto error = retirement.collect();
    check(!error && events.count == 0 && retirement.pendingCount() == 2,
          "error atomicity must preserve all entries");
    forced_result = VK_NOT_READY;
    const auto partial = retirement.collect();
    check(partial && partial->collected == 1 && partial->pending == 1 && events.count == 1 &&
              events.destroyed[0] == 21,
          "independent completed resource can retire while another is pending");
    forced_result.reset();
    forced_fence = VK_NULL_HANDLE;
    const auto final = retirement.collect();
    check(final && final->collected == 1 && final->pending == 0 && events.count == 2 &&
              events.destroyed[1] == 22,
          "remaining owner released only after completion");
  }

  {
    // A lost Device must be observed natively before destructive recovery.
    // This test injects that status AFTER real GPU fences completed, so it
    // tests our state machine without pretending the hardware was lost.
    Events events;
    Owner resource{events, 31};
    RetirementQueue lost;
    check(static_cast<bool>(lost.retire(resource, std::span{&*first, 1})), "retire before loss");
    const auto premature = lost.reclaimAfterDeviceLoss(device);
    check(!premature && premature.error().code() ==
                            make_error_code(RetirementError::device_loss_not_observed),
          "cannot reclaim without verified native loss");
    forced_fence = fences.first;
    forced_result = VK_ERROR_DEVICE_LOST;
    const auto error = lost.collect();
    check(!error && error.error().code() == vk::make_error_code(vk::Result::eErrorDeviceLost) &&
              lost.pendingCount() == 1 && events.count == 0 && lost.deviceLossObserved(),
          "native loss latched with no implicit release");
    const auto wrong = lost.reclaimAfterDeviceLoss(*second_device);
    check(!wrong && wrong.error().code() == make_error_code(RetirementError::device_mismatch) &&
              lost.pendingCount() == 1 && events.count == 0,
          "lost-device reclaim rejects a different still-healthy Device");
    Owner extra{events, 32};
    const auto reject = lost.retire(extra, std::span{&*second, 1});
    check(expectRetirementError(reject, RetirementError::device_lost) && extra.valid(),
          "lost-device queue refuses new ownership transfer");
    forced_result.reset();
    forced_fence = VK_NULL_HANDLE;
    const auto reclaimed = lost.reclaimAfterDeviceLoss(device);
    check(reclaimed && *reclaimed == 1 && events.count == 1 && events.destroyed[0] == 31 &&
              lost.requireEmpty(),
          "explicit device-lost reclaim destroys original owner only");
    const auto after = lost.collect();
    check(!after && after.error().code() == make_error_code(RetirementError::device_lost),
          "lost-device queue is not silently reused as normally completed");
  }

  {
    // Pending destruction must fail-fast even when source receipt was valid.
    const auto child = fork();
    if (child == -1)
      return blocked("fork for destructor fail-fast");
    if (child == 0) {
      std::set_terminate([]() noexcept { std::_Exit(91); });
      {
        Events events;
        Owner resource{events, 51};
        RetirementQueue pending;
        const auto enqueue = pending.retire(resource, std::span{&*first, 1});
        if (!enqueue)
          std::_Exit(92);
      }
      std::_Exit(93);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child)
      return blocked("wait for fail-fast child");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 91,
          "nonempty destructor must terminate, not free pending owner");
  }

  {
    // Genuine VMA-backed move-only Buffer: no VMA destroy until native receipt
    // completion. Parents stay alive until after collection and Buffer teardown.
    auto allocator = createAllocator(instance, candidate, device);
    if (!allocator)
      return blocked("VMA Allocator for real retirement");
    BufferDescription description;
    description.size = 64;
    description.usage = vk::BufferUsageFlagBits::eTransferSrc;
    description.allocation.usage = VMA_MEMORY_USAGE_AUTO;
    description.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    description.allocation.required_flags = vk::MemoryPropertyFlagBits::eHostVisible;
    auto buffer = createBuffer(*allocator, description);
    if (!buffer)
      return blocked("VMA Buffer for real retirement");
    RetirementQueue retirement;
    const auto before_destroy = native_buffer_destroys;
    const auto transferred = retirement.retire(*buffer, std::span{&*first, 1});
    // Buffer explicitly supports valid() on its moved-from representation.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
    check(transferred && !buffer->valid() && retirement.pendingCount() == 1,
          "real VMA Buffer moved to retirement queue");
    forced_fence = fences.first;
    forced_result = VK_NOT_READY;
    const auto pending = retirement.collect();
    check(pending && pending->pending == 1 && native_buffer_destroys == before_destroy,
          "no premature native VMA Buffer destroy while GPU pending");
    forced_result.reset();
    forced_fence = VK_NULL_HANDLE;
    const auto done = retirement.collect();
    check(done && done->pending == 0 && done->collected == 1 &&
              native_buffer_destroys == before_destroy + 1,
          "native VMA Buffer freed exactly once on completion");
  }

  if (timeline_mode) {
    Timeline timeline{native_device};
    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_info.pNext = &type_info;
    if (vkCreateSemaphore(native_device, &sem_info, nullptr, &timeline.semaphore) != VK_SUCCESS)
      return blocked("create native timeline semaphore");
    const std::array<const CommandBuffer *, 1> buffers{&*command_one};
    CompletionDescription completion;
    completion.device = &device;
    completion.queue = first_queue;
    completion.requested = {&identity_domain, 3};
    completion.effective = {&identity_domain, 4};
    completion.condition = TimelineCompletionCondition{vk::Semaphore{timeline.semaphore}, 5};
    SubmissionSemaphore signal;
    signal.kind = SubmissionSemaphoreKind::timeline;
    signal.native.semaphore = vk::Semaphore{timeline.semaphore};
    signal.native.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    signal.native.value = 5;
    SubmissionDescription descriptor;
    descriptor.command_buffers = buffers;
    descriptor.signals = std::span{&signal, 1};
    descriptor.completion = completion;
    auto receipt = submit(device, first_queue, descriptor);
    if (!receipt)
      return blocked("native timeline submission");
    const auto completed = receipt->wait(std::numeric_limits<std::uint64_t>::max());
    if (!completed || completed->state != SubmissionCompletionState::completed)
      return blocked("native timeline completion");
    Events events;
    Owner resource{events, 41};
    RetirementQueue pending;
    check(static_cast<bool>(pending.retire(resource, std::span{&*receipt, 1})),
          "timeline receipt retirement ownership transfer");
    forced_semaphore = timeline.semaphore;
    forced_counter = 4;
    const auto below_target = pending.collect();
    check(below_target && below_target->collected == 0 && below_target->pending == 1 &&
              events.count == 0,
          "successful below-target timeline counter remains pending");
    forced_counter.reset();
    forced_semaphore = VK_NULL_HANDLE;
    const auto released = pending.collect();
    check(released && released->collected == 1 && events.count == 1,
          "real timeline completion triggers collection");
  }
  check(idle_calls == initial_idle_calls, "retirement performed no vkDeviceWaitIdle");
  if (failures == 0)
    std::puts(timeline_mode ? "graphics.retirement: headless-timeline=PASS"
                            : "graphics.retirement: headless-fence=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int run(bool timeline_mode, bool require_distinct_queues) {
  const auto caps = queryInstanceCapabilities();
  if (!caps)
    return blocked("Vulkan loader capabilities");
  InstanceDescription instance_desc;
  instance_desc.api_version = VK_API_VERSION_1_3;
  const auto instance_plan = resolveInstance(instance_desc, *caps);
  if (!instance_plan)
    return blocked("Vulkan 1.3 Instance resolution");
  auto instance = createInstance(*instance_plan);
  if (!instance)
    return blocked("Vulkan Instance");
  const auto candidates = queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("PhysicalDevice inventory");
  for (const auto &candidate : *candidates) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      if (!families[index].queueCount ||
          !(families[index].queueFlags &
            (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)))
        continue;
      DeviceDescription desc;
      desc.required_features_13.synchronization2 = VK_TRUE;
      desc.required_features_12.timelineSemaphore = timeline_mode ? VK_TRUE : VK_FALSE;
      DeviceQueueRequest main;
      main.caller_id = "retirement-main";
      main.required_flags =
          families[index].queueFlags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                                        vk::QueueFlagBits::eTransfer);
      main.allowed_family_indices = {index};
      desc.queue_requests.push_back(main);
      // Request a distinct physical Queue explicitly when a second queue is
      // available. If not, still exercise the AND across two submissions.
      std::size_t alternate_index = families.size();
      if (families[index].queueCount >= 2) {
        alternate_index = index;
      } else {
        for (std::size_t other = 0; other < families.size(); ++other) {
          // Empty transfer-only command buffers are valid for this explicit
          // submission test: do not hide available distinct Queue families.
          if (other != index && families[other].queueCount &&
              (families[other].queueFlags &
               (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                vk::QueueFlagBits::eTransfer))) {
            alternate_index = other;
            break;
          }
        }
      }
      auto try_desc = desc;
      if (alternate_index != families.size()) {
        DeviceQueueRequest alternate = main;
        alternate.caller_id = "retirement-secondary";
        alternate.allowed_family_indices = {alternate_index};
        alternate.required_flags = families[alternate_index].queueFlags &
                                   (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                                    vk::QueueFlagBits::eTransfer);
        try_desc.queue_requests.push_back(alternate);
      }
      auto plan = resolveDevice(candidate, try_desc);
      if (!plan) {
        plan = resolveDevice(candidate, desc);
        if (!plan)
          continue;
      }
      auto device = createDevice(*instance, *plan);
      if (!device)
        return blocked("native Device");
      const auto queue_a = device->queue("retirement-main");
      auto queue_b = device->queue("retirement-secondary");
      if (!queue_b.valid()) {
        // A dedicated multiqueue CTest reports an actual SKIP rather than
        // silently treating a same-Queue AND test as distinct-Queue coverage.
        if (require_distinct_queues)
          continue;
        queue_b = queue_a;
        std::puts("graphics.retirement: true-multi-queue=SKIP (one native queue)");
      } else {
        if (!check(queue_b.nativeHandle() != queue_a.nativeHandle(),
                   "distinct request must produce distinct native queues"))
          return EXIT_FAILURE;
        std::puts("graphics.retirement: true-multi-queue=PASS");
      }
      return exercise(*instance, candidate, *device, queue_a, queue_b, timeline_mode);
    }
  }
  if (require_distinct_queues) {
    std::puts("graphics.retirement: true-multi-queue=SKIP (no valid distinct native queues)");
    return 77;
  }
  if (timeline_mode) {
    std::puts("graphics.retirement: timeline-capability=SKIP");
    return 77;
  }
  return blocked("no synchronization2-enabled Device available");
}
} // namespace
int main(int argc, char **argv) {
  const auto mode = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
  return run(mode == "timeline", mode == "multiqueue");
}
