#include <terreate/graphics/rendering_commands.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <terreate/graphics/pipeline_commands.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <vulkan/vulkan_format_traits.hpp>

namespace terreate::graphics {
namespace {
class RenderingCommandErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.rendering_commands"; }
  std::string message(int value) const override {
    switch (static_cast<RenderingCommandError>(value)) {
    case RenderingCommandError::invalid_device:
      return "invalid logical Device";
    case RenderingCommandError::invalid_command_buffer:
      return "invalid CommandBuffer";
    case RenderingCommandError::not_recording:
      return "CommandBuffer is not recording";
    case RenderingCommandError::parent_mismatch:
      return "object belongs to another logical Device";
    case RenderingCommandError::unsupported_queue:
      return "command queue family does not support graphics";
    case RenderingCommandError::feature_not_enabled:
      return "dynamicRendering is not enabled on the Device";
    case RenderingCommandError::invalid_attachment:
      return "invalid rendering attachment";
    case RenderingCommandError::invalid_render_area:
      return "render area empty or outside an attachment";
    case RenderingCommandError::invalid_layer_count:
      return "layer count zero or above an attachment's layers";
    case RenderingCommandError::invalid_rendering_scope:
      return "rendering scope inactive or begun on another CommandBuffer";
    case RenderingCommandError::invalid_viewport:
      return "invalid viewport";
    case RenderingCommandError::invalid_scissor:
      return "invalid scissor";
    case RenderingCommandError::allocation_failed:
      return "temporary storage allocation failed";
    case RenderingCommandError::invalid_buffer:
      return "invalid vertex or index buffer, usage or offset";
    case RenderingCommandError::invalid_binding_range:
      return "vertex binding range empty or above maxVertexInputBindings";
    case RenderingCommandError::invalid_index_type:
      return "index type is not UINT16 or UINT32";
    case RenderingCommandError::binding_state_mismatch:
      return "binding state belongs to another CommandBuffer or has no pipeline bound";
    case RenderingCommandError::missing_vertex_buffer:
      return "a pipeline vertex binding has no bound vertex buffer";
    case RenderingCommandError::vertex_range_out_of_bounds:
      return "draw fetches past the end of a bound vertex buffer";
    case RenderingCommandError::missing_index_buffer:
      return "indexed draw without a bound index buffer";
    case RenderingCommandError::index_range_out_of_bounds:
      return "indexed draw reads past the end of the bound index buffer";
    }
    return "unknown rendering command error";
  }
};

terreate::Error makeError(RenderingCommandError error, const char *name) {
  try {
    return terreate::Error{make_error_code(error), name,
                           "recording rejected before native Vulkan call"};
  } catch (const std::bad_alloc &) {
    // Code-only Error construction uses only empty strings.
    return terreate::Error{make_error_code(error)};
  }
}

terreate::Result<void> fail(RenderingCommandError error, const char *name) {
  return std::unexpected(makeError(error, name));
}

terreate::Result<void> withContext(terreate::Result<void> result, const char *name) {
  if (result)
    return result;
  return fail(static_cast<RenderingCommandError>(result.error().code().value()), name);
}

terreate::Result<void> validateRecording(const Device &device, const CommandBuffer &command,
                                         const char *name) {
  if (!device.valid() || !device.plan())
    return fail(RenderingCommandError::invalid_device, name);
  if (!command.valid())
    return fail(RenderingCommandError::invalid_command_buffer, name);
  if (command.nativeDevice() != device.nativeHandle())
    return fail(RenderingCommandError::parent_mismatch, name);
  if (command.state() != CommandBufferState::recording)
    return fail(RenderingCommandError::not_recording, name);
  const auto &families = device.plan()->capabilities().queue_families;
  const auto family = command.queueFamilyIndex();
  if (family >= families.size() || !(families[family].queueFlags & vk::QueueFlagBits::eGraphics))
    return fail(RenderingCommandError::unsupported_queue, name);
  return {};
}

terreate::Result<void> validateScope(const CommandBuffer &command, const RenderingScope &scope,
                                     const char *name) {
  if (!scope.active() || scope.nativeCommandBuffer() != command.nativeHandle())
    return fail(RenderingCommandError::invalid_rendering_scope, name);
  return {};
}

VkCommandBuffer nativeCommand(const CommandBuffer &command) noexcept {
  return static_cast<VkCommandBuffer>(command.nativeHandle());
}

enum class AttachmentKind : std::uint8_t { color, depth, stencil };

bool allowedLayout(AttachmentKind kind, vk::ImageLayout layout) noexcept {
  using L = vk::ImageLayout;
  if (layout == L::eGeneral || layout == L::eAttachmentOptimal)
    return true;
  switch (kind) {
  case AttachmentKind::color:
    return layout == L::eColorAttachmentOptimal;
  case AttachmentKind::depth:
    return layout == L::eDepthAttachmentOptimal || layout == L::eDepthStencilAttachmentOptimal;
  case AttachmentKind::stencil:
    return layout == L::eStencilAttachmentOptimal || layout == L::eDepthStencilAttachmentOptimal;
  }
  return false;
}

bool allowedOps(vk::AttachmentLoadOp load, vk::AttachmentStoreOp store) noexcept {
  using LO = vk::AttachmentLoadOp;
  using SO = vk::AttachmentStoreOp;
  return (load == LO::eLoad || load == LO::eClear || load == LO::eDontCare) &&
         (store == SO::eStore || store == SO::eDontCare || store == SO::eNone);
}

std::uint32_t mipDimension(std::uint32_t dimension, std::uint32_t level) noexcept {
  const auto shifted = level < 32 ? dimension >> level : 0U;
  return shifted ? shifted : 1U;
}

std::uint32_t viewLayers(const ImageConfiguration &image,
                         const vk::ImageSubresourceRange &range) noexcept {
  if (range.layerCount != VK_REMAINING_ARRAY_LAYERS)
    return range.layerCount;
  return range.baseArrayLayer < image.array_layers ? image.array_layers - range.baseArrayLayer : 0U;
}

// A Swapchain image as a color attachment: see
// RenderingAttachmentDescription::swapchain.
auto checkSwapchainAttachment(const Device &device, const RenderingDescription &description,
                              const RenderingAttachmentDescription &attachment, AttachmentKind kind,
                              RenderingAttachmentState &state, const char *name)
    -> terreate::Result<void> {
  const auto *swapchain = attachment.swapchain;
  const auto *image = attachment.swapchain_image;
  if (attachment.image != nullptr || attachment.view != nullptr || swapchain == nullptr ||
      image == nullptr || kind != AttachmentKind::color || !swapchain->valid() || !image->valid())
    return fail(RenderingCommandError::invalid_attachment, name);
  if (!swapchain->correlatedWith(device))
    return fail(RenderingCommandError::parent_mismatch, name);
  const auto *config = swapchain->configuration();
  if (config == nullptr || !swapchain->owns(*image) || !image->nativeImageView() ||
      !(config->usage & vk::ImageUsageFlagBits::eColorAttachment) ||
      !allowedLayout(kind, attachment.layout) ||
      !allowedOps(attachment.load_op, attachment.store_op))
    return fail(RenderingCommandError::invalid_attachment, name);
  const vk::Extent2D extent = image->extent();
  const auto &area = description.render_area;
  if (std::uint64_t(area.offset.x) + area.extent.width > extent.width ||
      std::uint64_t(area.offset.y) + area.extent.height > extent.height)
    return fail(RenderingCommandError::invalid_render_area, name);
  if (description.layer_count > config->requested.image_array_layers)
    return fail(RenderingCommandError::invalid_layer_count, name);
  state = {static_cast<VkImageView>(image->nativeImageView()),
           image->format(),
           extent,
           attachment.layout,
           attachment.load_op,
           attachment.store_op,
           attachment.clear_value};
  return {};
}

// Validates one attachment against the Device and the rendering instance, and
// returns its effective pointer-free state.
auto checkAttachment(const Device &device, const RenderingDescription &description,
                     const RenderingAttachmentDescription &attachment, AttachmentKind kind,
                     RenderingAttachmentState &state, const char *name) -> terreate::Result<void> {
  if (attachment.swapchain != nullptr || attachment.swapchain_image != nullptr)
    return checkSwapchainAttachment(device, description, attachment, kind, state, name);
  const auto *image = attachment.image;
  const auto *view = attachment.view;
  if (!image || !view || !image->valid() || !view->valid() || !image->configuration() ||
      !view->configuration())
    return fail(RenderingCommandError::invalid_attachment, name);
  if (image->nativeDevice() != device.nativeHandle() ||
      view->nativeDevice() != device.nativeHandle())
    return fail(RenderingCommandError::parent_mismatch, name);
  if (!view->correlatedWith(*image))
    return fail(RenderingCommandError::invalid_attachment, name);
  const auto &config = *image->configuration();
  const auto &effective = view->configuration()->effective;
  const auto &range = effective.subresource_range;
  const auto usage = kind == AttachmentKind::color
                         ? vk::ImageUsageFlagBits::eColorAttachment
                         : vk::ImageUsageFlagBits::eDepthStencilAttachment;
  const auto aspect = kind == AttachmentKind::color   ? vk::ImageAspectFlagBits::eColor
                      : kind == AttachmentKind::depth ? vk::ImageAspectFlagBits::eDepth
                                                      : vk::ImageAspectFlagBits::eStencil;
  const bool aspect_ok =
      kind == AttachmentKind::color ? range.aspectMask == aspect : bool(range.aspectMask & aspect);
  if (config.samples != vk::SampleCountFlagBits::e1 || !(config.usage & usage) || !aspect_ok ||
      range.levelCount != 1 ||
      (effective.view_type != vk::ImageViewType::e2D &&
       effective.view_type != vk::ImageViewType::e2DArray) ||
      !allowedLayout(kind, attachment.layout) ||
      !allowedOps(attachment.load_op, attachment.store_op))
    return fail(RenderingCommandError::invalid_attachment, name);

  const vk::Extent2D extent{mipDimension(config.extent.width, range.baseMipLevel),
                            mipDimension(config.extent.height, range.baseMipLevel)};
  const auto &area = description.render_area;
  if (std::uint64_t(area.offset.x) + area.extent.width > extent.width ||
      std::uint64_t(area.offset.y) + area.extent.height > extent.height)
    return fail(RenderingCommandError::invalid_render_area, name);
  if (description.layer_count > viewLayers(config, range))
    return fail(RenderingCommandError::invalid_layer_count, name);

  state = {view->nativeImageView(), effective.format,   extent,
           attachment.layout,       attachment.load_op, attachment.store_op,
           attachment.clear_value};
  return {};
}

// A live Buffer of the Device with `usage` and an offset inside it.
auto checkBuffer(const Device &device, const Buffer *buffer, vk::DeviceSize offset,
                 vk::BufferUsageFlagBits usage, const char *name) -> terreate::Result<void> {
  if (!buffer || !buffer->valid() || !buffer->configuration())
    return fail(RenderingCommandError::invalid_buffer, name);
  if (!buffer->correlatedWith(device))
    return fail(RenderingCommandError::parent_mismatch, name);
  const auto &config = *buffer->configuration();
  if (!(config.usage & usage) || offset >= config.size)
    return fail(RenderingCommandError::invalid_buffer, name);
  return {};
}

VkRenderingAttachmentInfo nativeAttachment(const RenderingAttachmentState &state) noexcept {
  VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  info.imageView = state.view;
  info.imageLayout = static_cast<VkImageLayout>(state.layout);
  info.resolveMode = VK_RESOLVE_MODE_NONE;
  info.resolveImageView = VK_NULL_HANDLE;
  info.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.loadOp = static_cast<VkAttachmentLoadOp>(state.load_op);
  info.storeOp = static_cast<VkAttachmentStoreOp>(state.store_op);
  info.clearValue = static_cast<VkClearValue>(state.clear_value);
  return info;
}
} // namespace

