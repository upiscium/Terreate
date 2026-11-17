#include <terreate/graphics/debug_utils.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <new>
#include <string>

namespace terreate::graphics {
namespace {
class DebugUtilsErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.debug_utils"; }
  std::string message(int value) const override {
    switch (static_cast<DebugUtilsError>(value)) {
    case DebugUtilsError::invalid_instance:
      return "invalid Instance";
    case DebugUtilsError::invalid_device:
      return "invalid logical Device";
    case DebugUtilsError::parent_mismatch:
      return "object belongs to another Instance or logical Device";
    case DebugUtilsError::extension_not_enabled:
      return "VK_EXT_debug_utils is not enabled on the Instance";
    case DebugUtilsError::function_unavailable:
      return "a Debug Utils entry point could not be loaded";
    case DebugUtilsError::not_enabled:
      return "DebugUtils value is not valid";
    case DebugUtilsError::invalid_object:
      return "invalid object to name";
    case DebugUtilsError::invalid_command_buffer:
      return "invalid CommandBuffer";
    case DebugUtilsError::not_recording:
      return "CommandBuffer is not recording";
    case DebugUtilsError::invalid_queue:
      return "invalid Queue";
    case DebugUtilsError::invalid_name:
      return "name or label contains NUL";
    case DebugUtilsError::allocation_failed:
      return "temporary name storage allocation failed";
    }
    return "unknown debug utils error";
  }
};

terreate::Result<void> fail(DebugUtilsError error, const char *name) {
  try {
    return std::unexpected(
        terreate::Error{make_error_code(error), name, "rejected before native Vulkan call"});
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return std::unexpected(terreate::Error{make_error_code(error)});
  }
}

terreate::Result<void> nativeFailure(VkResult result, const char *name) {
  try {
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                           name, "native Debug Utils call failed"});
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result))});
  }
}

// A NUL-terminated copy of a name. Short names stay on the stack so labels in
// a recording loop do not allocate.
class NameBuffer {
public:
  [[nodiscard]] bool assign(std::string_view name) {
    if (name.size() < inline_.size()) {
      if (!name.empty())
        std::memcpy(inline_.data(), name.data(), name.size());
      inline_[name.size()] = '\0';
      text_ = inline_.data();
      return true;
    }
    try {
      heap_.assign(name);
    } catch (const std::bad_alloc &) {
      return false;
    }
    text_ = heap_.c_str();
    return true;
  }
  [[nodiscard]] const char *c_str() const noexcept { return text_; }

private:
  std::array<char, 128> inline_{};
  std::string heap_{};
  const char *text_ = nullptr;
};

// Shared checks for every call that takes a name; on success `buffer` holds it.
terreate::Result<void> prepareName(const DebugUtils &debug, std::string_view name,
                                   NameBuffer &buffer, const char *operation) {
  if (!debug.valid())
    return fail(DebugUtilsError::not_enabled, operation);
  if (name.find('\0') != std::string_view::npos)
    return fail(DebugUtilsError::invalid_name, operation);
  if (!buffer.assign(name))
    return fail(DebugUtilsError::allocation_failed, operation);
  return {};
}

terreate::Result<void> nameObject(const DebugUtils &debug, VkObjectType type, std::uint64_t handle,
                                  std::string_view name) {
  constexpr auto operation = "setObjectName";
  NameBuffer buffer;
  if (auto prepared = prepareName(debug, name, buffer, operation); !prepared)
    return prepared;
  VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
  info.objectType = type;
  info.objectHandle = handle;
  // An empty name removes the object's name.
  info.pObjectName = name.empty() ? nullptr : buffer.c_str();
  const auto result = debug.nativeFunctions().set_object_name(debug.nativeDevice(), &info);
  if (result != VK_SUCCESS)
    return nativeFailure(result, "vkSetDebugUtilsObjectNameEXT");
  return {};
}

template <typename Object, typename Native>
terreate::Result<void> nameDeviceObject(const DebugUtils &debug, const Object &object,
                                        VkObjectType type, Native native, std::string_view name) {
  constexpr auto operation = "setObjectName";
  if (!debug.valid())
    return fail(DebugUtilsError::not_enabled, operation);
  if (!object.valid())
    return fail(DebugUtilsError::invalid_object, operation);
  if (static_cast<VkDevice>(object.nativeDevice()) != debug.nativeDevice())
    return fail(DebugUtilsError::parent_mismatch, operation);
  // Vulkan defines objectHandle as the handle value cast to uint64_t.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return nameObject(debug, type, reinterpret_cast<std::uint64_t>(native), name);
}

