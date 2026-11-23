#include <terreate/graphics/swapchain_frame.hpp>

#include <new>
#include <string>
#include <vector>

namespace terreate::graphics {
namespace {
class SwapchainFrameErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.swapchain_frame"; }
  std::string message(int value) const override {
    switch (static_cast<SwapchainFrameError>(value)) {
    case SwapchainFrameError::invalid_device:
      return "invalid Device";
    case SwapchainFrameError::invalid_queue:
      return "invalid Queue";
    case SwapchainFrameError::invalid_swapchain:
      return "invalid Swapchain";
    case SwapchainFrameError::parent_mismatch:
      return "the Device, Queue and Swapchain do not belong together";
    case SwapchainFrameError::swapchain_retired:
      return "the Swapchain was retired by a successful creation";
    case SwapchainFrameError::stale_image:
      return "the image does not belong to this Swapchain";
    case SwapchainFrameError::invalid_description:
      return "invalid acquire or present description";
    case SwapchainFrameError::function_unavailable:
      return "a swapchain function could not be loaded";
    case SwapchainFrameError::allocation_failed:
      return "allocation failed";
    }
    return "unknown swapchain frame error";
  }
};

terreate::Error fail(SwapchainFrameError error, const char *operation, const char *detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{make_error_code(error)};
  }
}

terreate::Error nativeFailure(VkResult result, const char *operation, const char *detail) {
  const auto code = vk::make_error_code(static_cast<vk::Result>(result));
  try {
    return terreate::Error{code, operation, detail};
  } catch (const std::bad_alloc &) {
    return terreate::Error{code};
  }
}

template <typename Function> Function loadDevice(VkDevice device, const char *name) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<Function>(vkGetDeviceProcAddr(device, name));
}
} // namespace

const std::error_category &swapchain_frame_error_category() noexcept {
  static const SwapchainFrameErrorCategory category;
  return category;
}
std::error_code make_error_code(SwapchainFrameError error) noexcept {
  return {static_cast<int>(error), swapchain_frame_error_category()};
}

auto acquireNextImage(const Device &device, const Swapchain &swapchain,
                      const AcquireDescription &description) -> terreate::Result<AcquireResult> {
  constexpr auto operation = "acquireNextImage";
  if (!device.valid())
    return std::unexpected(
        fail(SwapchainFrameError::invalid_device, operation, "moved-from Device"));
  if (!swapchain.valid())
    return std::unexpected(
        fail(SwapchainFrameError::invalid_swapchain, operation, "moved-from Swapchain"));
  if (!swapchain.correlatedWith(device))
    return std::unexpected(fail(SwapchainFrameError::parent_mismatch, operation,
                                "the Swapchain belongs to another Device"));
  if (swapchain.retired())
    return std::unexpected(fail(SwapchainFrameError::swapchain_retired, operation,
                                "no image can be acquired from a retired Swapchain"));
  if (!description.semaphore && !description.fence)
    return std::unexpected(fail(SwapchainFrameError::invalid_description, operation,
                                "a semaphore or a fence is required"));

  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  const auto acquire =
      loadDevice<PFN_vkAcquireNextImageKHR>(native_device, "vkAcquireNextImageKHR");
  if (acquire == nullptr)
    return std::unexpected(fail(SwapchainFrameError::function_unavailable, operation,
                                "vkGetDeviceProcAddr returned null"));

  std::uint32_t index = 0;
  const VkResult result =
      acquire(native_device, swapchain.nativeSwapchain(), description.timeout_ns,
              static_cast<VkSemaphore>(description.semaphore),
              static_cast<VkFence>(description.fence), &index);
  AcquireResult out;
  out.native_result = static_cast<vk::Result>(result);
  switch (result) {
  case VK_SUCCESS:
  case VK_SUBOPTIMAL_KHR: {
    const auto images = swapchain.images();
    if (index >= images.size())
      return std::unexpected(nativeFailure(VK_ERROR_UNKNOWN, "vkAcquireNextImageKHR",
                                           "the driver returned an out-of-range image index"));
    out.status =
        result == VK_SUCCESS ? AcquireStatus::acquired : AcquireStatus::acquired_suboptimal;
    out.image = images[index];
    return out;
  }
  case VK_ERROR_OUT_OF_DATE_KHR:
    out.status = AcquireStatus::out_of_date;
    return out;
  case VK_TIMEOUT:
    out.status = AcquireStatus::timeout;
    return out;
  case VK_NOT_READY:
    out.status = AcquireStatus::not_ready;
    return out;
  default:
    return std::unexpected(nativeFailure(result, "vkAcquireNextImageKHR", "native acquire failed"));
  }
}

auto presentImage(const Device &device, Queue queue, const Swapchain &swapchain,
                  const PresentDescription &description) -> terreate::Result<PresentResult> {
  constexpr auto operation = "presentImage";
  if (!device.valid())
    return std::unexpected(
        fail(SwapchainFrameError::invalid_device, operation, "moved-from Device"));
  if (!queue.valid())
    return std::unexpected(fail(SwapchainFrameError::invalid_queue, operation, "invalid Queue"));
  if (!swapchain.valid())
    return std::unexpected(
        fail(SwapchainFrameError::invalid_swapchain, operation, "moved-from Swapchain"));
  if (!swapchain.correlatedWith(device) || !queue.correlatedWith(device))
    return std::unexpected(fail(SwapchainFrameError::parent_mismatch, operation,
                                "the Queue and Swapchain must belong to the Device"));
  if (!swapchain.owns(description.image))
    return std::unexpected(fail(SwapchainFrameError::stale_image, operation,
                                "the image does not belong to this Swapchain generation"));
  for (const auto semaphore : description.wait_semaphores)
    if (!semaphore)
      return std::unexpected(
          fail(SwapchainFrameError::invalid_description, operation, "a wait semaphore is null"));

  const auto native_device = static_cast<VkDevice>(device.nativeHandle());
  const auto present = loadDevice<PFN_vkQueuePresentKHR>(native_device, "vkQueuePresentKHR");
  if (present == nullptr)
    return std::unexpected(fail(SwapchainFrameError::function_unavailable, operation,
                                "vkGetDeviceProcAddr returned null"));

  try {
    std::vector<VkSemaphore> waits;
    waits.reserve(description.wait_semaphores.size());
    for (const auto semaphore : description.wait_semaphores)
      waits.push_back(static_cast<VkSemaphore>(semaphore));
    const VkSwapchainKHR native_swapchain = swapchain.nativeSwapchain();
    const std::uint32_t index = description.image.index();
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
    info.pWaitSemaphores = waits.data();
    info.swapchainCount = 1;
    info.pSwapchains = &native_swapchain;
    info.pImageIndices = &index;
    const VkResult result = present(static_cast<VkQueue>(queue.nativeHandle()), &info);
    PresentResult out;
    out.native_result = static_cast<vk::Result>(result);
    switch (result) {
    case VK_SUCCESS:
      out.status = PresentStatus::presented;
      return out;
    case VK_SUBOPTIMAL_KHR:
      out.status = PresentStatus::presented_suboptimal;
      return out;
    case VK_ERROR_OUT_OF_DATE_KHR:
      out.status = PresentStatus::out_of_date;
      return out;
    default:
      return std::unexpected(nativeFailure(result, "vkQueuePresentKHR", "native present failed"));
    }
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(SwapchainFrameError::allocation_failed)});
  }
}

} // namespace terreate::graphics