const std::error_category &rendering_command_error_category() noexcept {
  static const RenderingCommandErrorCategory category;
  return category;
}
std::error_code make_error_code(RenderingCommandError error) noexcept {
  return {static_cast<int>(error), rendering_command_error_category()};
}

RenderingScope::RenderingScope(vk::CommandBuffer command, RenderingState state) noexcept
    : command_(command), state_(std::move(state)) {}
RenderingScope::RenderingScope(RenderingScope &&other) noexcept
    : command_(std::exchange(other.command_, vk::CommandBuffer{})),
      state_(std::exchange(other.state_, RenderingState{})) {}
bool RenderingScope::active() const noexcept { return command_ != vk::CommandBuffer{}; }
vk::CommandBuffer RenderingScope::nativeCommandBuffer() const noexcept { return command_; }

auto validateRenderingDescription(const RenderingDescription &description,
                                  const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void> {
  constexpr auto name = "validateRenderingDescription";
  if (description.color_attachments.size() > limits.maxColorAttachments)
    return fail(RenderingCommandError::invalid_attachment, name);
  const auto &area = description.render_area;
  constexpr auto max = std::uint64_t(std::numeric_limits<std::int32_t>::max());
  if (area.offset.x < 0 || area.offset.y < 0 || area.extent.width == 0 || area.extent.height == 0 ||
      std::uint64_t(area.offset.x) + area.extent.width > max ||
      std::uint64_t(area.offset.y) + area.extent.height > max ||
      std::uint64_t(area.offset.x) + area.extent.width > limits.maxFramebufferWidth ||
      std::uint64_t(area.offset.y) + area.extent.height > limits.maxFramebufferHeight)
    return fail(RenderingCommandError::invalid_render_area, name);
  if (description.layer_count == 0 || description.layer_count > limits.maxFramebufferLayers)
    return fail(RenderingCommandError::invalid_layer_count, name);
  return {};
}

auto validateViewport(const vk::Viewport &viewport, const vk::PhysicalDeviceLimits &limits)
    -> terreate::Result<void> {
  const auto low = limits.viewportBoundsRange[0];
  const auto high = limits.viewportBoundsRange[1];
  // Comparisons are written so that NaN fails every check.
  const bool ok =
      viewport.width > 0.0F && viewport.width <= float(limits.maxViewportDimensions[0]) &&
      std::fabs(viewport.height) > 0.0F &&
      std::fabs(viewport.height) <= float(limits.maxViewportDimensions[1]) && viewport.x >= low &&
      viewport.x + viewport.width <= high && viewport.y >= low && viewport.y <= high &&
      viewport.y + viewport.height >= low && viewport.y + viewport.height <= high &&
      viewport.minDepth >= 0.0F && viewport.minDepth <= 1.0F && viewport.maxDepth >= 0.0F &&
      viewport.maxDepth <= 1.0F;
  if (!ok)
    return fail(RenderingCommandError::invalid_viewport, "validateViewport");
  return {};
}

auto validateScissor(const vk::Rect2D &scissor) -> terreate::Result<void> {
  constexpr auto max = std::uint64_t(std::numeric_limits<std::int32_t>::max());
  if (scissor.offset.x < 0 || scissor.offset.y < 0 ||
      std::uint64_t(scissor.offset.x) + scissor.extent.width > max ||
      std::uint64_t(scissor.offset.y) + scissor.extent.height > max)
    return fail(RenderingCommandError::invalid_scissor, "validateScissor");
  return {};
}

auto recordBeginRendering(const Device &device, const CommandBuffer &command,
                          const RenderingDescription &description)
    -> terreate::Result<RenderingScope> {
  constexpr auto name = "recordBeginRendering";
  const auto reject = [&](terreate::Result<void> result) -> terreate::Result<RenderingScope> {
    return std::unexpected(std::move(result.error()));
  };
  if (auto valid = validateRecording(device, command, name); !valid)
    return reject(std::move(valid));
  if (!device.plan()->enabledFeatures13().dynamicRendering)
    return reject(fail(RenderingCommandError::feature_not_enabled, name));
  if (auto valid = validateRenderingDescription(description,
                                                device.plan()->capabilities().properties.limits);
      !valid)
    return reject(withContext(std::move(valid), name));
  const auto &depth = description.depth_attachment;
  const auto &stencil = description.stencil_attachment;
  if (depth && stencil && depth->view != stencil->view)
    return reject(fail(RenderingCommandError::invalid_attachment, name));

  RenderingState state;
  std::vector<VkRenderingAttachmentInfo> colors;
  try {
    state.color_attachments.resize(description.color_attachments.size());
    colors.reserve(description.color_attachments.size());
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(RenderingCommandError::allocation_failed)});
  }
  state.render_area = description.render_area;
  state.layer_count = description.layer_count;
  for (std::size_t index = 0; index < description.color_attachments.size(); ++index)
    if (auto valid = checkAttachment(device, description, description.color_attachments[index],
                                     AttachmentKind::color, state.color_attachments[index], name);
        !valid)
      return reject(std::move(valid));
  if (depth) {
    state.depth_attachment.emplace();
    if (auto valid = checkAttachment(device, description, *depth, AttachmentKind::depth,
                                     *state.depth_attachment, name);
        !valid)
      return reject(std::move(valid));
  }
  if (stencil) {
    state.stencil_attachment.emplace();
    if (auto valid = checkAttachment(device, description, *stencil, AttachmentKind::stencil,
                                     *state.stencil_attachment, name);
        !valid)
      return reject(std::move(valid));
  }

  for (const auto &color : state.color_attachments)
    colors.push_back(nativeAttachment(color));
  const auto depth_info =
      state.depth_attachment
          ? nativeAttachment(*state.depth_attachment)
          : VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  const auto stencil_info =
      state.stencil_attachment
          ? nativeAttachment(*state.stencil_attachment)
          : VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.flags = 0;
  info.renderArea = static_cast<VkRect2D>(description.render_area);
  info.layerCount = description.layer_count;
  info.viewMask = 0;
  info.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
  info.pColorAttachments = colors.empty() ? nullptr : colors.data();
  info.pDepthAttachment = state.depth_attachment ? &depth_info : nullptr;
  info.pStencilAttachment = state.stencil_attachment ? &stencil_info : nullptr;
  vkCmdBeginRendering(nativeCommand(command), &info);
  return RenderingScope{command.nativeHandle(), std::move(state)};
}