VkDebugUtilsLabelEXT nativeLabel(const DebugLabel &label, const NameBuffer &buffer) noexcept {
  VkDebugUtilsLabelEXT native{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
  native.pLabelName = buffer.c_str();
  std::copy(label.color.begin(), label.color.end(), std::begin(native.color));
  return native;
}

terreate::Result<void> validateCommand(const DebugUtils &debug, const CommandBuffer &command,
                                       const char *operation) {
  if (!debug.valid())
    return fail(DebugUtilsError::not_enabled, operation);
  if (!command.valid())
    return fail(DebugUtilsError::invalid_command_buffer, operation);
  if (static_cast<VkDevice>(command.nativeDevice()) != debug.nativeDevice())
    return fail(DebugUtilsError::parent_mismatch, operation);
  if (command.state() != CommandBufferState::recording)
    return fail(DebugUtilsError::not_recording, operation);
  return {};
}

terreate::Result<void> validateQueue(const DebugUtils &debug, const Queue &queue,
                                     const char *operation) {
  if (!debug.valid())
    return fail(DebugUtilsError::not_enabled, operation);
  if (!queue.valid())
    return fail(DebugUtilsError::invalid_queue, operation);
  if (!debug.correlatedWith(queue))
    return fail(DebugUtilsError::parent_mismatch, operation);
  return {};
}

VkCommandBuffer nativeCommand(const CommandBuffer &command) noexcept {
  return static_cast<VkCommandBuffer>(command.nativeHandle());
}

VkQueue nativeQueue(const Queue &queue) noexcept {
  return static_cast<VkQueue>(queue.nativeHandle());
}

template <typename Function>
bool load(VkInstance instance, const char *name, Function &function) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  function = reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
  return function != nullptr;
}
} // namespace

const std::error_category &debug_utils_error_category() noexcept {
  static const DebugUtilsErrorCategory category;
  return category;
}
std::error_code make_error_code(DebugUtilsError error) noexcept {
  return {static_cast<int>(error), debug_utils_error_category()};
}

bool DebugUtils::correlatedWith(const Device &device) const noexcept {
  return valid() && device.valid() && static_cast<VkDevice>(device.nativeHandle()) == device_;
}

bool DebugUtils::correlatedWith(const Queue &queue) const noexcept {
  return valid() && queue.valid() && std::ranges::any_of(queues_, [&](const Queue &owned) {
           return owned.nativeHandle() == queue.nativeHandle();
         });
}

auto createDebugUtils(const Instance &instance, const Device &device)
    -> terreate::Result<DebugUtils> {
  const auto failure = [](DebugUtilsError error) -> terreate::Result<DebugUtils> {
    return std::unexpected(fail(error, "createDebugUtils").error());
  };
  if (!instance.valid() || instance.plan() == nullptr)
    return failure(DebugUtilsError::invalid_instance);
  if (!device.valid())
    return failure(DebugUtilsError::invalid_device);
  if (!device.correlatedWith(instance))
    return failure(DebugUtilsError::parent_mismatch);
  if (!instance.plan()->debugUtilsEnabled())
    return failure(DebugUtilsError::extension_not_enabled);

  const auto native_instance = static_cast<VkInstance>(instance.nativeHandle());
  DebugUtilsNativeFunctions functions;
  const bool loaded =
      load(native_instance, "vkSetDebugUtilsObjectNameEXT", functions.set_object_name) &&
      load(native_instance, "vkCmdBeginDebugUtilsLabelEXT", functions.cmd_begin_label) &&
      load(native_instance, "vkCmdEndDebugUtilsLabelEXT", functions.cmd_end_label) &&
      load(native_instance, "vkCmdInsertDebugUtilsLabelEXT", functions.cmd_insert_label) &&
      load(native_instance, "vkQueueBeginDebugUtilsLabelEXT", functions.queue_begin_label) &&
      load(native_instance, "vkQueueEndDebugUtilsLabelEXT", functions.queue_end_label) &&
      load(native_instance, "vkQueueInsertDebugUtilsLabelEXT", functions.queue_insert_label);
  if (!loaded)
    return failure(DebugUtilsError::function_unavailable);
  return DebugUtils{static_cast<VkDevice>(device.nativeHandle()), device.queues(), functions};
}

