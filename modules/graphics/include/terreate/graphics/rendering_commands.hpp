#ifndef TERREATE_GRAPHICS_RENDERING_COMMANDS_HPP
#define TERREATE_GRAPHICS_RENDERING_COMMANDS_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/image_view.hpp>

namespace terreate::graphics {

enum class RenderingCommandError : std::uint8_t {
  invalid_device = 1,
  invalid_command_buffer,
  not_recording,
  parent_mismatch,
  unsupported_queue,
  feature_not_enabled,
  invalid_attachment,
  invalid_render_area,
  invalid_layer_count,
  invalid_rendering_scope,
  invalid_viewport,
  invalid_scissor,
  allocation_failed,
  // Appended in #265 Phase C2; existing values are unchanged.
  invalid_buffer,
  invalid_binding_range,
  invalid_index_type,
};

[[nodiscard]] const std::error_category &rendering_command_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(RenderingCommandError error) noexcept;

/// One attachment of a Dynamic Rendering instance. `image` and `view` are
/// borrowed only during recordBeginRendering: ImageView does not expose the
/// image extent, usage or sample count, so both are required and the view must
/// be correlated with the image. The default layout is deliberately invalid.
struct RenderingAttachmentDescription {
  const Image *image = nullptr;
  const ImageView *view = nullptr;
  vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eLoad;
  vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore;
  vk::ClearValue clear_value{};
};

/// Explicit rendering instance: no flags, viewMask 0 and no resolve. Color
/// attachment order is the fragment output location order. Depth and stencil
/// are optional; when both are given they must use the same image view.
struct RenderingDescription {
  vk::Rect2D render_area{};
  std::uint32_t layer_count = 1;
  std::vector<RenderingAttachmentDescription> color_attachments{};
  std::optional<RenderingAttachmentDescription> depth_attachment{};
  std::optional<RenderingAttachmentDescription> stencil_attachment{};
};

/// Pointer-free effective state of one attachment. The native view handle is
/// an observation only; it neither retains nor proves liveness of the view.
struct RenderingAttachmentState {
  VkImageView view = VK_NULL_HANDLE;
  vk::Format format = vk::Format::eUndefined;
  vk::Extent2D extent{};
  vk::ImageLayout layout = vk::ImageLayout::eUndefined;
  vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eLoad;
  vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore;
  vk::ClearValue clear_value{};
};

struct RenderingState {
  vk::Rect2D render_area{};
  std::uint32_t layer_count = 1;
  std::vector<RenderingAttachmentState> color_attachments{};
  std::optional<RenderingAttachmentState> depth_attachment{};
  std::optional<RenderingAttachmentState> stencil_attachment{};
};

/// Proof that recordBeginRendering succeeded on one CommandBuffer. Draws
/// require it and recordEndRendering consumes it. It is move-only and holds
/// no ownership; destroying it without ending records nothing, which leaves
/// the command buffer inside the rendering instance: end it explicitly.
class RenderingScope final {
public:
  RenderingScope() = delete;
  RenderingScope(const RenderingScope &) = delete;
  RenderingScope &operator=(const RenderingScope &) = delete;
  RenderingScope(RenderingScope &&other) noexcept;
  // Not assignable: overwriting an active scope would lose the only value that
  // can end its rendering instance.
  RenderingScope &operator=(RenderingScope &&) = delete;
  ~RenderingScope() = default;

  /// False after recordEndRendering consumed it or after a move.
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return active(); }
  /// The CommandBuffer this instance was begun on (borrowed observation).
  [[nodiscard]] vk::CommandBuffer nativeCommandBuffer() const noexcept;
  /// Effective attachment state; empty after a move.
  [[nodiscard]] const RenderingState &state() const noexcept { return state_; }

private:
  RenderingScope(vk::CommandBuffer command, RenderingState state) noexcept;
  vk::CommandBuffer command_{};
  RenderingState state_{};
  friend auto recordBeginRendering(const Device &, const CommandBuffer &,
                                   const RenderingDescription &)
      -> terreate::Result<RenderingScope>;
  friend auto recordEndRendering(const Device &, const CommandBuffer &, RenderingScope &&)
      -> terreate::Result<void>;
};

