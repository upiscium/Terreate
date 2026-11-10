#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_write.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Only the calling test thread is armed, only around the helper/count seam.
// Fixtures, input vectors, validation callbacks and native forwarding are never
// deliberately subjected to allocation failure. No production failpoint API.
namespace allocation_test {
thread_local bool armed = false;
thread_local std::size_t attempts = 0;
thread_local std::size_t successful_before_failure = 0;
thread_local std::array<void *, 8> live{};
thread_local bool tracker_overflow = false;
void record(void *memory) noexcept {
  if (!armed)
    return;
  for (auto &entry : live)
    if (!entry) {
      entry = memory;
      return;
    }
  tracker_overflow = true;
}
void release(void *memory) noexcept {
  for (auto &entry : live)
    if (entry == memory)
      entry = nullptr;
}
bool empty() noexcept {
  for (const auto *entry : live)
    if (entry)
      return false;
  return !tracker_overflow;
}
void attempt() {
  if (armed && attempts++ >= successful_before_failure)
    throw std::bad_alloc{};
}
struct Guard {
  explicit Guard(std::size_t successes) {
    attempts = 0;
    successful_before_failure = successes;
    armed = true;
  }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
  ~Guard() { armed = false; }
};
} // namespace allocation_test

// Replacement allocation functions are intentionally test-executable-local.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
void *operator new(std::size_t size) {
  allocation_test::attempt();
  if (void *memory = std::malloc(size ? size : 1)) {
    allocation_test::record(memory);
    return memory;
  }
  throw std::bad_alloc{};
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept {
  allocation_test::release(memory);
  std::free(memory);
}
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }
void *operator new(std::size_t size, std::align_val_t alignment) {
  allocation_test::attempt();
  const auto align = static_cast<std::size_t>(alignment);
  const auto requested = size ? size : 1;
  if (requested > std::numeric_limits<std::size_t>::max() - (align - 1))
    throw std::bad_alloc{};
  const auto rounded = ((requested + align - 1) / align) * align;
  if (void *memory = std::aligned_alloc(align, rounded)) {
    allocation_test::record(memory);
    return memory;
  }
  throw std::bad_alloc{};
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void *memory, std::align_val_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::align_val_t) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  ::operator delete(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  ::operator delete(memory);
}
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)

