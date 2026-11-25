// A textured, indexed, depth-tested quad pair: a gap-finding example. It uses the
// Graphics API for everything it can (Allocator, vertex/index/uniform Buffers, an
// UploadBatch for a Buffer and an Image, Sampler, descriptor set, indexed draw,
// depth attachment) and says in this comment which native calls it still makes, so a
// missing API shows up here instead of being hidden.
//
// The frame loop is the triangle's (examples/triangle): application-owned, one frame
// in flight, Swapchain recreation on resize. What is new is the resources: two quads
// at different depths, a 64x64 checkerboard texture, and a tint written every frame
// through the mapped uniform buffer.
//
// Native calls this file still makes (the gap report is in README.md, G1 to G8):
//   - vkQueueWaitIdle where no present fence proves the presents are done: a generation
//     dropped behind an out_of_date present or on a device without present fences, a
//     pipeline rebuild after a format change, and shutdown, plus the Queue::nativeHandle
//     it needs (G7);
//   - glfwSetWindowSize on Window::nativeHandle (scripted resize only).
// Image layout transitions use imageBarrier and recordImageBarrier (#437). The one
// VkMemoryBarrier2 for the vertex and index data is still a raw struct (G1), and the
// example passes a VMA flag for the host-visible uniform Buffer (G4). It calls no query for
// format features, which is why the depth format is chosen blind (G3).

// Modes: no arguments runs until the window is closed. For tests:
//   --frames N          present N frames, then exit
//   --resize-at K       ask the window system to resize the window at frame K
//   --require-validation  fail unless the validation layer is present and silent
//   --require-resize    needs --resize-at; reports SKIP, never PASS, when the window
//                       system does not resize the window
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/barriers.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/command_buffer.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/completion.hpp>
#include <terreate/graphics/descriptor_pool.hpp>
#include <terreate/graphics/descriptor_set.hpp>
#include <terreate/graphics/descriptor_set_layout.hpp>
#include <terreate/graphics/descriptor_write.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/image.hpp>
#include <terreate/graphics/image_view.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/pipeline_commands.hpp>
#include <terreate/graphics/pipeline_layout.hpp>
#include <terreate/graphics/presentation.hpp>
#include <terreate/graphics/rendering_commands.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/sampler.hpp>
#include <terreate/graphics/shader_module.hpp>
#include <terreate/graphics/spirv_binary.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/surface.hpp>
#include <terreate/graphics/swapchain.hpp>
#include <terreate/graphics/swapchain_frame.hpp>
#include <terreate/graphics/swapchain_retirement.hpp>
#include <terreate/graphics/synchronization.hpp>
#include <terreate/graphics/transfer_commands.hpp>
#include <terreate/graphics/upload_batch.hpp>
#include <terreate/integration/platform_graphics/surface.hpp>
#include <terreate/platform/events.hpp>
#include <terreate/platform/runtime.hpp>
#include <terreate/platform/window.hpp>

#include "quad_spirv.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace graphics = terreate::graphics;
namespace platform = terreate::platform;
namespace integration = terreate::integration::platform_graphics;

struct Options {
  bool require_validation = false;
  bool require_resize = false;
  std::optional<std::size_t> frames{};
  std::optional<std::size_t> resize_at{};
};

int fail(const char *what) {
  std::fprintf(stderr, "example.textured_quad: error: %s\n", what);
  return EXIT_FAILURE;
}