auto recordEndRendering(const Device &device, const CommandBuffer &command, RenderingScope &&scope)
    -> terreate::Result<void> {
  constexpr auto name = "recordEndRendering";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateScope(command, scope, name); !valid)
    return valid;
  vkCmdEndRendering(nativeCommand(command));
  scope.command_ = vk::CommandBuffer{};
  scope.state_ = RenderingState{};
  return {};
}

auto recordSetViewport(const Device &device, const CommandBuffer &command,
                       const vk::Viewport &viewport) -> terreate::Result<void> {
  constexpr auto name = "recordSetViewport";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateViewport(viewport, device.plan()->capabilities().properties.limits);
      !valid)
    return withContext(std::move(valid), name);
  const auto native = static_cast<VkViewport>(viewport);
  vkCmdSetViewport(nativeCommand(command), 0, 1, &native);
  return {};
}

auto recordSetScissor(const Device &device, const CommandBuffer &command, const vk::Rect2D &scissor)
    -> terreate::Result<void> {
  constexpr auto name = "recordSetScissor";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateScissor(scissor); !valid)
    return withContext(std::move(valid), name);
  const auto native = static_cast<VkRect2D>(scissor);
  vkCmdSetScissor(nativeCommand(command), 0, 1, &native);
  return {};
}

