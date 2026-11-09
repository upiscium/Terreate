#include <terreate/graphics/descriptor_pool.hpp>

#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace terreate::graphics {
namespace {
class DescriptorPoolErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.descriptor_pool"; }
  std::string message(int value) const override {
    switch (static_cast<DescriptorPoolError>(value)) {
    case DescriptorPoolError::invalid_device:
      return "invalid DescriptorPool Device";
    case DescriptorPoolError::invalid_description:
      return "invalid DescriptorPool description";
    case DescriptorPoolError::unsupported_configuration:
      return "unsupported descriptor type or flags";
    case DescriptorPoolError::invalid_pool:
      return "invalid moved-from DescriptorPool";
    case DescriptorPoolError::degraded_pool:
      return "DescriptorPool requires explicit reset";
    case DescriptorPoolError::invalid_layout:
      return "invalid or foreign DescriptorSetLayout";
    case DescriptorPoolError::invalid_set:
      return "invalid, foreign or duplicate DescriptorSet";
    case DescriptorPoolError::invalid_count:
      return "empty or unrepresentable descriptor batch";
    case DescriptorPoolError::free_not_supported:
      return "DescriptorPool lacks FREE_DESCRIPTOR_SET";
    case DescriptorPoolError::native_null_handle:
      return "Vulkan returned null descriptor handle on success";
    case DescriptorPoolError::allocation_failed:
      return "DescriptorPool CPU allocation failed";
    }
    return "unknown DescriptorPool error";
  }
};
auto failure(std::error_code code, const char *context, const char *detail) {
  try {
    return std::unexpected(terreate::Error{code, context, detail});
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{code});
  }
}
auto fail(DescriptorPoolError error) {
  return std::unexpected(terreate::Error{make_error_code(error)});
}
bool supportedType(vk::DescriptorType type) noexcept {
  switch (type) {
  case vk::DescriptorType::eUniformBuffer:
  case vk::DescriptorType::eStorageBuffer:
  case vk::DescriptorType::eSampler:
  case vk::DescriptorType::eSampledImage:
  case vk::DescriptorType::eStorageImage:
  case vk::DescriptorType::eCombinedImageSampler:
  case vk::DescriptorType::eUniformBufferDynamic:
  case vk::DescriptorType::eStorageBufferDynamic:
    return true;
  default:
    return false;
  }
}
bool validCount(std::size_t count) noexcept {
  return count != 0 && count <= std::numeric_limits<std::uint32_t>::max();
}
} // namespace

const std::error_category &descriptor_pool_error_category() noexcept {
  static const DescriptorPoolErrorCategory category;
  return category;
}
std::error_code make_error_code(DescriptorPoolError error) noexcept {
  return {static_cast<int>(error), descriptor_pool_error_category()};
}
auto validateDescriptorPoolDescription(const DescriptorPoolDescription &description)
    -> terreate::Result<void> {
  if (description.max_sets == 0 ||
      description.sizes.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(DescriptorPoolError::invalid_description);
  if ((static_cast<VkDescriptorPoolCreateFlags>(description.flags) &
       ~VkDescriptorPoolCreateFlags{VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT}) != 0)
    return fail(DescriptorPoolError::unsupported_configuration);
  for (const auto &size : description.sizes) {
    if (size.descriptor_count == 0)
      return fail(DescriptorPoolError::invalid_description);
    if (!supportedType(size.type))
      return fail(DescriptorPoolError::unsupported_configuration);
  }
  // No aggregate arithmetic: duplicate entries are legal native capacities,
  // not a layout budget. Preserve even totals beyond UINT32_MAX without wrapping.
  return {};
}

struct DescriptorPool::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorPool handle = VK_NULL_HANDLE;
  DescriptorPoolConfiguration config{};
  DescriptorPoolState state = DescriptorPoolState::ready;
  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(device, handle, nullptr);
  }
};

DescriptorPool::DescriptorPool(std::unique_ptr<Impl> impl) noexcept
    : implementation_(std::move(impl)) {}
DescriptorPool::DescriptorPool(DescriptorPool &&) noexcept = default;
DescriptorPool &DescriptorPool::operator=(DescriptorPool &&) noexcept = default;
DescriptorPool::~DescriptorPool() = default;
bool DescriptorPool::valid() const noexcept { return nativeDescriptorPool() != VK_NULL_HANDLE; }
DescriptorPoolState DescriptorPool::state() const noexcept {
  return implementation_ ? implementation_->state : DescriptorPoolState::invalid_moved_from;
}
vk::DescriptorPool DescriptorPool::nativeHandle() const noexcept {
  return vk::DescriptorPool{nativeDescriptorPool()};
}
VkDescriptorPool DescriptorPool::nativeDescriptorPool() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
vk::Device DescriptorPool::nativeDevice() const noexcept { return vk::Device{nativeVkDevice()}; }
VkDevice DescriptorPool::nativeVkDevice() const noexcept {
  return implementation_ ? implementation_->device : VK_NULL_HANDLE;
}
bool DescriptorPool::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
const DescriptorPoolConfiguration *DescriptorPool::configuration() const noexcept {
  return implementation_ ? &implementation_->config : nullptr;
}

