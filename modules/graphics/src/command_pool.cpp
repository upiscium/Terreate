#include <terreate/graphics/command_pool.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace terreate::graphics {
namespace {
auto failure(CommandError error, const char *operation, const char *detail) {
  return std::unexpected(terreate::Error{make_error_code(error), operation, detail});
}
auto nativeFailure(VkResult result, const char *operation) {
  return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                         operation, "native command pool operation failed"});
}
} // namespace

struct CommandPool::Impl {
  VkDevice device = VK_NULL_HANDLE;
  VkCommandPool handle = VK_NULL_HANDLE;
  CommandPoolDescription description{};
  // Stable pool-owned records, not pointers to movable/destroyable wrappers.
  std::vector<std::unique_ptr<CommandBuffer::Record>> records{};
  ~Impl() {
    if (handle != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, handle, nullptr);
  }
};

CommandPool::CommandPool(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}
CommandPool::CommandPool(CommandPool &&) noexcept = default;
CommandPool &CommandPool::operator=(CommandPool &&) noexcept = default;
CommandPool::~CommandPool() = default;
bool CommandPool::valid() const noexcept { return implementation_ != nullptr; }
vk::CommandPool CommandPool::nativeHandle() const noexcept {
  return implementation_ ? vk::CommandPool{implementation_->handle} : vk::CommandPool{};
}
const CommandPoolDescription *CommandPool::requested() const noexcept {
  return implementation_ ? &implementation_->description : nullptr;
}
const CommandPoolDescription *CommandPool::effective() const noexcept { return requested(); }

terreate::Result<CommandBuffer> CommandPool::allocatePrimary() {
  if (!valid())
    return failure(CommandError::invalid_pool, "allocatePrimary", "moved-from pool");
  auto record = std::make_unique<CommandBuffer::Record>();
  record->pool_flags = requested()->flags;
  // Reserve before Vulkan so allocation failure cannot strand an untracked
  // native allocation. Every successful native allocation is pool-owned.
  implementation_->records.reserve(implementation_->records.size() + 1);
  VkCommandBufferAllocateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  info.commandPool = implementation_->handle;
  info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  info.commandBufferCount = 1;
  VkCommandBuffer handle = VK_NULL_HANDLE;
  const auto result = vkAllocateCommandBuffers(implementation_->device, &info, &handle);
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkAllocateCommandBuffers");
  // Defensive check for a malformed successful native result; a conforming
  // vkAllocateCommandBuffers call returns a valid buffer handle on success.
  if (handle == VK_NULL_HANDLE)
    return failure(CommandError::invalid_native_handle, "vkAllocateCommandBuffers",
                   "Vulkan reported success without a command buffer");
  record->handle = vk::CommandBuffer{handle};
  auto *borrow = record.get();
  implementation_->records.push_back(std::move(record));
  return CommandBuffer{borrow};
}

terreate::Result<void> CommandPool::reset(vk::CommandPoolResetFlags flags) {
  if (!valid())
    return failure(CommandError::invalid_pool, "reset pool", "moved-from pool");
  const auto bits = static_cast<VkCommandPoolResetFlags>(flags);
  if (bits & ~VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT)
    return failure(CommandError::unsupported_flags, "reset pool", "unknown reset flags");
  const auto result = vkResetCommandPool(implementation_->device, implementation_->handle, bits);
  for (auto &record : implementation_->records)
    record->state =
        result == VK_SUCCESS ? CommandBufferState::initial : CommandBufferState::invalid;
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkResetCommandPool");
  return {};
}

auto createCommandPool(const Device &device, const CommandPoolDescription &description)
    -> terreate::Result<CommandPool> {
  if (!device.valid() || device.plan() == nullptr)
    return failure(CommandError::invalid_parent, "createCommandPool", "live Device required");
  const auto &plan = *device.plan();
  const auto family = description.queue_family_index;
  const auto &families = plan.capabilities().queue_families;
  const auto &allocations = plan.queueAllocations();
  if (family >= families.size() ||
      std::none_of(allocations.begin(), allocations.end(),
                   [family](const auto &allocation) { return allocation.family_index == family; }))
    return failure(CommandError::invalid_queue_family, "createCommandPool",
                   "caller must select a Device-enabled queue family");
  const auto flags = static_cast<VkCommandPoolCreateFlags>(description.flags);
  constexpr auto allowed = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                           VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                           VK_COMMAND_POOL_CREATE_PROTECTED_BIT;
  if ((flags & ~allowed) || ((flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT) &&
                             (!plan.enabledFeatures11().protectedMemory ||
                              !(families[family].queueFlags & vk::QueueFlagBits::eProtected))))
    return failure(CommandError::unsupported_flags, "createCommandPool",
                   "unknown flags or protectedMemory/protected-family prerequisite missing");
  auto impl = std::make_unique<CommandPool::Impl>();
  impl->device = static_cast<VkDevice>(device.nativeHandle());
  impl->description = description;
  VkCommandPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  info.flags = flags;
  info.queueFamilyIndex = family;
  const auto result = vkCreateCommandPool(impl->device, &info, nullptr, &impl->handle);
  if (result != VK_SUCCESS) {
    // Failed Vulkan output parameters do not establish ownership.
    impl->handle = VK_NULL_HANDLE;
    return nativeFailure(result, "vkCreateCommandPool");
  }
  // Do not publish a wrapper with an empty handle even if a native driver
  // erroneously reports VK_SUCCESS. No native allocation is owned here.
  if (impl->handle == VK_NULL_HANDLE)
    return failure(CommandError::invalid_native_handle, "vkCreateCommandPool",
                   "Vulkan reported success without a command pool");
  return CommandPool{std::move(impl)};
}
} // namespace terreate::graphics