auto recordDraw(const Device &device, const CommandBuffer &command, const RenderingScope &scope,
                std::uint32_t vertex_count, std::uint32_t instance_count,
                std::uint32_t first_vertex, std::uint32_t first_instance)
    -> terreate::Result<void> {
  constexpr auto name = "recordDraw";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateScope(command, scope, name); !valid)
    return valid;
  vkCmdDraw(nativeCommand(command), vertex_count, instance_count, first_vertex, first_instance);
  return {};
}

auto recordBindVertexBuffers(const Device &device, const CommandBuffer &command,
                             std::uint32_t first_binding,
                             std::span<const VertexBufferBinding> bindings)
    -> terreate::Result<void> {
  constexpr auto name = "recordBindVertexBuffers";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  const auto limit = device.plan()->capabilities().properties.limits.maxVertexInputBindings;
  if (bindings.empty() || std::uint64_t{first_binding} + bindings.size() > limit)
    return fail(RenderingCommandError::invalid_binding_range, name);
  for (const auto &binding : bindings)
    if (auto valid = checkBuffer(device, binding.buffer, binding.offset,
                                 vk::BufferUsageFlagBits::eVertexBuffer, name);
        !valid)
      return valid;
  std::vector<VkBuffer> buffers;
  std::vector<VkDeviceSize> offsets;
  try {
    buffers.reserve(bindings.size());
    offsets.reserve(bindings.size());
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        terreate::Error{make_error_code(RenderingCommandError::allocation_failed)});
  }
  for (const auto &binding : bindings) {
    buffers.push_back(binding.buffer->nativeBuffer());
    offsets.push_back(binding.offset);
  }
  vkCmdBindVertexBuffers(nativeCommand(command), first_binding,
                         static_cast<std::uint32_t>(buffers.size()), buffers.data(),
                         offsets.data());
  return {};
}