auto createDescriptorPool(const Device &device, const DescriptorPoolDescription &description)
    -> terreate::Result<DescriptorPool> {
  if (!device.valid())
    return fail(DescriptorPoolError::invalid_device);
  if (auto validation = validateDescriptorPoolDescription(description); !validation)
    return std::unexpected(std::move(validation.error()));
  try {
    auto impl = std::make_unique<DescriptorPool::Impl>();
    impl->device = static_cast<VkDevice>(device.nativeHandle());
    impl->config = {description, description};
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(description.sizes.size());
    for (const auto &size : description.sizes)
      sizes.push_back({static_cast<VkDescriptorType>(size.type), size.descriptor_count});
    const VkDescriptorPoolCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = static_cast<VkDescriptorPoolCreateFlags>(description.flags),
        .maxSets = description.max_sets,
        .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
        .pPoolSizes = sizes.empty() ? nullptr : sizes.data(),
    };
    VkDescriptorPool handle = VK_NULL_HANDLE;
    const auto result = vkCreateDescriptorPool(impl->device, &info, nullptr, &handle);
    if (result != VK_SUCCESS)
      return failure(vk::make_error_code(static_cast<vk::Result>(result)), "vkCreateDescriptorPool",
                     "native descriptor pool creation failed");
    impl->handle = handle;
    if (handle == VK_NULL_HANDLE)
      return fail(DescriptorPoolError::native_null_handle);
    return DescriptorPool{std::move(impl)};
  } catch (const std::bad_alloc &) {
    return fail(DescriptorPoolError::allocation_failed);
  } catch (const std::length_error &) {
    return fail(DescriptorPoolError::allocation_failed);
  }
}

