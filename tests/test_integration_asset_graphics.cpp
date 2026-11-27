// #279 part 1: a MeshData reaches the GPU in three explicit steps (plan, create
// buffers, upload items), and the application does the rest: it prepares and
// records the upload batch, submits with a fence, waits for the fence, reads the
// buffers back through a ReadbackBatch and retires the staging buffers. The
// bytes read back must equal the plan's vertex and index bytes, and the
// validation layer must stay silent.
//
// GPU test on a headless Instance (no window system). If no device has
// synchronization2 the test reports SKIP, never PASS. The validation layer must
// be available: without it the test fails. No linker or platform mechanism is
// used.
#include <terreate/integration/asset_graphics/mesh_upload.hpp>

#include <terreate/asset/primitives.hpp>
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/command_pool.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/retirement.hpp>
#include <terreate/graphics/submission.hpp>
#include <terreate/graphics/synchronization.hpp>
#include <terreate/graphics/transfer_commands.hpp>
#include <terreate/graphics/upload_batch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace asset = terreate::asset;
namespace graphics = terreate::graphics;
namespace ag = terreate::integration::asset_graphics;

const char *test_name = "integration.asset_graphics";
int failures = 0;

bool check(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message.c_str());
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

int skip(const char *reason) {
  std::printf("%s: asset_graphics=SKIP reason=%s\n", test_name, reason);
  return EXIT_SUCCESS;
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

template <typename T, typename Code> bool hasError(const terreate::Result<T> &result, Code error) {
  return !result && result.error().code() == make_error_code(error);
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "integration.asset_graphics: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

const asset::AttributeKey position{asset::AttributeSemantic::position, 0, {}};
const asset::AttributeKey normal{asset::AttributeSemantic::normal, 0, {}};
const asset::AttributeKey texcoord{asset::AttributeSemantic::texcoord, 0, {}};

constexpr std::uint64_t wait_ns = 10'000'000'000;

// The application's side of the upload: everything the Integration leaves to it.
struct Gpu {
  graphics::Device &device;
  graphics::Allocator &allocator;
  graphics::Queue queue;
};

// Uploads `plan` into `buffers`, reads both buffers back and compares them with
// the plan's bytes. Returns false on a failed check.
bool roundTrip(Gpu &gpu, const ag::MeshUploadPlan &plan, const ag::MeshBuffers &buffers,
               const std::string &label) {
  auto items = ag::meshUploadItems(plan, buffers);
  if (!check(bool(items), label + ": meshUploadItems"))
    return false;
  check(items->size() == (plan.indexed() ? 2U : 1U) && (*items)[0].destination == &buffers.vertex &&
            (*items)[0].destination_offset == 0 &&
            (*items)[0].bytes.data() == plan.vertex_bytes.data() &&
            (*items)[0].bytes.size() == plan.vertex_bytes.size(),
        label + ": the vertex item borrows the buffer and the plan's bytes");
  if (plan.indexed() && buffers.index && items->size() == 2)
    check((*items)[1].destination == &*buffers.index &&
              (*items)[1].bytes.data() == plan.index_bytes.data() &&
              (*items)[1].bytes.size() == plan.index_bytes.size(),
          label + ": the index item borrows the buffer and the plan's bytes");

  auto uploaded = graphics::prepareUploadBatch(gpu.allocator,
                                               graphics::UploadBatchDescription{.buffers = *items});
  if (!check(bool(uploaded), label + ": prepareUploadBatch"))
    return false;

  std::vector<graphics::BufferReadbackItem> reads;
  reads.push_back({&buffers.vertex, 0, plan.vertex_bytes.size()});
  if (plan.indexed() && buffers.index)
    reads.push_back({&*buffers.index, 0, plan.index_bytes.size()});
  auto readback = graphics::prepareReadbackBatch(
      gpu.allocator, graphics::ReadbackBatchDescription{.buffers = reads});
  if (!check(bool(readback), label + ": prepareReadbackBatch"))
    return false;

  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(), label + ": begin CommandBuffer"))
    return false;
  if (!check(bool(graphics::recordUploadBatch(gpu.device, *command, *uploaded)),
             label + ": recordUploadBatch"))
    return false;

  // The application's own dependencies: upload writes, then readback reads, then
  // the host reads. Nothing here is hidden in the Integration.
  VkMemoryBarrier2 upload_to_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  upload_to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  upload_to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  upload_to_read.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  upload_to_read.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  if (!check(bool(graphics::recordBarriers(gpu.device, *command,
                                           {.memory = std::span{&upload_to_read, 1}})),
             label + ": upload -> readback barrier"))
    return false;
  if (!check(bool(graphics::recordReadbackBatch(gpu.device, *command, *readback)),
             label + ": recordReadbackBatch"))
    return false;
  VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  if (!check(graphics::recordBarriers(gpu.device, *command, {.memory = std::span{&to_host, 1}}) &&
                 command->end(),
             label + ": readback -> host barrier and end"))
    return false;

  auto fence = graphics::createFence(gpu.device, {});
  if (!check(bool(fence), label + ": Fence"))
    return false;
  int domain = 0;
  const std::array<const graphics::CommandBuffer *, 1> commands{&*command};
  auto receipt = graphics::submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1},
                                        {.command_buffers = commands});
  if (!receipt) {
    // The outcome of a failed submission is unknown: do not run destructors that
    // would free resources the GPU may still use.
    std::fprintf(stderr, "%s: submission failed\n", test_name);
    std::_Exit(blocked("real Queue submission failed"));
  }
  const auto waited = fence->wait(wait_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied) {
    std::_Exit(blocked("the Fence did not prove the GPU work complete"));
  }
  const std::array<graphics::SubmissionReceipt, 1> uses{*receipt};
  auto copied = graphics::tryCopyCompletedReadbackBatch(gpu.device, *readback, uses);
  if (!check(bool(copied), label + ": the completed readback is copied"))
    return false;

  const auto vertex = copied->dataFor(0);
  check(vertex.size() == plan.vertex_bytes.size() &&
            std::equal(vertex.begin(), vertex.end(), plan.vertex_bytes.begin()),
        label + ": the vertex buffer holds the plan's vertex bytes");
  if (plan.indexed()) {
    const auto index = copied->dataFor(1);
    check(index.size() == plan.index_bytes.size() &&
              std::equal(index.begin(), index.end(), plan.index_bytes.begin()),
          label + ": the index buffer holds the plan's index bytes");
  }

  // The application retires the staging buffers with the real receipt.
  graphics::RetirementQueue retirement;
  const bool retired = retirement.retire(*uploaded, uses) && retirement.retire(*readback, uses);
  check(retired && retirement.pendingCount() == 2, label + ": both staging owners are retired");
  const auto collected = retirement.collect();
  check(collected && collected->collected == 2 && retirement.requireEmpty(),
        label + ": the staging buffers are collected once the Fence is signaled");
  return failures == 0;
}

