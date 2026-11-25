// Image barrier builders: imageBarrier maps every field to the native structure
// and nothing else, recordImageBarrier records one barrier through recordBarriers
// with the same refusals, and SwapchainImage::nativeHandle names the same image.
//
// `cpu` needs no device. `gpu` runs on a headless Instance with a software or real
// device and requires the validation layer: without it the test fails, never
// passes. No linker or platform mechanism is used.
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/barriers.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/completion.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <terreate/graphics/synchronization.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.barriers";
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

template <typename T> bool hasError(const terreate::Result<T> &result, TransferCommandError error) {
  return !result && result.error().code() == make_error_code(error);
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "graphics.barriers: validation message: %s\n", event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

ImageBarrierDescription fullDescription() {
  ImageBarrierDescription description;
  description.src_stage = vk::PipelineStageFlagBits2::eTransfer;
  description.src_access = vk::AccessFlagBits2::eTransferWrite;
  description.dst_stage = vk::PipelineStageFlagBits2::eFragmentShader;
  description.dst_access = vk::AccessFlagBits2::eShaderSampledRead;
  description.old_layout = vk::ImageLayout::eTransferDstOptimal;
  description.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
  description.subresource_range =
      vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 1, 2, 3, 4};
  description.src_queue_family = 5;
  description.dst_queue_family = 6;
  return description;
}

// Every field lands in the right native member; the image is the one passed in.
bool mapsEveryField(const VkImageMemoryBarrier2 &barrier, VkImage image) {
  return barrier.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 && barrier.pNext == nullptr &&
         barrier.srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
         barrier.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT &&
         barrier.dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT &&
         barrier.dstAccessMask == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT &&
         barrier.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
         barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
         barrier.srcQueueFamilyIndex == 5 && barrier.dstQueueFamilyIndex == 6 &&
         barrier.image == image &&
         barrier.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
         barrier.subresourceRange.baseMipLevel == 1 && barrier.subresourceRange.levelCount == 2 &&
         barrier.subresourceRange.baseArrayLayer == 3 && barrier.subresourceRange.layerCount == 4;
}