namespace {
using namespace terreate::graphics;
using BufferElements = std::vector<DescriptorBufferWriteElement>;
using ImageElements = std::vector<DescriptorImageWriteElement>;
using SamplerElements = std::vector<DescriptorSamplerWriteElement>;
using UpdateResult = terreate::Result<void>;
static_assert(std::is_aggregate_v<DescriptorBufferWriteElement>);
static_assert(std::is_aggregate_v<DescriptorImageWriteElement>);
static_assert(std::is_aggregate_v<DescriptorSamplerWriteElement>);
static_assert(std::is_aggregate_v<DescriptorWriteDescription>);
static_assert(std::is_trivially_copyable_v<DescriptorBufferWriteElement>);
static_assert(std::is_trivially_copyable_v<DescriptorImageWriteElement>);
static_assert(std::is_trivially_copyable_v<DescriptorSamplerWriteElement>);
static_assert(std::same_as<decltype(DescriptorBufferWriteElement::offset), vk::DeviceSize>);
static_assert(std::same_as<decltype(DescriptorBufferWriteElement::range), vk::DeviceSize>);
static_assert(std::same_as<decltype(DescriptorWriteDescription::binding), std::uint32_t>);
static_assert(
    std::same_as<decltype(DescriptorWriteDescription::first_array_element), std::uint32_t>);
static_assert(sizeof(vk::DeviceSize) == sizeof(std::uint64_t));
static_assert(sizeof(DescriptorUpdateError) == sizeof(std::uint8_t));
static_assert(std::is_error_code_enum_v<DescriptorUpdateError>);
static_assert(std::variant_size_v<DescriptorWritePayload> == 3);
static_assert(std::same_as<std::variant_alternative_t<0, DescriptorWritePayload>, BufferElements>);
static_assert(std::same_as<std::variant_alternative_t<1, DescriptorWritePayload>, ImageElements>);
static_assert(std::same_as<std::variant_alternative_t<2, DescriptorWritePayload>, SamplerElements>);
static_assert(noexcept(validateDescriptorWriteCount(std::size_t{})));
static_assert(std::same_as<decltype(validateDescriptorWriteCount(std::size_t{})), UpdateResult>);
static_assert(std::same_as<decltype(&updateDescriptorSet),
                           UpdateResult (*)(const Device &, const DescriptorSetLayout &,
                                            const DescriptorSet &,
                                            std::span<const DescriptorWriteDescription>)>);

const char *test_name = "graphics.descriptor_write.cpu";
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
bool hasError(const UpdateResult &result, DescriptorUpdateError code) {
  return !result && result.error().code() == make_error_code(code) &&
         result.error().code().category() == descriptor_update_error_category();
}
struct Tracker {
  std::size_t calls = 0;
  std::size_t forwarded = 0;
  bool allow = false;
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  std::span<const DescriptorWriteDescription> expected{};
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

// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" void __real_vkUpdateDescriptorSets(VkDevice, std::uint32_t, const VkWriteDescriptorSet *,
                                              std::uint32_t, const VkCopyDescriptorSet *);
extern "C" void __wrap_vkUpdateDescriptorSets(VkDevice device, std::uint32_t count,
                                              const VkWriteDescriptorSet *writes,
                                              std::uint32_t copy_count,
                                              const VkCopyDescriptorSet *copies) {
  ++native.calls;
  const bool was_armed = allocation_test::armed;
  allocation_test::armed = false; // Never inject into the driver, even on test failure.
  const auto before = failures;
  if (!check(native.allow && !was_armed && writes && count == native.expected.size() &&
                 !native.expected.empty(),
             "only expected, unarmed valid native update allowed"))
    return;
  check(device == native.device && copy_count == 0 && copies == nullptr,
        "exact native Device, zero copies and null copy pointer");
  const VkDescriptorBufferInfo *buffer_end = nullptr;
  const VkDescriptorImageInfo *image_end = nullptr;
  for (std::size_t index = 0; index < count; ++index) {
    const auto &actual = writes[index];
    const auto &expected = native.expected[index];
    check(actual.sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET && !actual.pNext &&
              actual.dstSet == native.set && actual.dstBinding == expected.binding &&
              actual.dstArrayElement == expected.first_array_element &&
              actual.descriptorType == static_cast<VkDescriptorType>(expected.descriptor_type) &&
              !actual.pTexelBufferView,
          "native header, destination and ordered write fields exact");
    if (const auto *elements = std::get_if<BufferElements>(&expected.elements)) {
      if (!check(actual.pBufferInfo && !actual.pImageInfo &&
                     actual.descriptorCount == elements->size(),
                 "buffer count and only active pointer exact"))
        continue;
      check(!buffer_end || buffer_end == actual.pBufferInfo, "stable contiguous buffer storage");
      check(static_cast<const void *>(actual.pBufferInfo) !=
                static_cast<const void *>(elements->data()),
            "native buffer infos do not alias caller elements");
      for (std::size_t item = 0; item < elements->size(); ++item) {
        const auto &element = (*elements)[item];
        const auto &info = actual.pBufferInfo[item];
        check(info.buffer == element.buffer->nativeBuffer() && info.offset == element.offset &&
                  info.range == element.range,
              "every buffer native handle/offset/range including WHOLE_SIZE preserved");
      }
      buffer_end = actual.pBufferInfo + elements->size();
    } else if (const auto *images = std::get_if<ImageElements>(&expected.elements)) {
      if (!check(actual.pImageInfo && !actual.pBufferInfo &&
                     actual.descriptorCount == images->size(),
                 "image count and only active pointer exact"))
        continue;
      check(!image_end || image_end == actual.pImageInfo, "stable contiguous image storage");
      check(static_cast<const void *>(actual.pImageInfo) !=
                static_cast<const void *>(images->data()),
            "native image infos do not alias caller elements");
      for (std::size_t item = 0; item < images->size(); ++item) {
        const auto &element = (*images)[item];
        const auto &info = actual.pImageInfo[item];
        check(info.imageView == element.view->nativeImageView() &&
                  info.sampler ==
                      (element.sampler ? element.sampler->nativeSampler() : VK_NULL_HANDLE) &&
                  info.imageLayout == static_cast<VkImageLayout>(element.image_layout),
              "every image native view/sampler/access-time layout exact");
      }
      image_end = actual.pImageInfo + images->size();
    } else {
      const auto &samplers = std::get<SamplerElements>(expected.elements);
      if (!check(actual.pImageInfo && !actual.pBufferInfo &&
                     actual.descriptorCount == samplers.size(),
                 "sampler uses only image-info pointer"))
        continue;
      check(!image_end || image_end == actual.pImageInfo,
            "samplers share stable image-info storage");
      for (std::size_t item = 0; item < samplers.size(); ++item) {
        const auto &info = actual.pImageInfo[item];
        check(info.sampler == samplers[item].sampler->nativeSampler() &&
                  info.imageView == VK_NULL_HANDLE && info.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED,
              "sampler-only inactive view and layout deterministic");
      }
      image_end = actual.pImageInfo + samplers.size();
    }
  }
  // Real success paths are always forwarded; malformed unexpected calls are not
  // sent to Vulkan and have already permanently failed this test.
  if (failures == before) {
    ++native.forwarded;
    __real_vkUpdateDescriptorSets(device, count, writes, copy_count, copies);
  }
}
// NOLINTEND(bugprone-reserved-identifier)

namespace {
bool cpuTest() {
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  {
    allocation_test::Guard forbid{0};
    const auto zero = validateDescriptorWriteCount(0);
    const auto boundary = validateDescriptorWriteCount(maximum);
    check(hasError(zero, DescriptorUpdateError::invalid_write_count) &&
              zero.error().context().empty() && zero.error().detail().empty(),
          "zero count returns exact code-only error");
    check(boundary.has_value(),
          "UINT32_MAX structurally representable without allocating payloads");
    if constexpr (std::numeric_limits<std::size_t>::max() > maximum) {
      const auto overflow = validateDescriptorWriteCount(static_cast<std::size_t>(maximum) + 1);
      check(hasError(overflow, DescriptorUpdateError::invalid_write_count) &&
                overflow.error().context().empty() && overflow.error().detail().empty(),
            "UINT32_MAX+1 returns code-only error");
    }
    check(allocation_test::attempts == 0,
          "count seam attempts no allocation, not even diagnostics");
  }
  const std::error_code code = DescriptorUpdateError::invalid_write_count;
  check(code.value() == static_cast<int>(DescriptorUpdateError::invalid_write_count) &&
            std::string_view{code.category().name()} == "terreate.graphics.descriptor_update",
        "public error category and implicit error-code conversion");
  const DescriptorBufferWriteElement buffer;
  const DescriptorImageWriteElement image;
  const DescriptorSamplerWriteElement sampler;
  const DescriptorWriteDescription write;
  check(!buffer.buffer && buffer.offset == 0 && buffer.range == 0 && !image.image && !image.view &&
            !image.sampler && image.image_layout == vk::ImageLayout::eUndefined &&
            !sampler.sampler && write.binding == 0 && write.first_array_element == 0 &&
            std::to_underlying(write.descriptor_type) == VK_DESCRIPTOR_TYPE_MAX_ENUM &&
            std::holds_alternative<BufferElements>(write.elements) &&
            std::get<BufferElements>(write.elements).empty(),
        "all public default initializers are deliberately invalid");
  check(native.calls == 0 && native.forwarded == 0 && allocation_test::empty(),
        "CPU mode performs zero native descriptor updates or allocations");
  if (!failures)
    std::puts("graphics.descriptor_write.cpu: cpu=PASS");
  return failures == 0;
}

bool accept(const Device &device, const DescriptorSetLayout &layout, const DescriptorSet &set,
            std::span<const DescriptorWriteDescription> writes) {
  native.expected = writes;
  native.device = static_cast<VkDevice>(device.nativeHandle());
  native.set = set.nativeDescriptorSet();
  native.allow = true;
  const auto before = native.calls;
  const auto forwarded_before = native.forwarded;
  const auto result = updateDescriptorSet(device, layout, set, writes);
  native.allow = false;
  native.expected = {};
  return check(result.has_value() && native.calls == before + 1 &&
                   native.forwarded == forwarded_before + 1,
               "valid batch succeeds with exactly one forwarded native call");
}
bool reject(const Device &device, const DescriptorSetLayout &layout, const DescriptorSet &set,
            std::span<const DescriptorWriteDescription> writes, DescriptorUpdateError error) {
  const auto before = native.calls;
  const auto result = updateDescriptorSet(device, layout, set, writes);
  return check(hasError(result, error) && result.error().context() == "updateDescriptorSet" &&
                   !result.error().detail().empty() && native.calls == before,
               "rejected batch retains typed diagnostics and makes zero native calls");
}

constexpr std::array types{vk::DescriptorType::eStorageImage,
                           vk::DescriptorType::eUniformBuffer,
                           vk::DescriptorType::eSampler,
                           vk::DescriptorType::eStorageBufferDynamic,
                           vk::DescriptorType::eCombinedImageSampler,
                           vk::DescriptorType::eUniformBufferDynamic,
                           vk::DescriptorType::eSampledImage,
                           vk::DescriptorType::eStorageBuffer};
constexpr std::array<std::uint32_t, 8> bindings{31, 3, 19, 7, 23, 11, 29, 17};

bool resourceTest(Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
                  Device &moved_device, Device &other) {
  auto allocator = createAllocator(instance, candidate, device);
  auto same_allocator = createAllocator(instance, candidate, device);
  auto foreign_allocator = createAllocator(instance, candidate, other);
  if (!check(allocator && same_allocator && foreign_allocator, "three genuine VMA Allocators"))
    return false;
  BufferDescription buffer_description;
  buffer_description.size = 256;
  buffer_description.usage =
      vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer;
  auto buffer_source = createBuffer(*allocator, buffer_description);
  auto same_buffer = createBuffer(*same_allocator, buffer_description);
  auto foreign_buffer = createBuffer(*foreign_allocator, buffer_description);
  auto transfer_description = buffer_description;
  transfer_description.usage = vk::BufferUsageFlagBits::eTransferSrc;
  auto transfer_buffer = createBuffer(*allocator, transfer_description);
  if (!check(buffer_source && same_buffer && foreign_buffer && transfer_buffer,
             "real Buffer fixtures"))
    return false;
  Buffer buffer = std::move(*buffer_source);
  check(buffer.nativeAllocator() != same_buffer->nativeAllocator() &&
            buffer.nativeDevice() == same_buffer->nativeDevice() &&
            buffer.nativeDevice() != foreign_buffer->nativeDevice(),
        "different same-Device Allocators distinguished from foreign native Device");

  ImageDescription image_description;
  image_description.format = vk::Format::eR8G8B8A8Unorm;
  image_description.extent = vk::Extent3D{4, 4, 1};
  image_description.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
  auto image_source = createImage(*allocator, image_description);
  auto second_image = createImage(*same_allocator, image_description);
  auto foreign_image = createImage(*foreign_allocator, image_description);
  auto sampled_description = image_description;
  sampled_description.usage = vk::ImageUsageFlagBits::eSampled;
  auto sampled_image = createImage(*allocator, sampled_description);
  auto storage_description = image_description;
  storage_description.usage = vk::ImageUsageFlagBits::eStorage;
  auto storage_image = createImage(*allocator, storage_description);
  if (!check(image_source && second_image && foreign_image && sampled_image && storage_image,
             "real color Image fixtures"))
    return false;
  Image image = std::move(*image_source);
  ImageViewDescription view_description;
  view_description.format = image_description.format;
  view_description.subresource_range = vk::ImageSubresourceRange{
      vk::ImageAspectFlagBits::eColor, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  auto view_source = createImageView(device, image, view_description);
  auto second_view = createImageView(device, *second_image, view_description);
  auto foreign_view = createImageView(other, *foreign_image, view_description);
  auto sampled_view = createImageView(device, *sampled_image, view_description);
  auto storage_view = createImageView(device, *storage_image, view_description);
  auto explicit_identity = view_description;
  explicit_identity.components =
      vk::ComponentMapping{vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG,
                           vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA};
  auto explicit_view = createImageView(device, image, explicit_identity);
  auto sampler_source = createSampler(device);
  auto foreign_sampler = createSampler(other);
  SamplerDescription unnormalized_description;
  unnormalized_description.unnormalized_coordinates = true;
  auto unnormalized = createSampler(device, unnormalized_description);
  if (!check(view_source && second_view && foreign_view && sampled_view && storage_view &&
                 explicit_view && sampler_source && foreign_sampler && unnormalized,
             "genuine views, legal explicit RGBA identity, normalized/unnormalized Samplers"))
    return false;
  ImageView view = std::move(*view_source);
  Sampler sampler = std::move(*sampler_source);
  const auto &components = view.configuration()->effective.components;
  const auto &explicit_components = explicit_view->configuration()->effective.components;
  check(components.r == vk::ComponentSwizzle::eIdentity &&
            components.g == vk::ComponentSwizzle::eIdentity &&
            components.b == vk::ComponentSwizzle::eIdentity &&
            components.a == vk::ComponentSwizzle::eIdentity &&
            explicit_components.r == vk::ComponentSwizzle::eR &&
            explicit_components.g == vk::ComponentSwizzle::eG &&
            explicit_components.b == vk::ComponentSwizzle::eB &&
            explicit_components.a == vk::ComponentSwizzle::eA,
        "factory preserves legal eIdentity and explicit RGBA identity policies");

  DescriptorSetLayoutDescription layout_description;
  DescriptorPoolDescription pool_description{
      1, vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, {}};
  for (std::size_t index = 0; index < types.size(); ++index) {
    layout_description.bindings.push_back(
        {bindings[index], types[index], 2, vk::ShaderStageFlagBits::eCompute});
    pool_description.sizes.push_back(
        {types[index], types[index] == vk::DescriptorType::eUniformBuffer ? 3U : 2U});
  }
  // Binding 4 deliberately makes spilling out of binding 3 Vulkan-compatible;
  // this helper must still reject it rather than relying on driver validation.
  layout_description.bindings.push_back(
      {4, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute});
  auto layout_source = createDescriptorSetLayout(device, layout_description);
  auto different_layout = createDescriptorSetLayout(device, layout_description);
  auto foreign_layout = createDescriptorSetLayout(other, layout_description);
  if (!check(layout_source && different_layout && foreign_layout, "sparse unsorted real layouts"))
    return false;
  DescriptorSetLayout layout = std::move(*layout_source);
  auto pool = createDescriptorPool(device, pool_description);
  auto foreign_pool = createDescriptorPool(other, pool_description);
  if (!check(pool && foreign_pool, "same/foreign Device descriptor pools"))
    return false;
  auto set_source = pool->allocate(layout);
  auto foreign_set = foreign_pool->allocate(*foreign_layout);
  if (!check(set_source && foreign_set, "real native descriptor sets"))
    return false;
  DescriptorSet set = std::move(*set_source);
  const DescriptorBufferWriteElement buffer_element{&buffer, 0, 63};
  const DescriptorImageWriteElement image_element{&image, &view, nullptr,
                                                  vk::ImageLayout::eGeneral};
  auto combined_element = image_element;
  combined_element.sampler = &sampler;
  std::vector<DescriptorWriteDescription> writes;
  for (std::size_t index = 0; index < types.size(); ++index) {
    DescriptorWriteDescription write{bindings[index], 0, types[index], {}};
    switch (types[index]) {
    case vk::DescriptorType::eSampler:
      write.elements = SamplerElements{{&sampler}, {&*unnormalized}};
      break;
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eSampledImage:
      write.elements = ImageElements{image_element, image_element};
      break;
    case vk::DescriptorType::eCombinedImageSampler:
      write.elements = ImageElements{combined_element, combined_element};
      break;
    default:
      write.elements = BufferElements{buffer_element, {&*same_buffer, 0, 64}};
      break;
    }
    writes.push_back(std::move(write));
  }
  // All eight types, count=2 arrays ending exactly at the binding boundary.
  accept(device, layout, set, writes);
  // Native declarations do not require transitioning an unaccessed image from
  // its creation-time UNDEFINED layout. Nothing is bound, submitted or waited.
  check(image.configuration()->initial_layout == vk::ImageLayout::eUndefined,
        "declared descriptor layout independent from creation-time layout");
  auto overlap = writes;
  overlap.push_back(
      {3, 1, vk::DescriptorType::eUniformBuffer, BufferElements{{&buffer, 0, VK_WHOLE_SIZE}}});
  overlap.push_back(
      {3, 1, vk::DescriptorType::eUniformBuffer, BufferElements{{&*same_buffer, 0, 17}}});
  accept(device, layout, set, overlap);
  auto read_only = writes;
  std::get<ImageElements>(read_only[4].elements)[1].image_layout =
      vk::ImageLayout::eShaderReadOnlyOptimal;
  std::get<ImageElements>(read_only[6].elements)[0].image_layout =
      vk::ImageLayout::eShaderReadOnlyOptimal;
  accept(device, layout, set, read_only);
  auto same_origin = writes;
  for (const auto index : {std::size_t{0}, std::size_t{4}, std::size_t{6}}) {
    auto &same_element = std::get<ImageElements>(same_origin[index].elements).back();
    same_element.image = &*second_image;
    same_element.view = &*second_view;
  }
  accept(device, layout, set, same_origin);
  const std::array<DescriptorWriteDescription, 2> whole_buffers{
      {{3, 0, vk::DescriptorType::eUniformBuffer, BufferElements{{&buffer, 0, VK_WHOLE_SIZE}}},
       {17, 1, vk::DescriptorType::eStorageBuffer,
        BufferElements{{&*same_buffer, 0, VK_WHOLE_SIZE}}}}};
  accept(device, layout, set, whole_buffers); // no image-info storage
  const std::array<DescriptorWriteDescription, 1> sampler_only{
      {{19, 1, vk::DescriptorType::eSampler, SamplerElements{{&*unnormalized}}}}};
  accept(device, layout, set, sampler_only); // no buffer-info storage

  reject(moved_device, layout, set, writes, DescriptorUpdateError::invalid_device);
  reject(device, *layout_source, set, writes, DescriptorUpdateError::invalid_layout);
  reject(device, layout, *set_source, writes, DescriptorUpdateError::invalid_set);
  const DescriptorSet empty_set;
  reject(device, layout, empty_set, writes, DescriptorUpdateError::invalid_set);
  reject(device, *different_layout, set, writes, DescriptorUpdateError::layout_mismatch);
  reject(device, *foreign_layout, set, writes, DescriptorUpdateError::device_mismatch);
  reject(device, layout, *foreign_set, writes, DescriptorUpdateError::device_mismatch);
  reject(other, layout, set, writes, DescriptorUpdateError::device_mismatch);
  reject(device, layout, set, {}, DescriptorUpdateError::invalid_write_count);

  // Every negative mutation includes earlier valid writes. Reject the last
  // element/write without allowing a partial native update of its prefix.
  const auto badWrite = [&](DescriptorWriteDescription bad, DescriptorUpdateError error) {
    auto batch = writes;
    batch.push_back(std::move(bad));
    reject(device, layout, set, batch, error);
  };
  auto bad = writes[1];
  bad.binding = 1;
  badWrite(bad, DescriptorUpdateError::invalid_binding);
  bad = writes[1];
  bad.descriptor_type = vk::DescriptorType::eStorageBuffer;
  badWrite(bad, DescriptorUpdateError::descriptor_type_mismatch);
  badWrite({}, DescriptorUpdateError::unsupported_configuration);
  for (const auto type :
       {vk::DescriptorType::eInputAttachment, vk::DescriptorType::eUniformTexelBuffer,
        vk::DescriptorType::eStorageTexelBuffer, static_cast<vk::DescriptorType>(-1),
        static_cast<vk::DescriptorType>(VK_DESCRIPTOR_TYPE_MAX_ENUM)}) {
    bad = writes[1];
    bad.descriptor_type = type;
    badWrite(bad, DescriptorUpdateError::unsupported_configuration);
  }
  for (std::size_t index = 0; index < writes.size(); ++index) {
    bad = writes[index];
    if (std::holds_alternative<BufferElements>(bad.elements))
      bad.elements = ImageElements{image_element};
    else
      bad.elements = BufferElements{buffer_element};
    badWrite(bad, DescriptorUpdateError::invalid_payload);
    bad = writes[index];
    if (std::holds_alternative<SamplerElements>(bad.elements))
      bad.elements = ImageElements{image_element};
    else
      bad.elements = SamplerElements{{&sampler}};
    badWrite(bad, DescriptorUpdateError::invalid_payload);
    bad = writes[index];
    std::visit([](auto &elements) { elements.clear(); }, bad.elements);
    badWrite(bad, DescriptorUpdateError::invalid_write_count);
    bad = writes[index];
    bad.first_array_element = 1; // two elements would spill into another binding
    badWrite(bad, DescriptorUpdateError::invalid_array_range);
    bad.first_array_element = 2;
    badWrite(bad, DescriptorUpdateError::invalid_array_range);
    bad.first_array_element = std::numeric_limits<std::uint32_t>::max();
    badWrite(bad, DescriptorUpdateError::invalid_array_range);
  }
  const auto badBuffer = [&](DescriptorBufferWriteElement element, DescriptorUpdateError error,
                             std::size_t index) {
    auto write = writes[index];
    std::get<BufferElements>(write.elements).back() = element;
    badWrite(std::move(write), error);
  };
  for (const auto index : {std::size_t{1}, std::size_t{3}, std::size_t{5}, std::size_t{7}}) {
    badBuffer({nullptr, 0, 64}, DescriptorUpdateError::invalid_resource, index);
    badBuffer({&*buffer_source, 0, 64}, DescriptorUpdateError::invalid_resource, index);
    badBuffer({&*foreign_buffer, 0, 64}, DescriptorUpdateError::device_mismatch, index);
    badBuffer({&*transfer_buffer, 0, 64}, DescriptorUpdateError::unsupported_configuration, index);
    badBuffer({&buffer, 0, 0}, DescriptorUpdateError::invalid_buffer_range, index);
    badBuffer({&buffer, 256, 1}, DescriptorUpdateError::invalid_buffer_range, index);
    badBuffer({&buffer, 0, 257}, DescriptorUpdateError::invalid_buffer_range, index);
    badBuffer({&buffer, std::numeric_limits<vk::DeviceSize>::max(), 1},
              DescriptorUpdateError::invalid_buffer_range, index);
    badBuffer({&buffer, 0, std::numeric_limits<vk::DeviceSize>::max() - 1},
              DescriptorUpdateError::invalid_buffer_range, index);
    const auto &limits = device.plan()->capabilities().properties.limits;
    const auto alignment = (index == 1 || index == 5) ? limits.minUniformBufferOffsetAlignment
                                                      : limits.minStorageBufferOffsetAlignment;
    if (alignment > 1)
      badBuffer({&buffer, 1, 1}, DescriptorUpdateError::invalid_buffer_range, index);
  }
  badBuffer({&buffer, 0, VK_WHOLE_SIZE}, DescriptorUpdateError::invalid_buffer_range, 3);
  badBuffer({&buffer, 0, VK_WHOLE_SIZE}, DescriptorUpdateError::invalid_buffer_range, 5);
  // Check a core limit independently of the logical-size bound, with genuine
  // storage, not mutated capabilities/configuration or a fabricated Buffer.
  auto large_description = buffer_description;
  large_description.size =
      static_cast<vk::DeviceSize>(
          device.plan()->capabilities().properties.limits.maxUniformBufferRange) +
      1;
  large_description.usage = vk::BufferUsageFlagBits::eUniformBuffer;
  auto large_buffer = createBuffer(*allocator, large_description);
  if (!large_buffer) {
    blocked("native Buffer larger than uniform descriptor limit required for range-limit coverage");
    return false;
  }
  badBuffer({&*large_buffer, 0, large_description.size},
            DescriptorUpdateError::invalid_buffer_range, 1);
  badBuffer({&*large_buffer, 0, VK_WHOLE_SIZE}, DescriptorUpdateError::invalid_buffer_range, 1);

  const auto badImage = [&](DescriptorImageWriteElement element, DescriptorUpdateError error,
                            std::size_t index) {
    auto write = writes[index];
    std::get<ImageElements>(write.elements).back() = element;
    badWrite(std::move(write), error);
  };
  for (const auto index : {std::size_t{0}, std::size_t{4}, std::size_t{6}}) {
    const auto base = index == 4 ? combined_element : image_element;
    auto element = base;
    element.image = nullptr;
    badImage(element, DescriptorUpdateError::invalid_resource, index);
    element = base;
    element.view = nullptr;
    badImage(element, DescriptorUpdateError::invalid_resource, index);
    element = base;
    element.image = &*image_source;
    badImage(element, DescriptorUpdateError::invalid_resource, index);
    element = base;
    element.view = &*view_source;
    badImage(element, DescriptorUpdateError::invalid_resource, index);
    element = base;
    element.view = &*second_view;
    badImage(element, DescriptorUpdateError::invalid_resource, index);
    element = base;
    element.image = &*foreign_image;
    element.view = &*foreign_view;
    badImage(element, DescriptorUpdateError::device_mismatch, index);
    element = base;
    element.view = &*foreign_view;
    badImage(element, DescriptorUpdateError::device_mismatch, index);
    for (const auto image_layout :
         {vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
          vk::ImageLayout::eColorAttachmentOptimal,
          static_cast<vk::ImageLayout>(VK_IMAGE_LAYOUT_MAX_ENUM)}) {
      element = base;
      element.image_layout = image_layout;
      badImage(element, DescriptorUpdateError::invalid_image_layout, index);
    }
  }
  auto element = image_element;
  element.image_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
  badImage(element, DescriptorUpdateError::invalid_image_layout, 0);
  element = image_element;
  element.view = &*explicit_view;
  badImage(element, DescriptorUpdateError::unsupported_configuration, 0);
  element = image_element;
  element.image = &*sampled_image;
  element.view = &*sampled_view;
  badImage(element, DescriptorUpdateError::unsupported_configuration, 0);
  element = image_element;
  element.image = &*storage_image;
  element.view = &*storage_view;
  badImage(element, DescriptorUpdateError::unsupported_configuration, 6);
  element.sampler = &sampler;
  badImage(element, DescriptorUpdateError::unsupported_configuration, 4);
  element = image_element;
  element.sampler = &sampler;
  badImage(element, DescriptorUpdateError::invalid_payload, 0);
  badImage(element, DescriptorUpdateError::invalid_payload, 6);
  const std::array<const Sampler *, 3> invalid_samplers{nullptr, &*sampler_source,
                                                        &*foreign_sampler};
  for (const Sampler *invalid : invalid_samplers) {
    const auto error = invalid == &*foreign_sampler ? DescriptorUpdateError::device_mismatch
                                                    : DescriptorUpdateError::invalid_resource;
    auto sampler_write = writes[2];
    std::get<SamplerElements>(sampler_write.elements).back().sampler = invalid;
    badWrite(std::move(sampler_write), error);
    element = combined_element;
    element.sampler = invalid;
    badImage(element, error, 4);
  }
  element = combined_element;
  element.sampler = &*unnormalized;
  badImage(element, DescriptorUpdateError::unsupported_configuration, 4);

  // A legal sampled depth view is outside B1-A's color-only admission. Select
  // from real format capabilities; never mutate a configuration or fake a view.
  bool depth_checked = false;
  VmaAllocatorInfo parent{};
  vmaGetAllocatorInfo(allocator->nativeHandle(), &parent);
  for (const auto format : {vk::Format::eD32Sfloat, vk::Format::eD16Unorm}) {
    VkImageFormatProperties properties{};
    if (vkGetPhysicalDeviceImageFormatProperties(
            parent.physicalDevice, static_cast<VkFormat>(format), VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &properties) != VK_SUCCESS)
      continue;
    auto depth_description = sampled_description;
    depth_description.format = format;
    auto depth = createImage(*allocator, depth_description);
    if (!check(depth.has_value(), "supported native sampled depth Image"))
      break;
    auto depth_view_description = view_description;
    depth_view_description.format = format;
    depth_view_description.subresource_range.aspectMask = vk::ImageAspectFlagBits::eDepth;
    auto depth_view = createImageView(device, *depth, depth_view_description);
    if (!check(depth_view.has_value(), "legal sampled depth ImageView"))
      break;
    badImage({&*depth, &*depth_view, nullptr, vk::ImageLayout::eGeneral},
             DescriptorUpdateError::unsupported_configuration, 6);
    depth_checked = true;
    break;
  }
  if (!depth_checked) {
    // A failed factory above is an assertion failure, not a missing prerequisite.
    if (!failures)
      blocked("sampled depth format required for color-only admission coverage");
    return false;
  }

  // Persistent failure after N successes also forces diagnostic fallback. Both
  // kinds of native payload storage are nonempty, so 0/1/2 target distinct first,
  // later image-info and final write-info allocations in the current contract.
  for (const auto successes : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
    const auto before = native.calls;
    UpdateResult result;
    std::size_t attempts = 0;
    try {
      allocation_test::Guard injection{successes};
      result = updateDescriptorSet(device, layout, set, writes);
      attempts = allocation_test::attempts;
    } catch (...) {
      check(false, "temporary allocation exception escaped Result boundary");
    }
    check(attempts > successes && hasError(result, DescriptorUpdateError::allocation_failed) &&
              result.error().context().empty() && result.error().detail().empty() &&
              native.calls == before && allocation_test::empty(),
          "first/later temporary failure cleans up, code-only fallback, no native update");
  }
  auto diagnostic_batch = writes;
  diagnostic_batch.back().binding = 1;
  for (const bool count_error : {false, true}) {
    const auto before = native.calls;
    UpdateResult result;
    std::size_t attempts = 0;
    try {
      allocation_test::Guard injection{0};
      result = updateDescriptorSet(
          device, layout, set,
          count_error ? std::span<const DescriptorWriteDescription>{}
                      : std::span<const DescriptorWriteDescription>{diagnostic_batch});
      attempts = allocation_test::attempts;
    } catch (...) {
      check(false, "diagnostic allocation exception escaped Result boundary");
    }
    const auto error = count_error ? DescriptorUpdateError::invalid_write_count
                                   : DescriptorUpdateError::invalid_binding;
    check(attempts > 0 && hasError(result, error) && result.error().context().empty() &&
              result.error().detail().empty() && native.calls == before && allocation_test::empty(),
          "diagnostic allocation failure preserves original validation category/code");
  }
  accept(device, layout, set, writes); // recovery after all narrowly armed failures
  check(pool->free(set).has_value() && !set.valid(), "explicit same-Device set free");
  check(foreign_pool->free(*foreign_set).has_value(), "explicit foreign set free via its owner");
  check(pool->reset().has_value() && foreign_pool->reset().has_value(),
        "explicit empty pool resets");
  // All sets/pools/layouts/views/samplers/Images/Buffers die before Allocators,
  // and all Allocators before their caller-owned Devices. No GPU work exists.
  return failures == 0;
}

int exerciseDevices(Instance &instance) {
  const auto devices = queryPhysicalDevices(instance);
  if (!devices || devices->empty())
    return blocked("no Vulkan PhysicalDevice");
  for (const auto &candidate : *devices) {
    const auto &families = candidate.capabilities().queue_families;
    for (std::size_t index = 0; index < families.size(); ++index) {
      const auto flags =
          families[index].queueFlags & (vk::QueueFlagBits::eTransfer | vk::QueueFlagBits::eCompute |
                                        vk::QueueFlagBits::eGraphics);
      if (!families[index].queueCount || !flags)
        continue;
      DeviceDescription description;
      DeviceQueueRequest queue;
      queue.caller_id = "descriptor-write-test";
      queue.required_flags = flags;
      queue.allowed_family_indices = {index};
      description.queue_requests.push_back(queue);
      const auto plan = resolveDevice(candidate, description);
      if (!plan)
        continue;
      auto device = createDevice(instance, *plan);
      auto other = createDevice(instance, *plan);
      if (!device || !other)
        return blocked("two real logical Devices required");
      Device moved = std::move(*device);
      // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
      return resourceTest(instance, candidate, moved, *device, *other) ? EXIT_SUCCESS
                                                                       : EXIT_FAILURE;
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
  data.pMessageIdName = "terreate-descriptor-write-validation-probe";
  data.messageIdNumber = 266;
  data.pMessage = "synthetic warning verifies native validation transport";
  const auto before = recorder.warnings_or_errors.load(std::memory_order_relaxed);
  send(raw, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
  if (!check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == before + 1,
             "actual validation sink receives native probe"))
    return false;
  recorder.warnings_or_errors.fetch_sub(1, std::memory_order_relaxed);
  return true;
}
int nativeTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capabilities unavailable");
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
    return blocked("validation layer absent from effective plan");
  ValidationRecorder recorder;
  int status = EXIT_FAILURE;
  {
    auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
    if (!instance)
      return blocked("headless Vulkan Instance creation");
    if (validationProbe(*instance, recorder))
      status = exerciseDevices(*instance);
  }
  check(recorder.warnings_or_errors.load(std::memory_order_relaxed) == 0,
        "zero validation warnings/errors including full native teardown");
  if (status != EXIT_SUCCESS || failures)
    return EXIT_FAILURE;
  std::puts("graphics.descriptor_write: headless-native=PASS validation-layer=PASS");
  return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      return blocked("specify cpu or gpu mode");
    const std::string_view mode{argv[1]};
    if (mode == "cpu")
      return cpuTest() ? EXIT_SUCCESS : EXIT_FAILURE;
    if (mode == "gpu") {
      test_name = "graphics.descriptor_write";
      return nativeTest();
    }
    return blocked("unknown test mode");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s: assertion=FAIL: unhandled exception: %s\n", test_name, error.what());
    return EXIT_FAILURE;
  } catch (...) {
    std::fprintf(stderr, "%s: assertion=FAIL: unhandled unknown exception\n", test_name);
    return EXIT_FAILURE;
  }
}