auto recordBindIndexBuffer(const Device &device, const CommandBuffer &command, const Buffer &buffer,
                           vk::DeviceSize offset, vk::IndexType index_type)
    -> terreate::Result<void> {
  constexpr auto name = "recordBindIndexBuffer";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  vk::DeviceSize index_size = 0;
  if (index_type == vk::IndexType::eUint16)
    index_size = 2;
  else if (index_type == vk::IndexType::eUint32)
    index_size = 4;
  else
    return fail(RenderingCommandError::invalid_index_type, name);
  if (auto valid =
          checkBuffer(device, &buffer, offset, vk::BufferUsageFlagBits::eIndexBuffer, name);
      !valid)
    return valid;
  // VUID-vkCmdBindIndexBuffer-offset-08783 constrains the memory address, not
  // only the buffer offset: include where the buffer is bound in its memory.
  const auto allocation = buffer.allocationInfo();
  if (!allocation || (allocation->native.offset + offset) % index_size != 0)
    return fail(RenderingCommandError::invalid_buffer, name);
  vkCmdBindIndexBuffer(nativeCommand(command), buffer.nativeBuffer(), offset,
                       static_cast<VkIndexType>(index_type));
  return {};
}

auto recordDrawIndexed(const Device &device, const CommandBuffer &command,
                       const RenderingScope &scope, std::uint32_t index_count,
                       std::uint32_t instance_count, std::uint32_t first_index,
                       std::int32_t vertex_offset, std::uint32_t first_instance)
    -> terreate::Result<void> {
  constexpr auto name = "recordDrawIndexed";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateScope(command, scope, name); !valid)
    return valid;
  vkCmdDrawIndexed(nativeCommand(command), index_count, instance_count, first_index, vertex_offset,
                   first_instance);
  return {};
}