template <typename T> int fail(const char *what, const terreate::Result<T> &result) {
  std::fprintf(stderr, "example.textured_quad: error: %s: %s\n", what,
               result.error().code().message().c_str());
  return EXIT_FAILURE;
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

// Counts validation warnings and errors and prints them.
struct ValidationSink {
  std::atomic<std::size_t> messages{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "example.textured_quad: validation: %s\n", event.message.c_str());
        messages.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

// What the run actually did, counted where each path is taken (nothing is
// inferred), and printed on one line at the end so a green run says what it covered.
struct Paths {
  std::size_t acquire_out_of_date = 0;
  std::size_t acquire_suboptimal = 0;
  std::size_t acquire_no_image = 0; // timeout or not_ready
  std::size_t present_out_of_date = 0;
  std::size_t present_suboptimal = 0;
  std::size_t zero_size_waits = 0;
  std::size_t deferred_zero_extent = 0;
  std::size_t recreations = 0;
  std::size_t retired_via_queue = 0;
  std::size_t retired_via_queue_idle = 0;
};

struct Outcome {
  std::size_t presented = 0;
  // --require-resize was given and the window system did not resize the window.
  bool resize_skipped = false;
};

// Everything that belongs to one Swapchain generation. It is dropped (or handed
// to the retirement queue) as a whole when the Swapchain is recreated.
struct Generation {
  /// One present semaphore per image index: reused only when that index is
  /// acquired again from this same Swapchain.
  std::vector<graphics::Semaphore> render_done{};
  /// One present fence per image index (VK_KHR_swapchain_maintenance1 only).
  std::vector<graphics::Fence> present_fences{};
  /// Which present fences were handed to a present that succeeded.
  std::vector<bool> armed{};
  /// A present returned an error or out_of_date: its fence may never signal, so
  /// this generation is dropped behind a queue idle instead.
  bool tainted = false;
  /// The depth target of this generation (one frame in flight, so one image). The view is
  /// declared after the image: it is destroyed first.
  std::optional<graphics::Image> depth_image{};
  std::optional<graphics::ImageView> depth_view{};
  /// Declared last, so it is destroyed first: before the semaphores and fences
  /// of its images.
  graphics::Swapchain swapchain;
};

// What stays alive behind a Swapchain that went to the retirement queue.
struct Retired {
  std::vector<graphics::Semaphore> render_done{};
  std::vector<graphics::Fence> present_fences{};
  std::optional<graphics::Image> depth_image{};
  std::optional<graphics::ImageView> depth_view{};
};

// The whole first mip level and array layer of a color image and of a depth image.
const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
const vk::ImageSubresourceRange depth_range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

bool parseOptions(int argc, char **argv, Options &options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    const auto number = [&](std::optional<std::size_t> &out) {
      if (i + 1 >= argc)
        return false;
      char *end = nullptr;
      const unsigned long long value = std::strtoull(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0')
        return false;
      out = static_cast<std::size_t>(value);
      return true;
    };
    if (argument == "--frames") {
      if (!number(options.frames))
        return false;
    } else if (argument == "--resize-at") {
      if (!number(options.resize_at))
        return false;
    } else if (argument == "--require-validation") {
      options.require_validation = true;
    } else if (argument == "--require-resize") {
      options.require_resize = true;
    } else {
      return false;
    }
  }
  // Requiring a resize needs one to be asked for.
  if (options.require_resize && !options.resize_at)
    return false;
  // A resize that is asked for after the last frame would never happen.
  if (options.frames && options.resize_at && *options.resize_at >= *options.frames)
    return false;
  return true;
}

int runApplication(const Options &options, ValidationSink &validation, Outcome &outcome) {
  // ---- Platform: a Runtime and a Window ----
  auto runtime = platform::createRuntime();
  if (!runtime)
    return fail("create the Platform Runtime (is a window system available?)", runtime);
  auto window =
      platform::createWindow(*runtime, {.width = 640, .height = 480, .title = "terreate"});
  if (!window)
    return fail("create the Window", window);
  const auto surface_extensions = runtime->requiredInstanceExtensions();
  if (!surface_extensions)
    return fail("ask GLFW for its Vulkan Instance extensions", surface_extensions);

  // ---- Instance, Surface ----
  const auto loader = graphics::queryInstanceCapabilities();
  if (!loader)
    return fail("query the Vulkan loader", loader);
  const bool has_validation =
      hasName(loader->available_layers, "VK_LAYER_KHRONOS_validation") &&
      hasName(loader->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (options.require_validation && !has_validation)
    return fail("--require-validation: the validation layer or VK_EXT_debug_utils is missing");
  // Present fences (VK_KHR_swapchain_maintenance1) are used when the loader has
  // the Instance extensions they depend on; otherwise the example falls back.
  const bool loader_has_maintenance =
      hasName(loader->available_extensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
      hasName(loader->available_extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
  graphics::InstanceDescription instance_description;
  instance_description.api_version = VK_API_VERSION_1_3;
  instance_description.application_name = "terreate-textured-quad";
  instance_description.required_extensions = *surface_extensions;
  if (loader_has_maintenance)
    instance_description.optional_extensions = {VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                                                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
  if (has_validation) {
    instance_description.required_layers = {"VK_LAYER_KHRONOS_validation"};
    instance_description.debug_utils = graphics::DebugUtilsMode::required;
  }
  const auto instance_plan = graphics::resolveInstance(instance_description, *loader);
  if (!instance_plan)
    return fail("resolve the Instance", instance_plan);
  auto instance =
      graphics::createInstance(*instance_plan, terreate::DiagnosticSinkView::bind(validation));
  if (!instance)
    return fail("create the Instance", instance);
  auto surface = integration::createWindowSurface(*instance, *runtime, *window);
  if (!surface)
    return fail("create the Surface from the Window", surface);

  // ---- Physical device and Device ----
  // The application's own policy: the first device that can present to this
  // Surface and satisfies the Device request below, preferring a discrete GPU.
  auto inventory = graphics::queryPhysicalDevices(*instance);
  if (!inventory)
    return fail("enumerate physical devices", inventory);
  graphics::PhysicalDeviceRequirements requirements;
  graphics::requirePresentationDevice(requirements);
  const auto evaluations = graphics::evaluateSurfaceCandidates(
      *instance, inventory->candidates(), *surface, requirements, graphics::PresentationRequest{});
  if (!evaluations)
    return fail("evaluate the devices for the Surface", evaluations);

  const graphics::PhysicalDeviceCandidate *chosen = nullptr;
  const graphics::SurfaceCandidateEvaluation *chosen_evaluation = nullptr;
  std::optional<graphics::DevicePlan> device_plan;
  bool use_present_fences = false;
  for (const bool want_fences : {true, false}) {
    for (const bool discrete_only : {true, false}) {
      for (const auto &evaluation : *evaluations) {
        if (!evaluation.selectable() || chosen != nullptr)
          continue;
        const graphics::PhysicalDeviceCandidate *candidate = nullptr;
        for (const auto &entry : *inventory)
          if (entry.enumerationIndex() == evaluation.enumeration_index)
            candidate = &entry;
        if (candidate == nullptr)
          continue;
        if (discrete_only &&
            candidate->capabilities().properties.deviceType != vk::PhysicalDeviceType::eDiscreteGpu)
          continue;
        const auto &caps = candidate->capabilities();
        if (want_fences && !(caps.swapchain_maintenance1_available &&
                             caps.swapchain_maintenance1.swapchainMaintenance1 == VK_TRUE))
          continue;
        graphics::DeviceDescription description;
        description.required_features_13.dynamicRendering = VK_TRUE;
        description.required_features_13.synchronization2 = VK_TRUE;
        if (want_fences) {
          description.required_extensions = {VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
          description.required_swapchain_maintenance1.swapchainMaintenance1 = VK_TRUE;
        }
        if (!evaluation.resolution ||
            !graphics::applyPresentation(description, *evaluation.resolution))
          continue;
        auto resolved = graphics::resolveDevice(*candidate, description);
        if (!resolved)
          continue;
        chosen = candidate;
        chosen_evaluation = &evaluation;
        device_plan = std::move(*resolved);
        use_present_fences = want_fences;
      }
    }
  }
  if (chosen == nullptr || !device_plan || chosen_evaluation == nullptr)
    return fail("no device can present to the window with dynamic rendering and synchronization2");
  auto device = graphics::createDevice(*instance, *device_plan);
  if (!device)
    return fail("create the Device", device);
  const graphics::Queue queue = device->queue("present");
  if (!queue.valid())
    return fail("the present Queue is missing");
  const auto native_queue = static_cast<VkQueue>(queue.nativeHandle());
  std::printf("example.textured_quad: device=%s present-fences=%s\n",
              chosen->capabilities().properties.deviceName.data(),
              use_present_fences ? "yes" : "no (queue idle)");

  constexpr vk::Format depth_format = vk::Format::eD32Sfloat;

  // ---- Allocator and the resources the quads need ----
  // The Allocator is declared first, so everything created from it is destroyed before it.
  auto allocator = graphics::createAllocator(*instance, *chosen, *device);
  if (!allocator)
    return fail("create the Allocator", allocator);

  // Two quads at different depths: five floats per vertex (x, y, z, u, v). The nearer quad
  // is drawn first and the farther one second, so the depth test is what hides the part of
  // the far quad that lies behind it.
  constexpr std::array<float, 40> vertices{
      -0.8F, -0.6F, 0.2F, 0.0F, 1.0F, 0.2F,  -0.6F, 0.2F, 1.0F, 1.0F,
      0.2F,  0.6F,  0.2F, 1.0F, 0.0F, -0.8F, 0.6F,  0.2F, 0.0F, 0.0F, // near quad
      -0.2F, -0.4F, 0.6F, 0.0F, 1.0F, 0.8F,  -0.4F, 0.6F, 1.0F, 1.0F,
      0.8F,  0.8F,  0.6F, 1.0F, 0.0F, -0.2F, 0.8F,  0.6F, 0.0F, 0.0F, // far quad
  };
  constexpr std::array<std::uint16_t, 12> indices{0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4};
  graphics::BufferDescription vertex_description;
  vertex_description.size = sizeof(vertices);
  vertex_description.usage =
      vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
  graphics::BufferDescription index_description;
  index_description.size = sizeof(indices);
  index_description.usage =
      vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
  auto vertex_buffer = graphics::createBuffer(*allocator, vertex_description);
  auto index_buffer = graphics::createBuffer(*allocator, index_description);
  if (!vertex_buffer || !index_buffer)
    return fail("create the vertex and index Buffers");

  // The tint is rewritten every frame, so its Buffer is host visible and stays mapped.
  graphics::BufferDescription uniform_description;
  uniform_description.size = sizeof(float) * 4;
  uniform_description.usage = vk::BufferUsageFlagBits::eUniformBuffer;
  uniform_description.allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  auto uniform_buffer = graphics::createBuffer(*allocator, uniform_description);
  if (!uniform_buffer)
    return fail("create the uniform Buffer", uniform_buffer);
  const auto mapped_uniform = uniform_buffer->map();
  if (!mapped_uniform)
    return fail("map the uniform Buffer", mapped_uniform);

  // A 64x64 checkerboard, generated here: eight squares of eight texels per side.
  constexpr std::uint32_t texture_size = 64;
  std::vector<std::uint8_t> texels(std::size_t{texture_size} * texture_size * 4);
  for (std::uint32_t y = 0; y < texture_size; ++y) {
    for (std::uint32_t x = 0; x < texture_size; ++x) {
      const bool light = (((x / 8) + (y / 8)) % 2) == 0;
      std::uint8_t *texel = &texels[(std::size_t{y} * texture_size + x) * 4];
      texel[0] = light ? 235 : 40;
      texel[1] = light ? 215 : 60;
      texel[2] = light ? 190 : 110;
      texel[3] = 255;
    }
  }
  graphics::ImageDescription texture_description;
  texture_description.format = vk::Format::eR8G8B8A8Unorm;
  texture_description.extent = vk::Extent3D{texture_size, texture_size, 1};
  texture_description.usage =
      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
  auto texture = graphics::createImage(*allocator, texture_description);
  if (!texture)
    return fail("create the texture Image", texture);
  graphics::ImageViewDescription texture_view_description;
  texture_view_description.format = vk::Format::eR8G8B8A8Unorm;
  texture_view_description.subresource_range =
      vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
  auto texture_view = graphics::createImageView(*device, *texture, texture_view_description);
  if (!texture_view)
    return fail("create the texture ImageView", texture_view);
  auto sampler = graphics::createSampler(*device, {});
  if (!sampler)
    return fail("create the Sampler", sampler);

  // ---- One descriptor set: the tint (binding 0) and the texture (binding 1) ----
  graphics::DescriptorSetLayoutDescription set_description;
  set_description.bindings = {
      {0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment},
      {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment}};
  auto set_layout = graphics::createDescriptorSetLayout(*device, set_description);
  if (!set_layout)
    return fail("create the DescriptorSetLayout", set_layout);
  graphics::DescriptorPoolDescription pool_description_sets;
  pool_description_sets.max_sets = 1;
  pool_description_sets.sizes = {{vk::DescriptorType::eUniformBuffer, 1},
                                 {vk::DescriptorType::eCombinedImageSampler, 1}};
  auto descriptor_pool = graphics::createDescriptorPool(*device, pool_description_sets);
  if (!descriptor_pool)
    return fail("create the DescriptorPool", descriptor_pool);
  auto descriptor_set = descriptor_pool->allocate(*set_layout);
  if (!descriptor_set)
    return fail("allocate the DescriptorSet", descriptor_set);
  const std::array<graphics::DescriptorWriteDescription, 2> writes{{
      {0, 0, vk::DescriptorType::eUniformBuffer,
       std::vector<graphics::DescriptorBufferWriteElement>{
           {&*uniform_buffer, 0, sizeof(float) * 4}}},
      {1, 0, vk::DescriptorType::eCombinedImageSampler,
       std::vector<graphics::DescriptorImageWriteElement>{
           {&*texture, &*texture_view, &*sampler, vk::ImageLayout::eShaderReadOnlyOptimal}}},
  }};
  if (!graphics::updateDescriptorSet(*device, *set_layout, *descriptor_set, writes))
    return fail("write the descriptor set");

  // ---- The pipeline for the Swapchain's format ----
  if (!chosen_evaluation->resolution)
    return fail("the chosen device has no presentation resolution");
  const graphics::PresentationResolution &presentation = *chosen_evaluation->resolution;
  const auto vertex_binary = graphics::SpirvBinary::fromWords(quad_spirv::vertex_words);
  const auto fragment_binary = graphics::SpirvBinary::fromWords(quad_spirv::fragment_words);
  if (!vertex_binary || !fragment_binary)
    return fail("the embedded SPIR-V is invalid");
  auto vertex_module = graphics::createShaderModule(*device, *vertex_binary);
  auto fragment_module = graphics::createShaderModule(*device, *fragment_binary);
  if (!vertex_module || !fragment_module)
    return fail("create the shader modules");
  const std::array<const graphics::DescriptorSetLayout *, 1> set_layouts{&*set_layout};
  auto pipeline_layout = graphics::createPipelineLayout(*device, set_layouts, {});
  if (!pipeline_layout)
    return fail("create the pipeline layout", pipeline_layout);
  std::optional<graphics::GraphicsPipeline> pipeline;
  vk::Format pipeline_format = vk::Format::eUndefined;
  const auto build_pipeline = [&](vk::Format format) {
    graphics::GraphicsPipelineDescription description;
    description.vertex_entry_point = "main";
    description.fragment_entry_point = "main";
    description.vertex_bindings = {
        vk::VertexInputBindingDescription{0, 5 * sizeof(float), vk::VertexInputRate::eVertex}};
    description.vertex_attributes = {
        vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32B32Sfloat, 0},
        vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32Sfloat, 3 * sizeof(float)}};
    description.depth_test_enable = true;
    description.depth_write_enable = true;
    description.depth_compare_op = vk::CompareOp::eLess;
    description.depth_attachment_format = depth_format;
    description.color_attachment_formats = {format};
    vk::PipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    description.color_blend_attachments = {blend};
    auto created = graphics::createGraphicsPipeline(*device, *pipeline_layout, *vertex_module,
                                                    *fragment_module, description);
    if (!created)
      return false;
    pipeline.reset();
    pipeline.emplace(std::move(*created));
    pipeline_format = format;
    return true;
  };

  // ---- Swapchain generations ----
  const auto make_generation = [&](graphics::Swapchain swapchain) -> std::optional<Generation> {
    Generation generation{.swapchain = std::move(swapchain)};
    const std::size_t images = generation.swapchain.images().size();
    generation.render_done.reserve(images);
    generation.present_fences.reserve(images);
    for (std::size_t i = 0; i < images; ++i) {
      auto semaphore = graphics::createSemaphore(*device, {});
      if (!semaphore)
        return std::nullopt;
      generation.render_done.push_back(std::move(*semaphore));
      if (use_present_fences) {
        auto fence = graphics::createFence(*device, {});
        if (!fence)
          return std::nullopt;
        generation.present_fences.push_back(std::move(*fence));
      }
    }
    generation.armed.assign(images, false);
    const vk::Extent2D extent = generation.swapchain.configuration()->extent;
    graphics::ImageDescription depth_description;
    depth_description.format = depth_format;
    depth_description.extent = vk::Extent3D{extent.width, extent.height, 1};
    depth_description.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    auto depth_image = graphics::createImage(*allocator, depth_description);
    if (!depth_image)
      return std::nullopt;
    graphics::ImageViewDescription depth_view_description;
    depth_view_description.format = depth_format;
    depth_view_description.subresource_range =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    auto depth_view = graphics::createImageView(*device, *depth_image, depth_view_description);
    if (!depth_view)
      return std::nullopt;
    generation.depth_image.emplace(std::move(*depth_image));
    generation.depth_view.emplace(std::move(*depth_view));
    return std::optional<Generation>{std::move(generation)};
  };

  const auto framebuffer = window->framebufferSize();
  if (!framebuffer || framebuffer->width == 0 || framebuffer->height == 0)
    return fail("the window has no framebuffer extent to start with");
  graphics::SwapchainRequest first_request;
  first_request.extent = vk::Extent2D{framebuffer->width, framebuffer->height};
  const auto swapchain_description =
      graphics::resolveSwapchain(chosen_evaluation->support, presentation, first_request);
  if (!swapchain_description)
    return fail("resolve the Swapchain", swapchain_description);
  auto first_swapchain =
      graphics::createSwapchain(*instance, *device, *surface, *swapchain_description);
  if (!first_swapchain)
    return fail("create the Swapchain", first_swapchain);
  if (!build_pipeline(first_swapchain->configuration()->format))
    return fail("create the graphics pipeline");
  std::optional<Generation> current = make_generation(std::move(*first_swapchain));
  if (!current)
    return fail("create the per-image synchronization objects");

  std::vector<Retired> retired;
  graphics::SwapchainRetirementQueue retirement;
  // The upload's staging memory is retired here with the receipt of the upload submission,
  // declared before `finish` so that `finish` drains it before it is destroyed.
  graphics::RetirementQueue upload_retirement;

  // ---- Frame objects: one frame in flight ----
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  pool_description.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  auto command_pool = graphics::createCommandPool(*device, pool_description);
  if (!command_pool)
    return fail("create the command pool", command_pool);
  auto command = command_pool->allocatePrimary();
  if (!command)
    return fail("allocate the command buffer", command);
  auto acquire_semaphore = graphics::createSemaphore(*device, {});
  auto frame_fence = graphics::createFence(*device, {.signaled = true});
  if (!acquire_semaphore || !frame_fence)
    return fail("create the frame synchronization objects");

  std::size_t frame_serial = 0;
  int identity_domain = 0;
  Paths paths;

  // Whatever happens next, including an early return on an error, this runs
  // before anything it covers is destroyed (it is declared after them): no GPU
  // work may still use the objects, and the retirement queue must be empty
  // (destroying a non-empty queue is a contract violation).
  struct ScopeExit {
    std::function<void()> fn;
    ScopeExit() = default;
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ~ScopeExit() {
      if (fn)
        fn();
    }
  } finish;
  finish.fn = [&] {
    vkQueueWaitIdle(native_queue);
    // After the idle every present fence is signaled, so one collect drains the
    // queue; a lost device is the only reason it may not.
    if (const auto uploads = upload_retirement.collect(); !uploads) [[maybe_unused]]
      const auto reclaimed_uploads = upload_retirement.reclaimAfterDeviceLoss(*device);
    const bool collected = retirement.collect(*device).has_value();
    if (!collected) [[maybe_unused]]
      const auto reclaimed = retirement.reclaimAfterDeviceLoss(*device);
    std::printf("example.textured_quad: shutdown retirement=queue-idle drained=%s\n",
                collected ? "by-collect" : "by-device-loss-reclaim");
    retired.clear();
  };

  // ---- Upload: the vertex and index data and the texture, in one submission ----
  // The staging memory belongs to `upload` and must live until the GPU is done with it, so
  // the block waits on the fence (an explicit wait of this file) before it ends.
  {
    graphics::BufferUploadItem vertex_item{&*vertex_buffer, 0, std::as_bytes(std::span{vertices})};
    graphics::BufferUploadItem index_item{&*index_buffer, 0, std::as_bytes(std::span{indices})};
    const std::array<graphics::BufferUploadItem, 2> buffer_items{vertex_item, index_item};
    graphics::ImageUploadItem texture_item;
    texture_item.destination = &*texture;
    texture_item.copy_layout = vk::ImageLayout::eTransferDstOptimal;
    texture_item.region.imageSubresource =
        VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    texture_item.region.imageExtent = VkExtent3D{texture_size, texture_size, 1};
    texture_item.bytes = std::as_bytes(std::span{texels});
    const std::array<graphics::ImageUploadItem, 1> image_items{texture_item};
    auto upload = graphics::prepareUploadBatch(
        *allocator,
        graphics::UploadBatchDescription{.buffers = buffer_items, .images = image_items});
    if (!upload)
      return fail("prepare the upload", upload);

    // The recorder never transitions a layout: the barriers are this file's.
    const VkImageMemoryBarrier2 to_transfer =
        graphics::imageBarrier(*texture, {.dst_stage = vk::PipelineStageFlagBits2::eTransfer,
                                          .dst_access = vk::AccessFlagBits2::eTransferWrite,
                                          .old_layout = vk::ImageLayout::eUndefined,
                                          .new_layout = vk::ImageLayout::eTransferDstOptimal,
                                          .subresource_range = color_range});
    const VkImageMemoryBarrier2 to_sampled =
        graphics::imageBarrier(*texture, {.src_stage = vk::PipelineStageFlagBits2::eTransfer,
                                          .src_access = vk::AccessFlagBits2::eTransferWrite,
                                          .dst_stage = vk::PipelineStageFlagBits2::eFragmentShader,
                                          .dst_access = vk::AccessFlagBits2::eShaderSampledRead,
                                          .old_layout = vk::ImageLayout::eTransferDstOptimal,
                                          .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                          .subresource_range = color_range});
    VkMemoryBarrier2 buffers_ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    buffers_ready.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    buffers_ready.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    buffers_ready.dstStageMask =
        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    buffers_ready.dstAccessMask =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
    if (!command->reset() || !command->begin() ||
        !graphics::recordBarriers(*device, *command, {.images = std::span{&to_transfer, 1}}) ||
        !graphics::recordUploadBatch(*device, *command, *upload) ||
        !graphics::recordBarriers(
            *device, *command,
            {.memory = std::span{&buffers_ready, 1}, .images = std::span{&to_sampled, 1}}) ||
        !command->end())
      return fail("record the upload");

    if (!frame_fence->reset())
      return fail("reset the frame fence");
    ++frame_serial;
    const std::array<const graphics::CommandBuffer *, 1> upload_buffers{&*command};
    const auto upload_receipt =
        graphics::submitFenced(*device, queue, *frame_fence, {&identity_domain, frame_serial},
                               {.command_buffers = upload_buffers});
    if (!upload_receipt)
      return fail("submit the upload", upload_receipt);
    // The staging memory must outlive the GPU's use of it: hand it to the queue with the
    // receipt instead of waiting here. The frame loop waits on this same fence before it reuses
    // the command buffer, and the scope guard drains the queue on every exit.
    const std::array<graphics::SubmissionReceipt, 1> upload_uses{*upload_receipt};
    if (!upload_retirement.retire(*upload, upload_uses)) {
      vkQueueWaitIdle(native_queue); // the owner stays here: the GPU must be done before it dies
      return fail("retire the upload staging memory");
    }
  }

  // Retire the current generation: behind the retirement queue when its present
  // fences prove the presents are done, else behind a queue idle (one native call).
  const auto drop_current = [&]() {
    if (!current)
      return;
    const char *fallback_reason = "no present fences on this device";
    if (use_present_fences && !current->tainted) {
      fallback_reason = "the retirement queue refused the generation";
      std::vector<vk::Fence> fences;
      for (std::size_t i = 0; i < current->present_fences.size(); ++i)
        if (current->armed[i])
          fences.push_back(current->present_fences[i].nativeHandle());
      graphics::SwapchainRetirementUses uses;
      uses.present_fences = fences;
      uses.no_pending_present = fences.empty();
      if (retirement.retire(*device, current->swapchain, uses)) {
        ++paths.retired_via_queue;
        std::puts("example.textured_quad: retired generation=queue");
        retired.push_back(Retired{std::move(current->render_done),
                                  std::move(current->present_fences),
                                  std::move(current->depth_image), std::move(current->depth_view)});
        current.reset();
        return;
      }
    }
    if (use_present_fences && current->tainted)
      fallback_reason = "a present was out_of_date, its fence may never signal";
    ++paths.retired_via_queue_idle;
    std::printf("example.textured_quad: retired generation=queue-idle reason=%s\n",
                fallback_reason);
    vkQueueWaitIdle(native_queue);
    current.reset();
  };

  enum class Recreated : std::uint8_t { failed, deferred, done };
  // Recreate the Swapchain for the window's current framebuffer. `deferred`:
  // the window has no extent now (the caller waits for events and retries).
  const auto recreate = [&]() -> Recreated {
    const auto size = window->framebufferSize();
    if (!size)
      return Recreated::failed;
    graphics::SwapchainRequest request;
    request.extent = vk::Extent2D{size->width, size->height};
    auto result =
        graphics::recreateSwapchain(*instance, *device, *chosen, *surface, current->swapchain,
                                    graphics::PresentationRequest{}, request);
    if (!result.has_value()) {
      std::fprintf(stderr, "example.textured_quad: error: recreate the Swapchain: %s\n",
                   result.error().code().message().c_str());
      return Recreated::failed;
    }
    if (result->status == graphics::RecreationStatus::deferred_zero_extent)
      return Recreated::deferred;
    // The format may have changed: a pipeline is built for one format.
    const vk::Format format = result->swapchain->configuration()->format;
    if (format != pipeline_format) {
      vkQueueWaitIdle(native_queue);
      if (!build_pipeline(format))
        return Recreated::failed;
    }
    // The old generation goes away; the new one is ready to render.
    auto next = make_generation(std::move(*result->swapchain));
    if (!next)
      return Recreated::failed;
    drop_current();
    current = std::move(next);
    return Recreated::done;
  };

  // ---- The frame loop ----
  std::size_t presented = 0;
  std::size_t recreations = 0;
  bool resize_requested = false;
  std::size_t frames_since_resize_request = 0;
  std::size_t recreations_at_resize_request = 0;
  vk::Extent2D size_at_resize_request{};
  bool close_requested = false;
  bool needs_recreation = false;
  bool resize_event_after_request = false;

  // Events, from poll or wait alike: nothing the window reports is dropped.
  const auto handle_events = [&](const platform::EventBatch &batch) {
    for (const auto &event : batch.events) {
      if (std::holds_alternative<platform::CloseRequested>(event.data))
        close_requested = true;
      else if (std::holds_alternative<platform::FramebufferResized>(event.data)) {
        needs_recreation = true;
        if (resize_requested)
          resize_event_after_request = true;
      }
    }
  };
  // A suboptimal status only asks for a recreation when the Swapchain really has
  // another extent than the window (some systems report it permanently).
  const auto extent_differs = [&]() {
    const auto size = window->framebufferSize();
    const auto extent = current->swapchain.configuration()->extent;
    return size && (size->width != extent.width || size->height != extent.height);
  };
  // The scripted resize is over once it was seen (a recreation followed) or the
  // window system had enough frames to answer.
  const auto resize_pending = [&]() {
    return resize_requested && recreations == recreations_at_resize_request &&
           frames_since_resize_request < 60;
  };

  while (!close_requested) {
    // 1. Events: the application decides when and how to wait for them.
    const auto batch = runtime->pollEvents();
    if (!batch)
      return fail("poll events", batch);
    handle_events(*batch);
    if (close_requested || window->shouldClose().value_or(false))
      break;
    if (options.frames && presented >= *options.frames && !resize_pending())
      break;

    // Scripted resize: ask the window system once, at the requested frame.
    if (options.resize_at && !resize_requested && presented >= *options.resize_at) {
      resize_requested = true;
      recreations_at_resize_request = recreations;
      const auto before = window->framebufferSize();
      size_at_resize_request =
          vk::Extent2D{before ? before->width : 0, before ? before->height : 0};
      glfwSetWindowSize(window->nativeHandle(), 400, 300);
    }
    if (resize_requested)
      ++frames_since_resize_request;

    if (upload_retirement.pendingCount() > 0) {
      const auto collected_uploads = upload_retirement.collect();
      if (!collected_uploads)
        return fail("collect the retired upload staging memory", collected_uploads);
    }
    // Free what the retirement queue has let go of.
    if (retirement.pendingCount() > 0 || !retired.empty()) {
      const auto collected = retirement.collect(*device);
      if (!collected)
        return fail("collect the retired Swapchains", collected);
      if (retirement.pendingCount() == 0)
        retired.clear();
    }

    // 2. A window with no extent (minimized): do not render, wait for events.
    const auto size = window->framebufferSize();
    if (!size)
      return fail("framebuffer size", size);
    if (size->width == 0 || size->height == 0) {
      needs_recreation = true; // the size will differ when the window comes back
      ++paths.zero_size_waits;
      const auto waited = runtime->waitEvents(std::chrono::milliseconds(100));
      if (!waited)
        return fail("wait for events", waited);
      handle_events(*waited);
      continue;
    }
    if (needs_recreation) {
      const Recreated done = recreate();
      if (done == Recreated::failed)
        return fail("recreate the Swapchain");
      if (done == Recreated::deferred) {
        ++paths.deferred_zero_extent;
        const auto waited = runtime->waitEvents(std::chrono::milliseconds(100));
        if (!waited)
          return fail("wait for events", waited);
        handle_events(*waited);
        continue;
      }
      needs_recreation = false;
      ++recreations;
      ++paths.recreations;
    }

    // 3. The previous frame must be done before its objects are reused.
    if (!frame_fence->wait(UINT64_MAX))
      return fail("wait for the frame fence");

    // 4. Acquire. out_of_date acquired nothing: recreate and try again. The
    // timeout is explicit: a compositor that stops presenting must not hang the
    // example for ever, so the wait is bounded and a timeout is just a retry.
    graphics::AcquireDescription acquire;
    acquire.timeout_ns = 1'000'000'000;
    acquire.semaphore = acquire_semaphore->nativeHandle();
    const auto acquired = graphics::acquireNextImage(*device, current->swapchain, acquire);
    if (!acquired)
      return fail("acquire an image", acquired);
    if (!acquired->image) {
      if (acquired->status == graphics::AcquireStatus::out_of_date) {
        ++paths.acquire_out_of_date;
        needs_recreation = true;
      } else {
        ++paths.acquire_no_image;
      }
      continue; // out_of_date, timeout or not_ready: nothing was acquired
    }
    if (acquired->status == graphics::AcquireStatus::acquired_suboptimal) {
      ++paths.acquire_suboptimal;
      if (extent_differs())
        needs_recreation = true;
    }
    const graphics::SwapchainImage image = *acquired->image;
    const std::size_t index = image.index();

    // 4b. The previous frame is done (step 3), so the uniform Buffer may be rewritten: a
    // pulsing tint through the mapped pointer, then one explicit flush.
    const std::array<float, 4> tint{0.75F + 0.25F * std::sin(float(presented) * 0.1F), 1.0F, 1.0F,
                                    1.0F};
    std::memcpy(*mapped_uniform, tint.data(), sizeof(tint));
    if (!uniform_buffer->flush())
      return fail("flush the uniform Buffer");

    // 5. Record: barriers, rendering into the Swapchain image with a depth attachment,
    // indexed draw, barrier.
    if (!command->reset() || !command->begin())
      return fail("begin the command buffer");
    const std::array<VkImageMemoryBarrier2, 2> attachment_barriers{
        graphics::imageBarrier(image,
                               {.src_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dst_access = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .old_layout = vk::ImageLayout::eUndefined,
                                .new_layout = vk::ImageLayout::eColorAttachmentOptimal,
                                .subresource_range = color_range}),
        graphics::imageBarrier(*current->depth_image,
                               {.src_stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                .dst_stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                .dst_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                                .old_layout = vk::ImageLayout::eUndefined,
                                .new_layout = vk::ImageLayout::eDepthAttachmentOptimal,
                                .subresource_range = depth_range})};
    if (!graphics::recordBarriers(*device, *command, {.images = attachment_barriers}))
      return fail("record the barriers to the attachment layouts");

    graphics::RenderingDescription rendering;
    rendering.render_area = vk::Rect2D{{0, 0}, image.extent()};
    graphics::RenderingAttachmentDescription attachment;
    attachment.layout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.load_op = vk::AttachmentLoadOp::eClear;
    attachment.store_op = vk::AttachmentStoreOp::eStore;
    attachment.clear_value =
        vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{0.05F, 0.05F, 0.08F, 1.0F}}};
    attachment.swapchain = &current->swapchain;
    attachment.swapchain_image = &image;
    rendering.color_attachments = {attachment};
    graphics::RenderingAttachmentDescription depth_attachment;
    depth_attachment.image = &*current->depth_image;
    depth_attachment.view = &*current->depth_view;
    depth_attachment.layout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_attachment.load_op = vk::AttachmentLoadOp::eClear;
    depth_attachment.store_op = vk::AttachmentStoreOp::eDontCare;
    depth_attachment.clear_value = vk::ClearValue{vk::ClearDepthStencilValue{1.0F, 0}};
    rendering.depth_attachment = depth_attachment;
    auto scope = graphics::recordBeginRendering(*device, *command, rendering);
    if (!scope)
      return fail("begin rendering into the Swapchain image", scope);
    const vk::Rect2D area{{0, 0}, image.extent()};
    const std::array<const graphics::DescriptorSet *, 1> sets{&*descriptor_set};
    const std::array<graphics::VertexBufferBinding, 1> vertex_bindings{{{&*vertex_buffer, 0}}};
    graphics::GraphicsBindingState binding_state;
    if (!graphics::recordSetViewport(*device, *command,
                                     vk::Viewport{0.0F, 0.0F, float(area.extent.width),
                                                  float(area.extent.height), 0.0F, 1.0F}) ||
        !graphics::recordSetScissor(*device, *command, area) ||
        !graphics::recordBindPipeline(*device, *command, *pipeline, binding_state) ||
        !graphics::recordBindDescriptorSets(*device, *command, vk::PipelineBindPoint::eGraphics,
                                            *pipeline_layout, 0, sets, {}) ||
        !graphics::recordBindVertexBuffers(*device, *command, 0, vertex_bindings, binding_state) ||
        !graphics::recordBindIndexBuffer(*device, *command, *index_buffer, 0,
                                         vk::IndexType::eUint16, binding_state) ||
        !graphics::recordDrawIndexed(*device, *command, *scope, binding_state, 12, 1, 0, 0, 0) ||
        !graphics::recordEndRendering(*device, *command, std::move(*scope)))
      return fail("record the draw");

    if (!graphics::recordImageBarrier(
            *device, *command, image,
            {.src_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
             .src_access = vk::AccessFlagBits2::eColorAttachmentWrite,
             .old_layout = vk::ImageLayout::eColorAttachmentOptimal,
             .new_layout = vk::ImageLayout::ePresentSrcKHR,
             .subresource_range = color_range}) ||
        !command->end())
      return fail("record the barrier to the present layout");

    // 6. Submit: wait for the acquire semaphore, signal this image's semaphore.
    if (!frame_fence->reset())
      return fail("reset the frame fence");
    ++frame_serial;
    const std::array<const graphics::CommandBuffer *, 1> buffers{&*command};
    const std::array<graphics::SubmissionSemaphore, 1> waits{graphics::submissionWait(
        *acquire_semaphore, vk::PipelineStageFlagBits2::eColorAttachmentOutput, 0)};
    const std::array<graphics::SubmissionSemaphore, 1> signals{graphics::submissionSignal(
        current->render_done[index], vk::PipelineStageFlagBits2::eAllCommands, 0)};
    if (const auto receipt = graphics::submitFenced(
            *device, queue, *frame_fence, {&identity_domain, frame_serial},
            {.command_buffers = buffers, .waits = waits, .signals = signals});
        !receipt)
      return fail("submit the frame", receipt);

    // 7. Present. A present fence is reused for the same image index only after
    // that index was acquired again, which proves the earlier present finished.
    graphics::PresentDescription present;
    present.image = image;
    const std::array<vk::Semaphore, 1> present_waits{current->render_done[index].nativeHandle()};
    present.wait_semaphores = present_waits;
    if (use_present_fences) {
      if (current->armed[index]) {
        if (!current->present_fences[index].wait(UINT64_MAX) ||
            !current->present_fences[index].reset())
          return fail("reuse the present fence");
        current->armed[index] = false;
      }
      present.fence = current->present_fences[index].nativeHandle();
    }
    const auto result = graphics::presentImage(*device, queue, current->swapchain, present);
    if (!result)
      return fail("present", result);
    if (result->status == graphics::PresentStatus::out_of_date) {
      ++paths.present_out_of_date;
      current->tainted = true;
      needs_recreation = true;
    } else {
      if (use_present_fences)
        current->armed[index] = true;
      if (result->status == graphics::PresentStatus::presented_suboptimal)
        ++paths.present_suboptimal;
      if (result->recreationNeeded() && extent_differs())
        needs_recreation = true;
    }
    ++presented;
  }

  // ---- Shutdown: the armed present fences first (the evidence the retirement
  // path relies on), then the scope guard idles the Queue and drains the queue.
  if (current && use_present_fences)
    for (std::size_t i = 0; i < current->present_fences.size(); ++i)
      if (current->armed[i] && !current->present_fences[i].wait(UINT64_MAX))
        return fail("wait for a present fence at shutdown");

  outcome.presented = presented;
  int status = EXIT_SUCCESS;
  if (options.frames && presented < *options.frames) {
    std::fprintf(stderr, "example.textured_quad: error: presented %zu of %zu frames\n", presented,
                 *options.frames);
    status = EXIT_FAILURE;
  }
  if (options.resize_at && resize_requested) {
    const auto after = window->framebufferSize();
    // The window system accepted the resize if the size differs from the one at the
    // request or it reported a framebuffer-resized event after the request.
    const bool changed =
        resize_event_after_request || (after && (after->width != size_at_resize_request.width ||
                                                 after->height != size_at_resize_request.height));
    const std::size_t recreated = recreations - recreations_at_resize_request;
    if (changed && recreated == 0) {
      std::fprintf(stderr,
                   "example.textured_quad: error: the window was resized but the Swapchain was not "
                   "recreated\n");
      status = EXIT_FAILURE;
    }
    if (changed) {
      std::printf("example.textured_quad: resize=observed recreations-since-request=%zu\n",
                  recreated);
    } else if (options.require_resize) {
      // The window system did not resize the window (a tiling compositor, for
      // one): this run did not test a resize. run() reports it as SKIP, but only
      // when nothing else failed.
      outcome.resize_skipped = true;
    } else {
      std::printf("example.textured_quad: resize=refused-by-the-window-system\n");
    }
  }
  if (options.require_resize && !resize_requested) {
    std::fprintf(stderr,
                 "example.textured_quad: error: --require-resize but no resize was requested\n");
    status = EXIT_FAILURE;
  }
  // Counted where each path is taken. Shutdown always idles the Queue first (the
  // scope guard, which reports its own path), so it never exercises the retirement
  // queue on its own. A queue idle before a pipeline rebuild is not counted here.
  std::printf("example.textured_quad: paths acquire_out_of_date=%zu acquire_suboptimal=%zu "
              "acquire_no_image=%zu present_out_of_date=%zu present_suboptimal=%zu "
              "zero_size_waits=%zu deferred_zero_extent=%zu recreations=%zu "
              "retired_via_queue=%zu retired_via_queue_idle=%zu\n",
              paths.acquire_out_of_date, paths.acquire_suboptimal, paths.acquire_no_image,
              paths.present_out_of_date, paths.present_suboptimal, paths.zero_size_waits,
              paths.deferred_zero_extent, paths.recreations, paths.retired_via_queue,
              paths.retired_via_queue_idle);
  return status;
}

// Runs the application, lets every Terreate object go, and only then judges the
// validation messages, so those of the destruction count too (the sink is
// declared before anything that reports to it).
int run(const Options &options) {
  ValidationSink validation;
  Outcome outcome;
  int status = runApplication(options, validation, outcome);
  if (options.require_validation && validation.messages.load() != 0) {
    std::fprintf(stderr, "example.textured_quad: error: the validation layer reported messages\n");
    status = EXIT_FAILURE;
  }
  if (status == EXIT_SUCCESS && outcome.resize_skipped)
    std::puts(
        "example.textured_quad: resize=SKIP reason=the window system did not resize the window");
  else if (status == EXIT_SUCCESS)
    std::printf("example.textured_quad: frames=PASS presented=%zu\n", outcome.presented);
  return status;
}
} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: terreate_example_textured_quad [--frames N] [--resize-at K] "
                 "[--require-validation] [--require-resize]   (K must be smaller than N)\n");
    return EXIT_FAILURE;
  }
  // A scripted run without a window system is reported as SKIP, never PASS.
  // (With a display configured, a failure to use it is a real failure, as in the
  // native tests.)
#if !defined(_WIN32) && !defined(__APPLE__)
  const char *display = std::getenv("DISPLAY");
  const char *wayland = std::getenv("WAYLAND_DISPLAY");
  if (options.frames &&
      !((display != nullptr && *display != '\0') || (wayland != nullptr && *wayland != '\0'))) {
    std::puts("example.textured_quad: frames=SKIP reason=no DISPLAY or WAYLAND_DISPLAY");
    return EXIT_SUCCESS;
  }
#endif
  return run(options);
}