auto DescriptorPool::allocate(std::span<const DescriptorSetLayout *const> layouts)
    -> terreate::Result<std::vector<DescriptorSet>> {
  if (!valid())
    return fail(DescriptorPoolError::invalid_pool);
  if (state() != DescriptorPoolState::ready)
    return fail(DescriptorPoolError::degraded_pool);
  if (!validCount(layouts.size()))
    return fail(DescriptorPoolError::invalid_count);
  for (const auto *layout : layouts)
    if (!layout || !layout->valid() || layout->nativeVkDevice() != nativeVkDevice())
      return fail(DescriptorPoolError::invalid_layout);
  try {
    std::vector<VkDescriptorSetLayout> native_layouts;
    native_layouts.reserve(layouts.size());
    for (const auto *layout : layouts)
      native_layouts.push_back(layout->nativeDescriptorSetLayout());
    std::vector<VkDescriptorSet> handles(layouts.size(), VK_NULL_HANDLE);
    std::vector<DescriptorSet> sets(layouts.size());
    const VkDescriptorSetAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = nativeDescriptorPool(),
        .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
        .pSetLayouts = native_layouts.data(),
    };
    const auto result = vkAllocateDescriptorSets(nativeVkDevice(), &info, handles.data());
    if (result != VK_SUCCESS)
      // Vulkan allocation failure is atomic; failed outputs are never adopted.
      return failure(vk::make_error_code(static_cast<vk::Result>(result)),
                     "vkAllocateDescriptorSets", "native descriptor set allocation failed");
    for (const auto handle : handles) {
      if (handle == VK_NULL_HANDLE) {
        implementation_->state = DescriptorPoolState::degraded;
        return fail(DescriptorPoolError::native_null_handle);
      }
    }
    // All storage predates the native call; publication cannot allocate or throw.
    for (std::size_t index = 0; index < handles.size(); ++index)
      sets[index] = DescriptorSet{handles[index], nativeDescriptorPool(), nativeVkDevice(),
                                  native_layouts[index]};
    return sets;
  } catch (const std::bad_alloc &) {
    return fail(DescriptorPoolError::allocation_failed);
  } catch (const std::length_error &) {
    return fail(DescriptorPoolError::allocation_failed);
  }
}
auto DescriptorPool::allocate(const DescriptorSetLayout &layout)
    -> terreate::Result<DescriptorSet> {
  const auto *pointer = &layout;
  auto sets = allocate(std::span<const DescriptorSetLayout *const>{&pointer, 1});
  if (!sets)
    return std::unexpected(std::move(sets.error()));
  return std::move(sets->front());
}
auto DescriptorPool::free(std::span<DescriptorSet> sets) -> terreate::Result<void> {
  if (!valid())
    return fail(DescriptorPoolError::invalid_pool);
  if (state() != DescriptorPoolState::ready)
    return fail(DescriptorPoolError::degraded_pool);
  if (!(implementation_->config.effective.flags &
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet))
    return fail(DescriptorPoolError::free_not_supported);
  if (!validCount(sets.size()))
    return fail(DescriptorPoolError::invalid_count);
  for (const auto &set : sets)
    if (!set.correlatedWith(*this))
      return fail(DescriptorPoolError::invalid_set);
  try {
    std::unordered_set<VkDescriptorSet> unique;
    std::vector<VkDescriptorSet> handles;
    handles.reserve(sets.size());
    for (const auto &set : sets) {
      if (!unique.insert(set.nativeDescriptorSet()).second)
        return fail(DescriptorPoolError::invalid_set);
      handles.push_back(set.nativeDescriptorSet());
    }
    const auto result =
        vkFreeDescriptorSets(nativeVkDevice(), nativeDescriptorPool(),
                             static_cast<std::uint32_t>(handles.size()), handles.data());
    if (result != VK_SUCCESS) {
      implementation_->state = DescriptorPoolState::degraded;
      return failure(vk::make_error_code(static_cast<vk::Result>(result)), "vkFreeDescriptorSets",
                     "native free failed; explicit reset required");
    }
    for (auto &set : sets)
      set.invalidate();
    return {};
  } catch (const std::bad_alloc &) {
    return fail(DescriptorPoolError::allocation_failed);
  } catch (const std::length_error &) {
    return fail(DescriptorPoolError::allocation_failed);
  }
}
auto DescriptorPool::free(DescriptorSet &set) -> terreate::Result<void> {
  return free(std::span<DescriptorSet>{&set, 1});
}
auto DescriptorPool::reset() -> terreate::Result<void> {
  if (!valid())
    return fail(DescriptorPoolError::invalid_pool);
  const auto result = vkResetDescriptorPool(nativeVkDevice(), nativeDescriptorPool(), 0);
  if (result != VK_SUCCESS) {
    implementation_->state = DescriptorPoolState::degraded;
    return failure(vk::make_error_code(static_cast<vk::Result>(result)), "vkResetDescriptorPool",
                   "native reset failed; explicit recovery required");
  }
  implementation_->state = DescriptorPoolState::ready;
  return {};
}

DescriptorSet::DescriptorSet(VkDescriptorSet handle, VkDescriptorPool pool, VkDevice device,
                             VkDescriptorSetLayout layout) noexcept
    : handle_(handle), pool_(pool), device_(device), layout_(layout) {}
DescriptorSet::DescriptorSet(DescriptorSet &&other) noexcept { *this = std::move(other); }
DescriptorSet &DescriptorSet::operator=(DescriptorSet &&other) noexcept {
  if (this != &other) {
    handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
    pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
    device_ = std::exchange(other.device_, VK_NULL_HANDLE);
    layout_ = std::exchange(other.layout_, VK_NULL_HANDLE);
  }
  return *this;
}
void DescriptorSet::invalidate() noexcept {
  handle_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
}
bool DescriptorSet::valid() const noexcept { return handle_ != VK_NULL_HANDLE; }
vk::DescriptorSet DescriptorSet::nativeHandle() const noexcept {
  return vk::DescriptorSet{handle_};
}
VkDescriptorSet DescriptorSet::nativeDescriptorSet() const noexcept { return handle_; }
vk::DescriptorPool DescriptorSet::nativePool() const noexcept { return vk::DescriptorPool{pool_}; }
VkDescriptorPool DescriptorSet::nativeDescriptorPool() const noexcept { return pool_; }
vk::Device DescriptorSet::nativeDevice() const noexcept { return vk::Device{device_}; }
VkDevice DescriptorSet::nativeVkDevice() const noexcept { return device_; }
vk::DescriptorSetLayout DescriptorSet::nativeLayout() const noexcept {
  return vk::DescriptorSetLayout{layout_};
}
VkDescriptorSetLayout DescriptorSet::nativeDescriptorSetLayout() const noexcept { return layout_; }
bool DescriptorSet::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && nativeDevice() == device.nativeHandle();
}
bool DescriptorSet::correlatedWith(const DescriptorPool &pool) const noexcept {
  return valid() && pool.valid() && pool_ == pool.nativeDescriptorPool() &&
         device_ == pool.nativeVkDevice();
}
} // namespace terreate::graphics