namespace {
// Every vertex binding of the bound pipeline needs a bound buffer.
terreate::Result<void> checkVertexBuffersPresent(const GraphicsBindingState &state,
                                                 const char *name) {
  if (!state.pipeline_bound)
    return fail(RenderingCommandError::binding_state_mismatch, name);
  for (const auto &binding : state.vertex_bindings)
    if (binding.binding >= state.vertex_bytes.size() || state.vertex_bytes[binding.binding] == 0)
      return fail(RenderingCommandError::missing_vertex_buffer, name);
  return {};
}

// Stateful draws: the state must have been filled on this CommandBuffer.
terreate::Result<void> checkStateOwner(const CommandBuffer &command,
                                       const GraphicsBindingState &state, const char *name) {
  if (state.command != command.nativeHandle() || !state.pipeline_bound)
    return fail(RenderingCommandError::binding_state_mismatch, name);
  return {};
}

// A state filled on another CommandBuffer starts over for this one.
GraphicsBindingState adoptedState(const CommandBuffer &command, const GraphicsBindingState &state) {
  if (state.command == command.nativeHandle())
    return state;
  GraphicsBindingState fresh;
  fresh.command = command.nativeHandle();
  return fresh;
}
} // namespace

auto validateDrawBindings(const GraphicsBindingState &state, std::uint32_t vertex_count,
                          std::uint32_t instance_count, std::uint32_t first_vertex,
                          std::uint32_t first_instance) -> terreate::Result<void> {
  constexpr auto name = "validateDrawBindings";
  if (auto present = checkVertexBuffersPresent(state, name); !present)
    return present;
  if (vertex_count == 0 || instance_count == 0)
    return {};
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  for (const auto &binding : state.vertex_bindings) {
    std::uint64_t attribute_end = 0;
    for (const auto &attribute : state.vertex_attributes)
      if (attribute.binding == binding.binding)
        attribute_end = std::max<std::uint64_t>(attribute_end, std::uint64_t{attribute.offset} +
                                                                   vk::blockSize(attribute.format));
    if (attribute_end == 0)
      continue; // no attribute fetches from this binding
    const bool per_vertex = binding.inputRate == vk::VertexInputRate::eVertex;
    // first + count - 1 fits in 33 bits, so it cannot overflow uint64_t.
    const std::uint64_t last = per_vertex ? std::uint64_t{first_vertex} + vertex_count - 1
                                          : std::uint64_t{first_instance} + instance_count - 1;
    const std::uint64_t stride = binding.stride;
    if (stride != 0 && last > (max - attribute_end) / stride)
      return fail(RenderingCommandError::vertex_range_out_of_bounds, name);
    if ((last * stride) + attribute_end > state.vertex_bytes[binding.binding])
      return fail(RenderingCommandError::vertex_range_out_of_bounds, name);
  }
  return {};
}

