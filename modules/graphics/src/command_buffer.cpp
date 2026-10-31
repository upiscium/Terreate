#include <terreate/graphics/command_buffer.hpp>

#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class CommandErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.command"; }
  std::string message(int value) const override {
    switch (static_cast<CommandError>(value)) {
    case CommandError::invalid_parent:
      return "invalid command pool Device parent";
    case CommandError::invalid_queue_family:
      return "queue family is not enabled on Device";
    case CommandError::unsupported_flags:
      return "unsupported command flags or prerequisites";
    case CommandError::invalid_pool:
      return "invalid command pool";
    case CommandError::invalid_buffer:
      return "invalid command buffer view";
    case CommandError::invalid_transition:
      return "invalid command buffer state transition";
    case CommandError::reset_not_enabled:
      return "individual command buffer reset not enabled";
    case CommandError::invalid_native_handle:
      return "Vulkan returned success with a null command handle";
    }
    return "unknown command error";
  }
};
auto failure(CommandError error, const char *operation) {
  return std::unexpected(terreate::Error{make_error_code(error), operation,
                                         "command operation rejected before Vulkan call"});
}
auto nativeFailure(VkResult result, const char *operation) {
  return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                         operation, "native command operation failed"});
}
} // namespace

const std::error_category &command_error_category() noexcept {
  static const CommandErrorCategory category;
  return category;
}
std::error_code make_error_code(CommandError error) noexcept {
  return {static_cast<int>(error), command_error_category()};
}

CommandBuffer::CommandBuffer(CommandBuffer &&other) noexcept
    : record_(std::exchange(other.record_, nullptr)) {}
CommandBuffer &CommandBuffer::operator=(CommandBuffer &&other) noexcept {
  if (this != &other)
    record_ = std::exchange(other.record_, nullptr);
  return *this;
}
bool CommandBuffer::valid() const noexcept { return record_ != nullptr; }
vk::CommandBuffer CommandBuffer::nativeHandle() const noexcept {
  return record_ ? record_->handle : vk::CommandBuffer{};
}
CommandBufferState CommandBuffer::state() const noexcept {
  return record_ ? record_->state : CommandBufferState::invalid;
}
terreate::Result<void> CommandBuffer::begin(vk::CommandBufferUsageFlags flags) {
  if (!valid())
    return failure(CommandError::invalid_buffer, "begin");
  if (state() != CommandBufferState::initial)
    return failure(CommandError::invalid_transition, "begin");
  const auto bits = static_cast<VkCommandBufferUsageFlags>(flags);
  constexpr auto allowed =
      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
  if ((bits & ~allowed) || bits == allowed)
    return failure(CommandError::unsupported_flags, "begin");
  VkCommandBufferBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  info.flags = bits;
  const auto result = vkBeginCommandBuffer(static_cast<VkCommandBuffer>(nativeHandle()), &info);
  record_->state =
      result == VK_SUCCESS ? CommandBufferState::recording : CommandBufferState::invalid;
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkBeginCommandBuffer");
  return {};
}
terreate::Result<void> CommandBuffer::end() {
  if (!valid())
    return failure(CommandError::invalid_buffer, "end");
  if (state() != CommandBufferState::recording)
    return failure(CommandError::invalid_transition, "end");
  const auto result = vkEndCommandBuffer(static_cast<VkCommandBuffer>(nativeHandle()));
  record_->state =
      result == VK_SUCCESS ? CommandBufferState::executable : CommandBufferState::invalid;
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkEndCommandBuffer");
  return {};
}
terreate::Result<void> CommandBuffer::reset(vk::CommandBufferResetFlags flags) {
  if (!valid())
    return failure(CommandError::invalid_buffer, "reset buffer");
  if (state() == CommandBufferState::recording)
    return failure(CommandError::invalid_transition, "reset buffer");
  if (!(record_->pool_flags & vk::CommandPoolCreateFlagBits::eResetCommandBuffer))
    return failure(CommandError::reset_not_enabled, "reset buffer");
  const auto bits = static_cast<VkCommandBufferResetFlags>(flags);
  if (bits & ~VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT)
    return failure(CommandError::unsupported_flags, "reset buffer");
  const auto result = vkResetCommandBuffer(static_cast<VkCommandBuffer>(nativeHandle()), bits);
  record_->state = result == VK_SUCCESS ? CommandBufferState::initial : CommandBufferState::invalid;
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkResetCommandBuffer");
  return {};
}
} // namespace terreate::graphics
