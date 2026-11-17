#ifndef TERREATE_GRAPHICS_DEBUG_UTILS_HPP
#define TERREATE_GRAPHICS_DEBUG_UTILS_HPP

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include <terreate/core/result.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/compute_pipeline.hpp>
#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/sampler.hpp>
#include <terreate/graphics/shader_module.hpp>

namespace terreate::graphics {

enum class DebugUtilsError : std::uint8_t {
  invalid_instance = 1,
  invalid_device,
  parent_mismatch,
  extension_not_enabled,
  function_unavailable,
  not_enabled,
  invalid_object,
  invalid_command_buffer,
  not_recording,
  invalid_queue,
  invalid_name,
  allocation_failed,
};

[[nodiscard]] const std::error_category &debug_utils_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DebugUtilsError error) noexcept;

/// The VK_EXT_debug_utils entry points a DebugUtils value loaded through
/// vkGetInstanceProcAddr. This is the native escape hatch: calls made through
/// these pointers bypass every Terreate check.
struct DebugUtilsNativeFunctions {
  PFN_vkSetDebugUtilsObjectNameEXT set_object_name = nullptr;
  PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin_label = nullptr;
  PFN_vkCmdEndDebugUtilsLabelEXT cmd_end_label = nullptr;
  PFN_vkCmdInsertDebugUtilsLabelEXT cmd_insert_label = nullptr;
  PFN_vkQueueBeginDebugUtilsLabelEXT queue_begin_label = nullptr;
  PFN_vkQueueEndDebugUtilsLabelEXT queue_end_label = nullptr;
  PFN_vkQueueInsertDebugUtilsLabelEXT queue_insert_label = nullptr;
};

/// One label. `name` is copied during the call; it must not contain NUL.
/// An all-zero color means "no color", as in Vulkan.
struct DebugLabel {
  std::string_view name{};
  std::array<float, 4> color{};
};

/// A plain, copyable, non-owning view of the Debug Utils entry points for one
/// logical Device. It exists only when the Instance effectively enabled
/// VK_EXT_debug_utils; there is no silent no-op mode. An application that
/// disables debugging keeps no DebugUtils (for example an empty
/// std::optional) and skips the calls. Every function below returns
/// DebugUtilsError::not_enabled for a default-constructed value without a
/// native call. Use after the Device or Instance is destroyed is invalid by
/// contract, as for Queue, and is not detected.
///
/// Correlation compares native handles, like every Graphics object's
/// correlatedWith(Device): an object or Device matches when its VkDevice is
/// this value's VkDevice, and a Queue matches when its VkQueue is one of the
/// Device's queues. A handle reused after destruction would match, which is
/// why stale use stays a caller contract rather than a checked condition.
class DebugUtils final {
public:
  DebugUtils() noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] bool valid() const noexcept { return device_ != VK_NULL_HANDLE; }
  [[nodiscard]] VkDevice nativeDevice() const noexcept { return device_; }
  [[nodiscard]] const DebugUtilsNativeFunctions &nativeFunctions() const noexcept {
    return functions_;
  }
  [[nodiscard]] bool correlatedWith(const Device &device) const noexcept;
  /// True when `queue` is one of the DebugUtils Device's queues.
  [[nodiscard]] bool correlatedWith(const Queue &queue) const noexcept;

private:
  DebugUtils(VkDevice device, std::span<const Queue> queues,
             const DebugUtilsNativeFunctions &functions) noexcept
      : device_(device), queues_(queues), functions_(functions) {}

  VkDevice device_ = VK_NULL_HANDLE;
  // Borrowed from the Device's heap-owned state, so it survives Device moves.
  std::span<const Queue> queues_{};
  DebugUtilsNativeFunctions functions_{};

  friend auto createDebugUtils(const Instance &instance, const Device &device)
      -> terreate::Result<DebugUtils>;
};

/// Load the Debug Utils entry points for `device`. Fails with
/// extension_not_enabled when the Instance plan did not effectively enable
/// VK_EXT_debug_utils, and with parent_mismatch when `device` was not created
/// from `instance`. Both are borrowed only during the call.
[[nodiscard]] auto createDebugUtils(const Instance &instance, const Device &device)
    -> terreate::Result<DebugUtils>;

/// Name an object of the DebugUtils Device. Each typed overload rejects an
/// invalid object or one of another Device (parent_mismatch) before the single
/// native call. `name` is copied during the call and must not contain NUL; an
/// empty name removes the object's name, as in Vulkan.
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const Buffer &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const Image &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const ImageView &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const Sampler &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const ShaderModule &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const PipelineLayout &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const ComputePipeline &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const GraphicsPipeline &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const DescriptorSetLayout &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const DescriptorPool &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const CommandBuffer &object,
                                 std::string_view name) -> terreate::Result<void>;
[[nodiscard]] auto setObjectName(const DebugUtils &debug, const Queue &object,
                                 std::string_view name) -> terreate::Result<void>;

/// Name any native object of the DebugUtils Device. The caller vouches that
/// `handle` is a live object of that Device and of `type`; only
/// vk::ObjectType::eUnknown and a null handle are rejected.
[[nodiscard]] auto setObjectName(const DebugUtils &debug, vk::ObjectType type, std::uint64_t handle,
                                 std::string_view name) -> terreate::Result<void>;

/// Command buffer label regions. Each requires a CommandBuffer of the
/// DebugUtils Device in the recording state and records exactly one native
/// call, or none on error. Begin/end balance is not tracked: Vulkan lets a
/// region span command buffers, and the validation layer reports an
/// unbalanced end.
[[nodiscard]] auto recordBeginDebugLabel(const DebugUtils &debug, const CommandBuffer &command,
                                         const DebugLabel &label) -> terreate::Result<void>;
[[nodiscard]] auto recordEndDebugLabel(const DebugUtils &debug, const CommandBuffer &command)
    -> terreate::Result<void>;
[[nodiscard]] auto recordInsertDebugLabel(const DebugUtils &debug, const CommandBuffer &command,
                                          const DebugLabel &label) -> terreate::Result<void>;

/// Queue label regions on a Queue of the DebugUtils Device. Like submit, the
/// caller externally synchronizes the queue. Balance is not tracked.
[[nodiscard]] auto beginQueueDebugLabel(const DebugUtils &debug, const Queue &queue,
                                        const DebugLabel &label) -> terreate::Result<void>;
[[nodiscard]] auto endQueueDebugLabel(const DebugUtils &debug, const Queue &queue)
    -> terreate::Result<void>;
[[nodiscard]] auto insertQueueDebugLabel(const DebugUtils &debug, const Queue &queue,
                                         const DebugLabel &label) -> terreate::Result<void>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::DebugUtilsError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_DEBUG_UTILS_HPP