auto validateIndexedDrawBindings(const GraphicsBindingState &state, std::uint32_t index_count,
                                 std::uint32_t first_index) -> terreate::Result<void> {
  constexpr auto name = "validateIndexedDrawBindings";
  if (auto present = checkVertexBuffersPresent(state, name); !present)
    return present;
  if (!state.index)
    return fail(RenderingCommandError::missing_index_buffer, name);
  if (std::uint64_t{first_index} + index_count > state.index->available_indices)
    return fail(RenderingCommandError::index_range_out_of_bounds, name);
  return {};
}

auto recordBindPipeline(const Device &device, const CommandBuffer &command,
                        const GraphicsPipeline &pipeline, GraphicsBindingState &state)
    -> terreate::Result<void> {
  constexpr auto name = "recordBindPipeline";
  const auto *config = pipeline.configuration();
  if (!config) // invalid pipeline: report the stateless validation error
    return recordBindPipeline(device, command, pipeline);
  // Prepare every allocation before the native call; publish after success.
  GraphicsBindingState next;
  try {
    next = adoptedState(command, state);
    next.vertex_bindings = config->effective.vertex_bindings;
    next.vertex_attributes = config->effective.vertex_attributes;
  } catch (const std::bad_alloc &) {
    return fail(RenderingCommandError::allocation_failed, name);
  }
  next.pipeline_bound = true;
  if (auto bound = recordBindPipeline(device, command, pipeline); !bound)
    return bound;
  state = std::move(next);
  return {};
}

