#include <terreate/graphics/synchronization.hpp>

#include <new>
#include <optional>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class SynchronizationErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.synchronization"; }
  std::string message(int value) const override {
    switch (static_cast<SynchronizationError>(value)) {
    case SynchronizationError::invalid_device:
      return "invalid Device";
    case SynchronizationError::invalid_object:
      return "invalid or moved-from Fence or Semaphore";
    case SynchronizationError::timeline_not_enabled:
      return "the Device did not enable the timelineSemaphore feature";
    case SynchronizationError::invalid_description:
      return "invalid Fence or Semaphore description";
    case SynchronizationError::wrong_kind:
      return "a timeline operation on a binary Semaphore";
    case SynchronizationError::function_unavailable:
      return "a timeline semaphore function could not be loaded";
    case SynchronizationError::allocation_failed:
      return "allocation failed";
    }
    return "unknown synchronization error";
  }
};

auto fail(SynchronizationError error, const char *operation, const char *detail)
    -> terreate::Error {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}

auto nativeFailure(VkResult result, const char *operation, const char *detail) -> terreate::Error {
  const auto code = vk::make_error_code(static_cast<vk::Result>(result));
  try {
    return terreate::Error{code, operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{code};
  }
}

template <typename Function>
auto loadDevice(VkDevice device, const char *name) noexcept -> Function {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<Function>(vkGetDeviceProcAddr(device, name));
}
} // namespace

const std::error_category &synchronization_error_category() noexcept {
  static const SynchronizationErrorCategory category;
  return category;
}
std::error_code make_error_code(SynchronizationError error) noexcept {
  return {static_cast<int>(error), synchronization_error_category()};
}

// ---- Fence ----

struct Fence::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  FenceDescription description{};
  ~Impl() {
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device, fence, nullptr);
  }
};

Fence::Fence(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Fence::Fence(Fence &&other) noexcept = default;
Fence &Fence::operator=(Fence &&other) noexcept = default;
Fence::~Fence() = default;

vk::Fence Fence::nativeHandle() const noexcept {
  return vk::Fence{implementation_ ? implementation_->fence : VK_NULL_HANDLE};
}
bool Fence::correlatedWith(const Device &device) const noexcept {
  return implementation_ && device.valid() &&
         implementation_->device == static_cast<VkDevice>(device.nativeHandle());
}
const FenceDescription *Fence::configuration() const noexcept {
  return implementation_ ? &implementation_->description : nullptr;
}

auto Fence::status() const -> terreate::Result<FenceStatus> {
  constexpr auto operation = "Fence::status";
  if (!implementation_)
    return std::unexpected(
        fail(SynchronizationError::invalid_object, operation, "moved-from Fence"));
  const VkResult result = vkGetFenceStatus(implementation_->device, implementation_->fence);
  if (result == VK_SUCCESS)
    return FenceStatus::signaled;
  if (result == VK_NOT_READY)
    return FenceStatus::unsignaled;
  return std::unexpected(nativeFailure(result, "vkGetFenceStatus", "native fence status failed"));
}

auto Fence::wait(std::uint64_t timeout_ns) const -> terreate::Result<WaitStatus> {
  constexpr auto operation = "Fence::wait";
  if (!implementation_)
    return std::unexpected(
        fail(SynchronizationError::invalid_object, operation, "moved-from Fence"));
  const VkResult result =
      vkWaitForFences(implementation_->device, 1, &implementation_->fence, VK_TRUE, timeout_ns);
  if (result == VK_SUCCESS)
    return WaitStatus::satisfied;
  if (result == VK_TIMEOUT)
    return WaitStatus::timeout;
  return std::unexpected(nativeFailure(result, "vkWaitForFences", "native fence wait failed"));
}

auto Fence::reset() const -> terreate::Result<void> {
  constexpr auto operation = "Fence::reset";
  if (!implementation_)
    return std::unexpected(
        fail(SynchronizationError::invalid_object, operation, "moved-from Fence"));
  const VkResult result = vkResetFences(implementation_->device, 1, &implementation_->fence);
  if (result != VK_SUCCESS)
    return std::unexpected(nativeFailure(result, "vkResetFences", "native fence reset failed"));
  return {};
}

auto createFence(const Device &device, const FenceDescription &description)
    -> terreate::Result<Fence> {
  constexpr auto operation = "createFence";
  if (!device.valid())
    return std::unexpected(
        fail(SynchronizationError::invalid_device, operation, "moved-from Device"));
  try {
    auto impl = std::make_unique<Fence::Impl>();
    impl->device = static_cast<VkDevice>(device.nativeHandle());
    impl->description = description;
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    info.flags = description.signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    const VkResult result = vkCreateFence(impl->device, &info, nullptr, &impl->fence);
    if (result != VK_SUCCESS)
      return std::unexpected(
          nativeFailure(result, "vkCreateFence", "native fence creation failed"));
    return Fence{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(SynchronizationError::allocation_failed)});
  }
}

// ---- Semaphore ----

struct Semaphore::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkSemaphore semaphore = VK_NULL_HANDLE;
  SemaphoreDescription description{};
  ~Impl() {
    if (semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(device, semaphore, nullptr);
  }
};

