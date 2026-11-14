#include <terreate/graphics/rendering_commands.hpp>

#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

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

// Validates one attachment against the Device and the rendering instance, and
// returns its effective pointer-free state.
auto checkAttachment(const Device &device, const RenderingDescription &description,
                     const RenderingAttachmentDescription &attachment, AttachmentKind kind,
                     RenderingAttachmentState &state, const char *name) -> terreate::Result<void> {
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
} // namespace terreate::graphics