bool planMismatches(Gpu &gpu, const ag::MeshUploadPlan &indexed_plan) {
  // Plan errors that need an Allocator or Buffers.
  auto tampered = indexed_plan;
  tampered.vertex_buffer.size += 4;
  check(
      hasError(ag::createMeshBuffers(gpu.allocator, tampered), ag::MeshUploadError::plan_mismatch),
      "createMeshBuffers refuses a plan whose buffer size differs from its bytes");
  tampered = indexed_plan;
  tampered.index_buffer.reset();
  check(
      hasError(ag::createMeshBuffers(gpu.allocator, tampered), ag::MeshUploadError::plan_mismatch),
      "createMeshBuffers refuses index bytes without an index buffer");
  tampered = indexed_plan;
  tampered.vertex_buffer.usage = vk::BufferUsageFlagBits::eVertexBuffer;
  check(
      hasError(ag::createMeshBuffers(gpu.allocator, tampered), ag::MeshUploadError::plan_mismatch),
      "createMeshBuffers refuses a plan without TransferDst");

  auto buffers = ag::createMeshBuffers(gpu.allocator, indexed_plan);
  if (!check(bool(buffers), "buffers of the indexed plan"))
    return false;
  // A plan changed after the buffers were created no longer matches them.
  tampered = indexed_plan;
  tampered.index_bytes.resize(tampered.index_bytes.size() / 2);
  if (tampered.index_buffer)
    tampered.index_buffer->size = tampered.index_bytes.size();
  check(hasError(ag::meshUploadItems(tampered, *buffers), ag::MeshUploadError::plan_mismatch),
        "meshUploadItems refuses buffers of a different size");

  // Buffers without an index buffer do not match an indexed plan, and the other
  // way round.
  auto flat_plan = indexed_plan;
  flat_plan.index_bytes.clear();
  flat_plan.index_buffer.reset();
  flat_plan.index_count = 0;
  flat_plan.index_type = vk::IndexType::eNoneKHR;
  auto flat_buffers = ag::createMeshBuffers(gpu.allocator, flat_plan);
  if (!check(bool(flat_buffers) && !flat_buffers->index, "buffers of a non-indexed plan"))
    return false;
  check(hasError(ag::meshUploadItems(indexed_plan, *flat_buffers),
                 ag::MeshUploadError::plan_mismatch),
        "meshUploadItems refuses missing index buffer");
  check(hasError(ag::meshUploadItems(flat_plan, *buffers), ag::MeshUploadError::plan_mismatch),
        "meshUploadItems refuses an extra index buffer");

  // Nothing was submitted for these buffers, so they can be destroyed here.
  return failures == 0;
}