Semaphore::Semaphore(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
Semaphore::Semaphore(Semaphore &&other) noexcept = default;
Semaphore &Semaphore::operator=(Semaphore &&other) noexcept = default;
Semaphore::~Semaphore() = default;

vk::Semaphore Semaphore::nativeHandle() const noexcept {
  return vk::Semaphore{implementation_ ? implementation_->semaphore : VK_NULL_HANDLE};
}
bool Semaphore::correlatedWith(const Device &device) const noexcept {
  return implementation_ && device.valid() &&
         implementation_->device == static_cast<VkDevice>(device.nativeHandle());
}
SubmissionSemaphoreKind Semaphore::kind() const noexcept {
  return implementation_ ? implementation_->description.kind : SubmissionSemaphoreKind::binary;
}
const SemaphoreDescription *Semaphore::configuration() const noexcept {
  return implementation_ ? &implementation_->description : nullptr;
}

namespace {
// Common precondition of the timeline operations.
auto timelineCheck(const Semaphore &semaphore, bool valid, const char *operation)
    -> std::optional<terreate::Error> {
  if (!valid)
    return fail(SynchronizationError::invalid_object, operation, "moved-from Semaphore");
  if (semaphore.kind() != SubmissionSemaphoreKind::timeline)
    return fail(SynchronizationError::wrong_kind, operation, "the Semaphore is binary");
  return std::nullopt;
}
} // namespace

auto Semaphore::counterValue() const -> terreate::Result<std::uint64_t> {
  constexpr auto operation = "Semaphore::counterValue";
  if (const auto error = timelineCheck(*this, valid(), operation))
    return std::unexpected(*error);
  const auto get = loadDevice<PFN_vkGetSemaphoreCounterValue>(implementation_->device,
                                                              "vkGetSemaphoreCounterValue");
  if (get == nullptr)
    return std::unexpected(fail(SynchronizationError::function_unavailable, operation,
                                "vkGetDeviceProcAddr returned null"));
  std::uint64_t value = 0;
  const VkResult result = get(implementation_->device, implementation_->semaphore, &value);
  if (result != VK_SUCCESS)
    return std::unexpected(
        nativeFailure(result, "vkGetSemaphoreCounterValue", "native counter query failed"));
  return value;
}

auto Semaphore::wait(const TimelineWait &request) const -> terreate::Result<WaitStatus> {
  constexpr auto operation = "Semaphore::wait";
  if (const auto error = timelineCheck(*this, valid(), operation))
    return std::unexpected(*error);
  const auto wait_semaphores =
      loadDevice<PFN_vkWaitSemaphores>(implementation_->device, "vkWaitSemaphores");
  if (wait_semaphores == nullptr)
    return std::unexpected(fail(SynchronizationError::function_unavailable, operation,
                                "vkGetDeviceProcAddr returned null"));
  VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  info.semaphoreCount = 1;
  info.pSemaphores = &implementation_->semaphore;
  info.pValues = &request.value;
  const VkResult result = wait_semaphores(implementation_->device, &info, request.timeout_ns);
  if (result == VK_SUCCESS)
    return WaitStatus::satisfied;
  if (result == VK_TIMEOUT)
    return WaitStatus::timeout;
  return std::unexpected(nativeFailure(result, "vkWaitSemaphores", "native semaphore wait failed"));
}

auto Semaphore::signal(std::uint64_t value) const -> terreate::Result<void> {
  constexpr auto operation = "Semaphore::signal";
  if (const auto error = timelineCheck(*this, valid(), operation))
    return std::unexpected(*error);
  const auto signal_semaphore =
      loadDevice<PFN_vkSignalSemaphore>(implementation_->device, "vkSignalSemaphore");
  if (signal_semaphore == nullptr)
    return std::unexpected(fail(SynchronizationError::function_unavailable, operation,
                                "vkGetDeviceProcAddr returned null"));
  VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
  info.semaphore = implementation_->semaphore;
  info.value = value;
  const VkResult result = signal_semaphore(implementation_->device, &info);
  if (result != VK_SUCCESS)
    return std::unexpected(
        nativeFailure(result, "vkSignalSemaphore", "native semaphore signal failed"));
  return {};
}

auto createSemaphore(const Device &device, const SemaphoreDescription &description)
    -> terreate::Result<Semaphore> {
  constexpr auto operation = "createSemaphore";
  if (!device.valid() || device.plan() == nullptr)
    return std::unexpected(
        fail(SynchronizationError::invalid_device, operation, "moved-from Device"));
  const bool timeline = description.kind == SubmissionSemaphoreKind::timeline;
  if (!timeline && description.initial_value != 0)
    return std::unexpected(fail(SynchronizationError::invalid_description, operation,
                                "a binary Semaphore has no initial value"));
  if (timeline && !device.plan()->enabledFeatures12().timelineSemaphore)
    return std::unexpected(fail(SynchronizationError::timeline_not_enabled, operation,
                                "enable the timelineSemaphore feature for the Device"));
  try {
    auto impl = std::make_unique<Semaphore::Impl>();
    impl->device = static_cast<VkDevice>(device.nativeHandle());
    impl->description = description;
    VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = description.initial_value;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = timeline ? &type_info : nullptr;
    const VkResult result = vkCreateSemaphore(impl->device, &info, nullptr, &impl->semaphore);
    if (result != VK_SUCCESS)
      return std::unexpected(
          nativeFailure(result, "vkCreateSemaphore", "native semaphore creation failed"));
    return Semaphore{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(SynchronizationError::allocation_failed)});
  }
}

auto submissionWait(const Semaphore &semaphore, vk::PipelineStageFlags2 stage_mask,
                    std::uint64_t value) noexcept -> SubmissionSemaphore {
  SubmissionSemaphore out;
  out.native.semaphore = semaphore.nativeHandle();
  out.native.value = value;
  out.native.stageMask = stage_mask;
  out.kind = semaphore.kind();
  return out;
}

auto submissionSignal(const Semaphore &semaphore, vk::PipelineStageFlags2 stage_mask,
                      std::uint64_t value) noexcept -> SubmissionSemaphore {
  return submissionWait(semaphore, stage_mask, value);
}

} // namespace terreate::graphics