auto recordBindVertexBuffers(const Device &device, const CommandBuffer &command,
                             std::uint32_t first_binding,
                             std::span<const VertexBufferBinding> bindings,
                             GraphicsBindingState &state) -> terreate::Result<void> {
  constexpr auto name = "recordBindVertexBuffers";
  GraphicsBindingState next;
  try {
    next = adoptedState(command, state);
    const auto end = std::uint64_t{first_binding} + bindings.size();
    // An out-of-range request is rejected by the stateless validation below.
    if (end <= std::numeric_limits<std::uint32_t>::max() && next.vertex_bytes.size() < end)
      next.vertex_bytes.resize(static_cast<std::size_t>(end), 0);
  } catch (const std::bad_alloc &) {
    return fail(RenderingCommandError::allocation_failed, name);
  }
  if (auto bound = recordBindVertexBuffers(device, command, first_binding, bindings); !bound)
    return bound;
  // Validation succeeded, so every buffer is valid with offset < size.
  for (std::size_t index = 0; index < bindings.size(); ++index)
    next.vertex_bytes[first_binding + index] =
        bindings[index].buffer->configuration()->size - bindings[index].offset;
  state = std::move(next);
  return {};
}

auto recordBindIndexBuffer(const Device &device, const CommandBuffer &command, const Buffer &buffer,
                           vk::DeviceSize offset, vk::IndexType index_type,
                           GraphicsBindingState &state) -> terreate::Result<void> {
  constexpr auto name = "recordBindIndexBuffer";
  GraphicsBindingState next;
  try {
    next = adoptedState(command, state);
  } catch (const std::bad_alloc &) {
    return fail(RenderingCommandError::allocation_failed, name);
  }
  if (auto bound = recordBindIndexBuffer(device, command, buffer, offset, index_type); !bound)
    return bound;
  const vk::DeviceSize index_size = index_type == vk::IndexType::eUint16 ? 2 : 4;
  next.index = IndexBufferState{index_type, (buffer.configuration()->size - offset) / index_size};
  state = std::move(next);
  return {};
}

auto recordDraw(const Device &device, const CommandBuffer &command, const RenderingScope &scope,
                const GraphicsBindingState &state, std::uint32_t vertex_count,
                std::uint32_t instance_count, std::uint32_t first_vertex,
                std::uint32_t first_instance) -> terreate::Result<void> {
  constexpr auto name = "recordDraw";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateScope(command, scope, name); !valid)
    return valid;
  if (auto valid = checkStateOwner(command, state, name); !valid)
    return valid;
  if (auto valid =
          validateDrawBindings(state, vertex_count, instance_count, first_vertex, first_instance);
      !valid)
    return withContext(std::move(valid), name);
  vkCmdDraw(nativeCommand(command), vertex_count, instance_count, first_vertex, first_instance);
  return {};
}

auto recordDrawIndexed(const Device &device, const CommandBuffer &command,
                       const RenderingScope &scope, const GraphicsBindingState &state,
                       std::uint32_t index_count, std::uint32_t instance_count,
                       std::uint32_t first_index, std::int32_t vertex_offset,
                       std::uint32_t first_instance) -> terreate::Result<void> {
  constexpr auto name = "recordDrawIndexed";
  if (auto valid = validateRecording(device, command, name); !valid)
    return valid;
  if (auto valid = validateScope(command, scope, name); !valid)
    return valid;
  if (auto valid = checkStateOwner(command, state, name); !valid)
    return valid;
  if (auto valid = validateIndexedDrawBindings(state, index_count, first_index); !valid)
    return withContext(std::move(valid), name);
  vkCmdDrawIndexed(nativeCommand(command), index_count, instance_count, first_index, vertex_offset,
                   first_instance);
  return {};
}
} // namespace terreate::graphics