int cpuTest() {
  static_assert(noexcept(imageBarrier(std::declval<const Image &>(), ImageBarrierDescription{})));
  static_assert(
      noexcept(imageBarrier(std::declval<const SwapchainImage &>(), ImageBarrierDescription{})));
  static_assert(
      std::is_same_v<decltype(std::declval<const SwapchainImage &>().nativeHandle()), vk::Image>);

  const SwapchainImage empty;
  const VkImageMemoryBarrier2 mapped = imageBarrier(empty, fullDescription());
  check(mapsEveryField(mapped, VK_NULL_HANDLE), "every description field reaches the barrier");
  check(empty.nativeHandle() == empty.nativeImage(),
        "SwapchainImage::nativeHandle is the same image as nativeImage");

  // An unset description is Vulkan's neutral value, not a policy.
  const VkImageMemoryBarrier2 neutral = imageBarrier(empty, ImageBarrierDescription{});
  check(neutral.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 && neutral.pNext == nullptr &&
            neutral.srcStageMask == 0 && neutral.srcAccessMask == 0 && neutral.dstStageMask == 0 &&
            neutral.dstAccessMask == 0 && neutral.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            neutral.newLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            neutral.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
            neutral.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
            neutral.image == VK_NULL_HANDLE && neutral.subresourceRange.aspectMask == 0,
        "an unset description gives Vulkan's neutral values and no aspect");
  if (failures == 0)
    std::puts("graphics.barriers.cpu: cpu=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

DeviceQueueRequest graphicsQueue() {
  DeviceQueueRequest request;
  request.caller_id = "main";
  request.strength = RequirementStrength::required;
  request.required_flags = vk::QueueFlagBits::eGraphics;
  return request;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             ValidationRecorder &recorder) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("Allocator");
  const Queue queue = device.queue("main");
  if (!queue.valid())
    return blocked("graphics Queue");

  ImageDescription image_description;
  image_description.format = vk::Format::eR8G8B8A8Unorm;
  image_description.extent = vk::Extent3D{16, 16, 1};
  image_description.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  auto first = createImage(*allocator, image_description);
  auto second = createImage(*allocator, image_description);
  if (!first || !second)
    return blocked("test Images");

  ImageBarrierDescription to_transfer;
  to_transfer.dst_stage = vk::PipelineStageFlagBits2::eTransfer;
  to_transfer.dst_access = vk::AccessFlagBits2::eTransferWrite;
  to_transfer.old_layout = vk::ImageLayout::eUndefined;
  to_transfer.new_layout = vk::ImageLayout::eTransferDstOptimal;
  to_transfer.subresource_range =
      vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

  // The Image overload names the Image's own native handle.
  const VkImageMemoryBarrier2 built = imageBarrier(*first, fullDescription());
  check(mapsEveryField(built, first->nativeImage()),
        "the Image overload maps every field and the Image's native handle");

  CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto pool = createCommandPool(device, pool_description);
  if (!pool)
    return blocked("CommandPool");
  auto command = pool->allocatePrimary();
  if (!command)
    return blocked("CommandBuffer");

  // Not recording: the same refusal as recordBarriers.
  check(hasError(recordImageBarrier(device, *command, *first, to_transfer),
                 TransferCommandError::not_recording),
        "a CommandBuffer that is not recording is refused");

  if (!command->begin())
    return blocked("begin the CommandBuffer");

  // A moved-from Image gives a null image and is refused by recordBarriers' own rule.
  Image taken = std::move(*second);
  check(imageBarrier(*second, to_transfer).image == VK_NULL_HANDLE,
        "a moved-from Image gives a barrier with a null image");
  check(hasError(recordImageBarrier(device, *command, *second, to_transfer),
                 TransferCommandError::invalid_native_structure),
        "a moved-from Image is refused");
  // An empty SwapchainImage likewise; a range without an aspect likewise.
  check(hasError(recordImageBarrier(device, *command, SwapchainImage{}, to_transfer),
                 TransferCommandError::invalid_native_structure),
        "an empty SwapchainImage is refused");
  ImageBarrierDescription no_aspect = to_transfer;
  no_aspect.subresource_range.aspectMask = {};
  check(hasError(recordImageBarrier(device, *command, *first, no_aspect),
                 TransferCommandError::invalid_native_structure),
        "a subresource range without an aspect is refused");

  // Two transitions in one recordBarriers call, and one through recordImageBarrier.
  const std::array<VkImageMemoryBarrier2, 2> both{imageBarrier(*first, to_transfer),
                                                  imageBarrier(taken, to_transfer)};
  check(bool(recordBarriers(device, *command, {.images = both})),
        "two builder barriers go into one recordBarriers call");
  ImageBarrierDescription back = to_transfer;
  back.src_stage = to_transfer.dst_stage;
  back.src_access = to_transfer.dst_access;
  back.dst_stage = vk::PipelineStageFlagBits2::eFragmentShader;
  back.dst_access = vk::AccessFlagBits2::eShaderSampledRead;
  back.old_layout = vk::ImageLayout::eTransferDstOptimal;
  back.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
  check(bool(recordImageBarrier(device, *command, *first, back)),
        "recordImageBarrier records one transition");
  if (!command->end())
    return blocked("end the CommandBuffer");

  // The driver and the validation layer accept what was recorded.
  auto fence = createFence(device, {});
  if (!fence)
    return blocked("Fence");
  int identity_domain = 0;
  const std::array<const CommandBuffer *, 1> buffers{&*command};
  SubmissionDescription submission;
  submission.command_buffers = buffers;
  submission.fence = fence->nativeHandle();
  submission.completion.device = &device;
  submission.completion.queue = queue;
  submission.completion.requested = {&identity_domain, 1};
  submission.completion.effective = {&identity_domain, 1};
  submission.completion.condition = FenceCompletionCondition{fence->nativeHandle()};
  const auto receipt = submit(device, queue, submission);
  if (!check(bool(receipt), "the recorded barriers are submitted"))
    return EXIT_FAILURE;
  const auto waited = fence->wait(UINT64_MAX);
  check(waited && *waited == WaitStatus::satisfied, "the submission completes");
  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-barriers";
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance");
  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");

  for (const auto &candidate : *inventory) {
    DeviceDescription request;
    request.queue_requests = {graphicsQueue()};
    request.required_features_13.synchronization2 = VK_TRUE;
    const auto device_plan = resolveDevice(candidate, request);
    if (!device_plan)
      continue;
    int status = EXIT_FAILURE;
    {
      auto device = createDevice(*instance, *device_plan);
      if (!device)
        return blocked("Device with synchronization2");
      status = exercise(*instance, candidate, *device, recorder);
    }
    if (status != EXIT_SUCCESS)
      return status;
    check(recorder.warnings_or_errors.load() == 0,
          "the validation layer reports nothing after destruction");
    if (failures == 0)
      std::puts("graphics.barriers: barriers=PASS validation-layer=PASS");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return blocked("no device with synchronization2 and a graphics queue");
}

} // namespace

int main(int argc, char **argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "gpu";
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu")
    return gpuTest();
  std::fprintf(stderr, "usage: terreate_graphics_barriers_tests [cpu|gpu]\n");
  return EXIT_FAILURE;
}
