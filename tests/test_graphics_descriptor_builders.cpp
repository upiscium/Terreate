// Descriptor write builders and the pool description helper: each builder puts its arguments in the
// right members, a set written with builders passes updateDescriptorSet, a builder of the wrong
// type is refused against the layout, and a pool made from the layouts allocates exactly the sets
// it was asked for.
//
// GPU test on a headless Instance (no window system). The validation layer must be available:
// without it the test fails, never passes. No linker or platform mechanism is used.
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_set.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>
#include <terreate/graphics/descriptor_write.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/sampler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.descriptor_builders";
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

template <typename T>
bool hasError(const terreate::Result<T> &result, DescriptorUpdateError error) {
  return !result && result.error().code() == make_error_code(error);
}
template <typename T> bool hasError(const terreate::Result<T> &result, DescriptorPoolError error) {
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
        std::fprintf(stderr, "graphics.descriptor_builders: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

const DescriptorBufferWriteElement *bufferElement(const DescriptorWriteDescription &write) {
  const auto *elements = std::get_if<std::vector<DescriptorBufferWriteElement>>(&write.elements);
  return elements != nullptr && elements->size() == 1 ? &elements->front() : nullptr;
}
const DescriptorImageWriteElement *imageElement(const DescriptorWriteDescription &write) {
  const auto *elements = std::get_if<std::vector<DescriptorImageWriteElement>>(&write.elements);
  return elements != nullptr && elements->size() == 1 ? &elements->front() : nullptr;
}
const DescriptorSamplerWriteElement *samplerElement(const DescriptorWriteDescription &write) {
  const auto *elements = std::get_if<std::vector<DescriptorSamplerWriteElement>>(&write.elements);
  return elements != nullptr && elements->size() == 1 ? &elements->front() : nullptr;
}

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             ValidationRecorder &recorder) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("Allocator");

  BufferDescription buffer_description;
  buffer_description.size = 256;
  buffer_description.usage =
      vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer;
  auto buffer = createBuffer(*allocator, buffer_description);
  ImageDescription image_description;
  image_description.format = vk::Format::eR8G8B8A8Unorm;
  image_description.extent = vk::Extent3D{16, 16, 1};
  image_description.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
  auto image = createImage(*allocator, image_description);
  if (!buffer || !image)
    return blocked("test Buffer and Image");
  ImageViewDescription view_description;
  view_description.format = vk::Format::eR8G8B8A8Unorm;
  view_description.subresource_range =
      vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
  auto view = createImageView(device, *image, view_description);
  auto sampler = createSampler(device, {});
  if (!view || !sampler)
    return blocked("test ImageView and Sampler");

  // Each builder: the binding, the type, first_array_element 0, one element with the right members.
  const auto uniform = uniformBufferWrite(3, *buffer, 16, 64);
  const auto *uniform_element = bufferElement(uniform);
  check(uniform.binding == 3 && uniform.first_array_element == 0 &&
            uniform.descriptor_type == vk::DescriptorType::eUniformBuffer &&
            uniform_element != nullptr && uniform_element->buffer == &*buffer &&
            uniform_element->offset == 16 && uniform_element->range == 64,
        "uniformBufferWrite maps the binding, type, buffer, offset and range");
  const auto whole = uniformBufferWrite(0, *buffer);
  check(bufferElement(whole) != nullptr && bufferElement(whole)->offset == 0 &&
            bufferElement(whole)->range == VK_WHOLE_SIZE,
        "the range defaults to the whole buffer");
  const auto storage = storageBufferWrite(1, *buffer, 8);
  check(storage.descriptor_type == vk::DescriptorType::eStorageBuffer &&
            bufferElement(storage) != nullptr && bufferElement(storage)->offset == 8 &&
            bufferElement(storage)->range == VK_WHOLE_SIZE,
        "storageBufferWrite states the storage buffer type");
  const auto dynamic = bufferWrite(2, vk::DescriptorType::eUniformBufferDynamic, *buffer, 0, 32);
  check(dynamic.descriptor_type == vk::DescriptorType::eUniformBufferDynamic &&
            bufferElement(dynamic) != nullptr && bufferElement(dynamic)->range == 32,
        "bufferWrite takes any buffer descriptor type");
  const auto combined = combinedImageSamplerWrite(4, *image, *view, *sampler,
                                                  vk::ImageLayout::eShaderReadOnlyOptimal);
  const auto *combined_element = imageElement(combined);
  check(combined.binding == 4 && combined.first_array_element == 0 &&
            combined.descriptor_type == vk::DescriptorType::eCombinedImageSampler &&
            combined_element != nullptr && combined_element->image == &*image &&
            combined_element->view == &*view && combined_element->sampler == &*sampler &&
            combined_element->image_layout == vk::ImageLayout::eShaderReadOnlyOptimal,
        "combinedImageSamplerWrite maps the image, view, sampler and layout");
  const auto sampled = sampledImageWrite(5, *image, *view, vk::ImageLayout::eGeneral);
  check(sampled.descriptor_type == vk::DescriptorType::eSampledImage &&
            imageElement(sampled) != nullptr && imageElement(sampled)->sampler == nullptr &&
            imageElement(sampled)->image_layout == vk::ImageLayout::eGeneral,
        "sampledImageWrite has no sampler and the stated layout");
  const auto storage_image = storageImageWrite(6, *image, *view);
  check(storage_image.descriptor_type == vk::DescriptorType::eStorageImage &&
            imageElement(storage_image) != nullptr &&
            imageElement(storage_image)->image_layout == vk::ImageLayout::eGeneral &&
            imageElement(storage_image)->sampler == nullptr,
        "storageImageWrite is in the General layout");
  const auto plain_sampler = samplerWrite(7, *sampler);
  check(plain_sampler.descriptor_type == vk::DescriptorType::eSampler &&
            samplerElement(plain_sampler) != nullptr &&
            samplerElement(plain_sampler)->sampler == &*sampler,
        "samplerWrite maps the sampler");

  // Two layouts, written with designated initializers (no builder needed for those).
  DescriptorSetLayoutDescription first_description;
  first_description.bindings = {{.binding = 0,
                                 .descriptor_type = vk::DescriptorType::eUniformBuffer,
                                 .descriptor_count = 1,
                                 .stage_flags = vk::ShaderStageFlagBits::eFragment},
                                {.binding = 1,
                                 .descriptor_type = vk::DescriptorType::eCombinedImageSampler,
                                 .descriptor_count = 1,
                                 .stage_flags = vk::ShaderStageFlagBits::eFragment}};
  DescriptorSetLayoutDescription second_description;
  second_description.bindings = {{.binding = 0,
                                  .descriptor_type = vk::DescriptorType::eStorageBuffer,
                                  .descriptor_count = 2,
                                  .stage_flags = vk::ShaderStageFlagBits::eCompute}};
  auto first = createDescriptorSetLayout(device, first_description);
  auto second = createDescriptorSetLayout(device, second_description);
  if (!first || !second)
    return blocked("DescriptorSetLayouts");

  // The pool description sums what the layouts declare, per type, times the sets.
  const std::array<const DescriptorSetLayout *, 2> layouts{&*first, &*second};
  const auto pool_description = descriptorPoolDescriptionFor(layouts, 2);
  if (!check(bool(pool_description), "descriptorPoolDescriptionFor accepts live layouts"))
    return EXIT_FAILURE;
  const auto count_of = [&](vk::DescriptorType type) {
    for (const auto &size : pool_description->sizes)
      if (size.type == type)
        return size.descriptor_count;
    return std::uint32_t{0};
  };
  check(pool_description->max_sets == 4 && pool_description->sizes.size() == 3 &&
            count_of(vk::DescriptorType::eUniformBuffer) == 2 &&
            count_of(vk::DescriptorType::eCombinedImageSampler) == 2 &&
            count_of(vk::DescriptorType::eStorageBuffer) == 4,
        "the pool description sums the types and counts times the sets (2 sets per layout)");
  const auto with_flags = descriptorPoolDescriptionFor(
      layouts, 1, vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
  check(with_flags && with_flags->max_sets == 2 &&
            with_flags->flags == vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        "the flags are passed through and max_sets is layouts times sets");

  // A pool made from it allocates exactly the sets it was asked for.
  auto pool = createDescriptorPool(device, *pool_description);
  if (!pool)
    return blocked("DescriptorPool from the helper's description");
  auto set_a1 = pool->allocate(*first);
  auto set_a2 = pool->allocate(*first);
  auto set_b1 = pool->allocate(*second);
  auto set_b2 = pool->allocate(*second);
  check(set_a1 && set_a2 && set_b1 && set_b2, "the pool allocates the sets it was sized for");
  check(!pool->allocate(*first), "one set more than the pool was sized for is refused");

  // A set written with builders passes updateDescriptorSet; a wrong type is refused.
  if (set_a1) {
    const std::array<DescriptorWriteDescription, 2> writes{
        uniformBufferWrite(0, *buffer),
        combinedImageSamplerWrite(1, *image, *view, *sampler,
                                  vk::ImageLayout::eShaderReadOnlyOptimal)};
    check(bool(updateDescriptorSet(device, *first, *set_a1, writes)),
          "a set written with builders passes updateDescriptorSet");
    const std::array<DescriptorWriteDescription, 1> wrong{storageBufferWrite(0, *buffer)};
    check(hasError(updateDescriptorSet(device, *first, *set_a1, wrong),
                   DescriptorUpdateError::descriptor_type_mismatch),
          "a storage buffer write for a uniform buffer binding is a type mismatch");
  }

  // Named errors of the pool helper.
  DescriptorSetLayout taken = std::move(*second);
  const std::array<const DescriptorSetLayout *, 2> with_moved{&*first, &*second};
  check(hasError(descriptorPoolDescriptionFor(with_moved, 1), DescriptorPoolError::invalid_layout),
        "a moved-from layout is invalid_layout");
  check(taken.valid(), "the layout it was moved to is intact");
  check(hasError(descriptorPoolDescriptionFor(layouts, 0), DescriptorPoolError::invalid_count),
        "zero sets per layout is invalid_count");
  check(hasError(descriptorPoolDescriptionFor(layouts, std::numeric_limits<std::uint32_t>::max()),
                 DescriptorPoolError::invalid_count),
        "a set count that overflows 32 bits is invalid_count");
  check(hasError(descriptorPoolDescriptionFor({}, 1), DescriptorPoolError::invalid_count),
        "no layouts is invalid_count");

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
  description.application_name = "terreate-descriptor-builders";
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
    DeviceQueueRequest queue_request;
    queue_request.caller_id = "main";
    queue_request.required_flags = vk::QueueFlagBits::eGraphics;
    request.queue_requests = {queue_request};
    const auto device_plan = resolveDevice(candidate, request);
    if (!device_plan)
      continue;
    int status = EXIT_FAILURE;
    {
      auto device = createDevice(*instance, *device_plan);
      if (!device)
        return blocked("Device");
      status = exercise(*instance, candidate, *device, recorder);
    }
    if (status != EXIT_SUCCESS)
      return status;
    check(recorder.warnings_or_errors.load() == 0,
          "the validation layer reports nothing after destruction");
    if (failures == 0)
      std::puts("graphics.descriptor_builders: descriptor_builders=PASS validation-layer=PASS");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return blocked("no device with a graphics queue");
}

} // namespace

int main() { return gpuTest(); }