int exercise(const graphics::Instance &instance, const graphics::PhysicalDeviceCandidate &candidate,
             graphics::Device &device, ValidationRecorder &recorder) {
  const graphics::Queue queue = device.queue("main");
  if (!queue.valid())
    return blocked("graphics Queue");
  auto allocator = graphics::createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("Allocator");
  Gpu gpu{device, *allocator, queue};

  const auto upload_description = [](std::initializer_list<asset::AttributeKey> keys) {
    ag::MeshUploadDescription description;
    std::uint32_t location = 0;
    for (const auto &key : keys)
      description.attributes.push_back({key, location++});
    description.extra_vertex_usage = vk::BufferUsageFlagBits::eTransferSrc;
    description.extra_index_usage = vk::BufferUsageFlagBits::eTransferSrc;
    return description;
  };

  // Indexed with uint16 indices: a box.
  const auto box = terreate::unwrap(asset::box({}));
  const auto box_plan = ag::planMeshUpload(box, upload_description({position, normal, texcoord}));
  if (!check(bool(box_plan), "box plan"))
    return EXIT_FAILURE;
  {
    auto buffers = ag::createMeshBuffers(*allocator, *box_plan);
    if (!check(bool(buffers) && buffers->index.has_value(), "box buffers"))
      return EXIT_FAILURE;
    const auto observed = buffers->vertex.configuration();
    check(observed != nullptr && observed->requested.size == box_plan->vertex_buffer.size &&
              observed->requested.usage == box_plan->vertex_buffer.usage &&
              (observed->usage & vk::BufferUsageFlagBits::eVertexBuffer) &&
              (observed->usage & vk::BufferUsageFlagBits::eTransferDst),
          "the created vertex buffer is the planned one");
    if (!roundTrip(gpu, *box_plan, *buffers, "box"))
      return EXIT_FAILURE;
  }

  // Indexed with uint32 indices: a plane with more than 65536 vertices.
  {
    const auto plane = terreate::unwrap(asset::plane({.segmentsX = 256, .segmentsZ = 256}));
    const auto plan = ag::planMeshUpload(plane, upload_description({position, texcoord}));
    if (!check(bool(plan) && plan->index_type == vk::IndexType::eUint32, "plane plan"))
      return EXIT_FAILURE;
    auto buffers = ag::createMeshBuffers(*allocator, *plan);
    if (!check(bool(buffers), "plane buffers"))
      return EXIT_FAILURE;
    if (!roundTrip(gpu, *plan, *buffers, "plane"))
      return EXIT_FAILURE;
  }

  // Not indexed: the box vertices drawn in order (24 vertices, a triangle list).
  {
    asset::MeshData flat = box;
    flat.indices = std::monostate{};
    flat.bounds.reset();
    const auto plan = ag::planMeshUpload(flat, upload_description({position, normal, texcoord}));
    if (!check(bool(plan) && !plan->indexed(), "non-indexed plan"))
      return EXIT_FAILURE;
    auto buffers = ag::createMeshBuffers(*allocator, *plan);
    if (!check(bool(buffers) && !buffers->index, "non-indexed buffers"))
      return EXIT_FAILURE;
    if (!roundTrip(gpu, *plan, *buffers, "non-indexed"))
      return EXIT_FAILURE;
  }

  if (!planMismatches(gpu, *box_plan))
    return EXIT_FAILURE;

  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int gpuTest() {
  const auto capabilities = graphics::queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");
  graphics::InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-asset-graphics";
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = graphics::DebugUtilsMode::required;
  const auto plan = graphics::resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance");
  ValidationRecorder recorder;
  auto instance = graphics::createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance");
  auto inventory = graphics::queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");

  for (const auto &candidate : *inventory) {
    graphics::DeviceDescription request;
    graphics::DeviceQueueRequest queue_request;
    queue_request.caller_id = "main";
    queue_request.strength = graphics::RequirementStrength::required;
    queue_request.required_flags = vk::QueueFlagBits::eGraphics;
    request.queue_requests = {queue_request};
    request.required_features_13.synchronization2 = VK_TRUE;
    const auto device_plan = graphics::resolveDevice(candidate, request);
    if (!device_plan)
      continue;
    int status = EXIT_FAILURE;
    {
      auto device = graphics::createDevice(*instance, *device_plan);
      if (!device)
        return blocked("Device with synchronization2");
      status = exercise(*instance, candidate, *device, recorder);
    }
    if (status != EXIT_SUCCESS)
      return status;
    check(recorder.warnings_or_errors.load() == 0,
          "the validation layer reports nothing after destruction");
    if (failures != 0)
      return EXIT_FAILURE;
    std::puts("integration.asset_graphics: asset_graphics=PASS validation-layer=PASS");
    return EXIT_SUCCESS;
  }
  return skip("no device with a graphics queue and synchronization2");
}

} // namespace

int main() { return gpuTest(); }