auto setObjectName(const DebugUtils &debug, const Buffer &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_BUFFER, object.nativeBuffer(), name);
}
auto setObjectName(const DebugUtils &debug, const Image &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_IMAGE,
                          static_cast<VkImage>(object.nativeHandle()), name);
}
auto setObjectName(const DebugUtils &debug, const ImageView &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_IMAGE_VIEW, object.nativeImageView(), name);
}
auto setObjectName(const DebugUtils &debug, const Sampler &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_SAMPLER, object.nativeSampler(), name);
}
auto setObjectName(const DebugUtils &debug, const ShaderModule &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_SHADER_MODULE, object.nativeShaderModule(),
                          name);
}
auto setObjectName(const DebugUtils &debug, const PipelineLayout &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                          object.nativePipelineLayout(), name);
}
auto setObjectName(const DebugUtils &debug, const ComputePipeline &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_PIPELINE, object.nativePipeline(), name);
}
auto setObjectName(const DebugUtils &debug, const GraphicsPipeline &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_PIPELINE, object.nativePipeline(), name);
}
auto setObjectName(const DebugUtils &debug, const DescriptorSetLayout &object,
                   std::string_view name) -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                          object.nativeDescriptorSetLayout(), name);
}
auto setObjectName(const DebugUtils &debug, const DescriptorPool &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                          object.nativeDescriptorPool(), name);
}
auto setObjectName(const DebugUtils &debug, const CommandBuffer &object, std::string_view name)
    -> terreate::Result<void> {
  return nameDeviceObject(debug, object, VK_OBJECT_TYPE_COMMAND_BUFFER, nativeCommand(object),
                          name);
}
auto setObjectName(const DebugUtils &debug, const Queue &object, std::string_view name)
    -> terreate::Result<void> {
  constexpr auto operation = "setObjectName";
  if (!debug.valid())
    return fail(DebugUtilsError::not_enabled, operation);
  if (!object.valid())
    return fail(DebugUtilsError::invalid_object, operation);
  if (!debug.correlatedWith(object))
    return fail(DebugUtilsError::parent_mismatch, operation);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return nameObject(debug, VK_OBJECT_TYPE_QUEUE,
                    reinterpret_cast<std::uint64_t>(nativeQueue(object)), name);
}

auto setObjectName(const DebugUtils &debug, vk::ObjectType type, std::uint64_t handle,
                   std::string_view name) -> terreate::Result<void> {
  constexpr auto operation = "setObjectName";
  if (!debug.valid())
    return fail(DebugUtilsError::not_enabled, operation);
  if (type == vk::ObjectType::eUnknown || handle == 0)
    return fail(DebugUtilsError::invalid_object, operation);
  return nameObject(debug, static_cast<VkObjectType>(type), handle, name);
}

auto recordBeginDebugLabel(const DebugUtils &debug, const CommandBuffer &command,
                           const DebugLabel &label) -> terreate::Result<void> {
  constexpr auto operation = "recordBeginDebugLabel";
  if (auto valid = validateCommand(debug, command, operation); !valid)
    return valid;
  NameBuffer buffer;
  if (auto prepared = prepareName(debug, label.name, buffer, operation); !prepared)
    return prepared;
  const auto native = nativeLabel(label, buffer);
  debug.nativeFunctions().cmd_begin_label(nativeCommand(command), &native);
  return {};
}

auto recordEndDebugLabel(const DebugUtils &debug, const CommandBuffer &command)
    -> terreate::Result<void> {
  if (auto valid = validateCommand(debug, command, "recordEndDebugLabel"); !valid)
    return valid;
  debug.nativeFunctions().cmd_end_label(nativeCommand(command));
  return {};
}

auto recordInsertDebugLabel(const DebugUtils &debug, const CommandBuffer &command,
                            const DebugLabel &label) -> terreate::Result<void> {
  constexpr auto operation = "recordInsertDebugLabel";
  if (auto valid = validateCommand(debug, command, operation); !valid)
    return valid;
  NameBuffer buffer;
  if (auto prepared = prepareName(debug, label.name, buffer, operation); !prepared)
    return prepared;
  const auto native = nativeLabel(label, buffer);
  debug.nativeFunctions().cmd_insert_label(nativeCommand(command), &native);
  return {};
}

auto beginQueueDebugLabel(const DebugUtils &debug, const Queue &queue, const DebugLabel &label)
    -> terreate::Result<void> {
  constexpr auto operation = "beginQueueDebugLabel";
  if (auto valid = validateQueue(debug, queue, operation); !valid)
    return valid;
  NameBuffer buffer;
  if (auto prepared = prepareName(debug, label.name, buffer, operation); !prepared)
    return prepared;
  const auto native = nativeLabel(label, buffer);
  debug.nativeFunctions().queue_begin_label(nativeQueue(queue), &native);
  return {};
}

auto endQueueDebugLabel(const DebugUtils &debug, const Queue &queue) -> terreate::Result<void> {
  if (auto valid = validateQueue(debug, queue, "endQueueDebugLabel"); !valid)
    return valid;
  debug.nativeFunctions().queue_end_label(nativeQueue(queue));
  return {};
}

auto insertQueueDebugLabel(const DebugUtils &debug, const Queue &queue, const DebugLabel &label)
    -> terreate::Result<void> {
  constexpr auto operation = "insertQueueDebugLabel";
  if (auto valid = validateQueue(debug, queue, operation); !valid)
    return valid;
  NameBuffer buffer;
  if (auto prepared = prepareName(debug, label.name, buffer, operation); !prepared)
    return prepared;
  const auto native = nativeLabel(label, buffer);
  debug.nativeFunctions().queue_insert_label(nativeQueue(queue), &native);
  return {};
}

} // namespace terreate::graphics