/// Pure CPU validation of an attachment-independent part of a description:
/// color count within maxColorAttachments, nonempty render area with
/// nonnegative offset whose end fits int32_t and maxFramebufferWidth/Height,
/// and a layer count in [1, maxFramebufferLayers].
[[nodiscard]] auto validateRenderingDescription(const RenderingDescription &description,
                                                const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void>;
/// Positive width, nonzero height, extent within maxViewportDimensions, origin
/// and far corner within viewportBoundsRange, depths within [0, 1].
[[nodiscard]] auto validateViewport(const vk::Viewport &viewport,
                                    const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void>;
/// Nonnegative offset and offset + extent representable as int32_t.
[[nodiscard]] auto validateScissor(const vk::Rect2D &scissor) -> terreate::Result<void>;

/// Records exactly one vkCmdBeginRendering after validating, before any native
/// call: dynamicRendering enabled on the Device, a recording CommandBuffer of
/// that Device on a graphics queue family, and every attachment (live image
/// and correlated view on the Device, single-sample, matching attachment
/// usage and view aspect, an allowed layout, render area and layer count
/// inside the attachment). Layout transitions are NOT recorded: the caller
/// records barriers. No submit, wait or retention.
[[nodiscard]] auto recordBeginRendering(const Device &device, const CommandBuffer &command,
                                        const RenderingDescription &description)
    -> terreate::Result<RenderingScope>;
/// Records exactly one vkCmdEndRendering for an active scope of this
/// CommandBuffer and consumes it. On error the scope is left unchanged.
[[nodiscard]] auto recordEndRendering(const Device &device, const CommandBuffer &command,
                                      RenderingScope &&scope) -> terreate::Result<void>;

/// Record viewport/scissor 0 (count 1, matching GraphicsPipeline's dynamic
/// state). Allowed inside or outside a rendering instance.
[[nodiscard]] auto recordSetViewport(const Device &device, const CommandBuffer &command,
                                     const vk::Viewport &viewport) -> terreate::Result<void>;
[[nodiscard]] auto recordSetScissor(const Device &device, const CommandBuffer &command,
                                    const vk::Rect2D &scissor) -> terreate::Result<void>;

/// Records one vkCmdDraw inside `scope`, which must be active and belong to
/// `command`. A bound compatible graphics pipeline, viewport/scissor and
/// descriptor/push-constant state are caller responsibilities.
[[nodiscard]] auto recordDraw(const Device &device, const CommandBuffer &command,
                              const RenderingScope &scope, std::uint32_t vertex_count,
                              std::uint32_t instance_count, std::uint32_t first_vertex,
                              std::uint32_t first_instance) -> terreate::Result<void>;

/// One vertex buffer binding. `buffer` is borrowed only during
/// recordBindVertexBuffers; the caller keeps it alive through all GPU use.
struct VertexBufferBinding {
  const Buffer *buffer = nullptr;
  vk::DeviceSize offset = 0;
};

/// Records exactly one vkCmdBindVertexBuffers for bindings
/// [first_binding, first_binding + bindings.size()) after validating, before
/// any native call: a recording CommandBuffer of the Device on a graphics
/// queue family, a nonempty span within maxVertexInputBindings, and per
/// binding a live Buffer of the Device with VERTEX_BUFFER usage and an offset
/// below its size. Allowed inside or outside a rendering instance.
[[nodiscard]] auto recordBindVertexBuffers(const Device &device, const CommandBuffer &command,
                                           std::uint32_t first_binding,
                                           std::span<const VertexBufferBinding> bindings)
    -> terreate::Result<void>;
/// Records exactly one vkCmdBindIndexBuffer after validating a live Buffer of
/// the Device with INDEX_BUFFER usage, an offset below its size whose memory
/// address (allocation offset + offset) is aligned to the index size, and an
/// index type of UINT16 or UINT32. UINT8 needs indexTypeUint8, which a Device
/// cannot enable: DeviceDescription takes only pNext-free Vulkan 1.0-1.3
/// feature structures. The buffer is borrowed only during
/// the call. Allowed inside or outside a rendering instance.
[[nodiscard]] auto recordBindIndexBuffer(const Device &device, const CommandBuffer &command,
                                         const Buffer &buffer, vk::DeviceSize offset,
                                         vk::IndexType index_type) -> terreate::Result<void>;
/// Records one vkCmdDrawIndexed inside `scope`, which must be active and
/// belong to `command`. Binding state is not tracked: a bound index buffer
/// whose range covers [first_index, first_index + index_count), vertex buffers
/// matching the bound pipeline's vertex input, and the pipeline itself are
/// caller responsibilities.
[[nodiscard]] auto recordDrawIndexed(const Device &device, const CommandBuffer &command,
                                     const RenderingScope &scope, std::uint32_t index_count,
                                     std::uint32_t instance_count, std::uint32_t first_index,
                                     std::int32_t vertex_offset, std::uint32_t first_instance)
    -> terreate::Result<void>;

} // namespace terreate::graphics

namespace std {
template <> struct is_error_code_enum<terreate::graphics::RenderingCommandError> : true_type {};
} // namespace std

#endif // TERREATE_GRAPHICS_RENDERING_COMMANDS_HPP
