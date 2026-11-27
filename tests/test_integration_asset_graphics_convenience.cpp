// #463: the beginner mesh upload convenience on a real device.
//
// (a) submitMeshUpload does not wait: the test waits on the fence itself, reads
//     the buffers back and compares them with the buffers of the explicit
//     three-step path (same bytes, and the plan's bytes).
// (b) uploadMeshBlocking returns buffers that already hold the mesh: read back
//     and compare in the same way.
// (c) the staging owner of a submission is retired through a RetirementQueue
//     with the submission's receipt and collected once the fence has signaled.
//
// GPU test on a headless Instance (no window system). If no device has
// synchronization2 the test reports SKIP, never PASS. The validation layer must
// be available: without it the test fails and it must stay silent. No linker or
// platform mechanism is used.
#include <terreate/integration/asset_graphics/mesh_upload.hpp>
#include <terreate/integration/asset_graphics/mesh_upload_convenience.hpp>

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

const char *test_name = "integration.asset_graphics.convenience";
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
  std::printf("%s: convenience=SKIP reason=%s\n", test_name, reason);
  return EXIT_SUCCESS;
}

bool hasName(const std::vector<std::string> &names, std::string_view wanted) {
  return std::ranges::any_of(names, [&](const auto &name) { return name == wanted; });
}

struct ValidationRecorder {
  std::atomic<std::size_t> warnings_or_errors{0};
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    if (event.severity != terreate::DiagnosticSeverity::warning &&
        event.severity != terreate::DiagnosticSeverity::error)
      return;
    for (const auto &category : event.categories) {
      if (category == "VALIDATION") {
        std::fprintf(stderr, "%s: validation message: %s\n", test_name, event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

const asset::AttributeKey position{asset::AttributeSemantic::position, 0, {}};
const asset::AttributeKey texcoord{asset::AttributeSemantic::texcoord, 0, {}};

constexpr std::uint64_t wait_ns = 10'000'000'000;

struct Gpu {
  graphics::Device &device;
  graphics::Allocator &allocator;
  graphics::Queue queue;
};

struct Bytes {
  std::vector<std::byte> vertex;
  std::vector<std::byte> index;
};

// Reads the buffers back with the explicit Graphics API: a ReadbackBatch in its
// own command buffer and fence, waited on by the test. The buffers must have
// been created with eTransferSrc and their upload must be complete.
bool readBack(Gpu &gpu, const ag::MeshBuffers &buffers, const ag::MeshUploadPlan &plan, Bytes &out,
              const std::string &label) {
  std::vector<graphics::BufferReadbackItem> reads;
  reads.push_back({&buffers.vertex, 0, plan.vertex_bytes.size()});
  if (buffers.index)
    reads.push_back({&*buffers.index, 0, plan.index_bytes.size()});
  auto readback = graphics::prepareReadbackBatch(
      gpu.allocator, graphics::ReadbackBatchDescription{.buffers = reads});
  if (!check(bool(readback), label + ": prepareReadbackBatch"))
    return false;
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": readback CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(), label + ": begin readback CommandBuffer"))
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
  if (!check(bool(fence), label + ": readback Fence"))
    return false;
  static constexpr int domain = 0;
  const std::array<const graphics::CommandBuffer *, 1> commands{&*command};
  auto receipt = graphics::submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1},
                                        {.command_buffers = commands});
  if (!receipt)
    std::_Exit(blocked("readback submission failed"));
  const auto waited = fence->wait(wait_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied)
    std::_Exit(blocked("the readback Fence did not prove the GPU work complete"));
  const std::array<graphics::SubmissionReceipt, 1> uses{*receipt};
  auto copied = graphics::tryCopyCompletedReadbackBatch(gpu.device, *readback, uses);
  if (!check(bool(copied), label + ": the completed readback is copied"))
    return false;
  const auto vertex = copied->dataFor(0);
  out.vertex.assign(vertex.begin(), vertex.end());
  out.index.clear();
  if (buffers.index) {
    const auto index = copied->dataFor(1);
    out.index.assign(index.begin(), index.end());
  }
  graphics::RetirementQueue retirement;
  const bool retired = static_cast<bool>(retirement.retire(*readback, uses));
  const auto collected = retirement.collect();
  check(retired && collected && collected->collected == 1 && retirement.requireEmpty(),
        label + ": the readback staging is retired");
  return true;
}

// The explicit three-step path, written out as the documentation says, with the
// application's own command buffer, fence, wait and retirement.
bool explicitUpload(Gpu &gpu, const asset::MeshData &mesh,
                    const ag::MeshUploadDescription &description, Bytes &out,
                    const std::string &label) {
  auto plan = ag::planMeshUpload(mesh, description);
  if (!check(bool(plan), label + ": explicit plan"))
    return false;
  auto buffers = ag::createMeshBuffers(gpu.allocator, *plan);
  if (!check(bool(buffers), label + ": explicit buffers"))
    return false;
  auto items = ag::meshUploadItems(*plan, *buffers);
  if (!check(bool(items), label + ": explicit items"))
    return false;
  auto staging = graphics::prepareUploadBatch(gpu.allocator,
                                              graphics::UploadBatchDescription{.buffers = *items});
  if (!check(bool(staging), label + ": explicit prepareUploadBatch"))
    return false;
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": explicit CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit) &&
                 graphics::recordUploadBatch(gpu.device, *command, *staging) && command->end(),
             label + ": explicit recording"))
    return false;
  auto fence = graphics::createFence(gpu.device, {});
  if (!check(bool(fence), label + ": explicit Fence"))
    return false;
  static constexpr int domain = 0;
  const std::array<const graphics::CommandBuffer *, 1> commands{&*command};
  auto receipt = graphics::submitFenced(gpu.device, gpu.queue, *fence, {&domain, 1},
                                        {.command_buffers = commands});
  if (!receipt)
    std::_Exit(blocked("explicit submission failed"));
  const auto waited = fence->wait(wait_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied)
    std::_Exit(blocked("the explicit Fence did not prove the GPU work complete"));
  const std::array<graphics::SubmissionReceipt, 1> uses{*receipt};
  graphics::RetirementQueue retirement;
  check(retirement.retire(*staging, uses) && retirement.collect() && retirement.requireEmpty(),
        label + ": explicit staging retired");
  return readBack(gpu, *buffers, *plan, out, label + " explicit");
}

bool sameBytes(const Bytes &a, const Bytes &b) {
  return a.vertex == b.vertex && a.index == b.index;
}

// (a) + (c): submitMeshUpload, the test's own wait, retirement and read back.
bool submitPath(Gpu &gpu, const asset::MeshData &mesh, const ag::MeshUploadDescription &description,
                const Bytes &explicit_bytes, bool expect_default, const std::string &label) {
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(gpu.queue.familyIndex());
  auto pool = graphics::createCommandPool(gpu.device, pool_description);
  if (!check(bool(pool), label + ": CommandPool"))
    return false;
  auto command = pool->allocatePrimary();
  if (!check(command && command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit),
             label + ": begin"))
    return false;
  auto fence = graphics::createFence(gpu.device, {});
  if (!check(bool(fence), label + ": Fence"))
    return false;
  static constexpr int domain = 0;
  auto submission = ag::submitMeshUpload(gpu.device, gpu.allocator, gpu.queue, *command, *fence,
                                         {&domain, 1}, mesh, description);
  if (!check(bool(submission), label + ": submitMeshUpload")) {
    if (!submission)
      std::fprintf(stderr, "%s: %s\n", test_name, submission.error().code().message().c_str());
    // Nothing was submitted for a plan, buffer or recording error; a submit
    // error has an unknown outcome and the objects are not released safely.
    return false;
  }
  check(submission->plan.used_default_attributes == expect_default,
        label + ": the plan reports which mapping was used");
  check(command->isExecutable(), label + ": the command buffer was ended");

  // (c) The staging owner goes to the RetirementQueue with the receipt; the test
  // itself waits on the fence (submitMeshUpload never does).
  const std::array<graphics::SubmissionReceipt, 1> uses{submission->receipt};
  graphics::RetirementQueue retirement;
  if (!check(retirement.retire(submission->staging, uses) && retirement.pendingCount() == 1,
             label + ": the staging owner is retired with the receipt"))
    return false;
  const auto waited = fence->wait(wait_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied)
    std::_Exit(blocked("the Fence did not prove the GPU work complete"));
  const auto collected = retirement.collect();
  check(collected && collected->collected == 1 && retirement.requireEmpty(),
        label + ": the staging is collected only once the Fence has signaled");

  Bytes bytes;
  if (!readBack(gpu, submission->buffers, submission->plan, bytes, label))
    return false;
  check(bytes.vertex == submission->plan.vertex_bytes &&
            bytes.index == submission->plan.index_bytes,
        label + ": the buffers hold the plan's bytes");
  check(sameBytes(bytes, explicit_bytes), label + ": same bytes as the explicit three steps");
  return true;
}

// (b) uploadMeshBlocking: the buffers are complete on return.
bool blockingPath(Gpu &gpu, const asset::MeshData &mesh,
                  const ag::MeshUploadDescription &description, const Bytes &explicit_bytes,
                  const std::string &label) {
  auto result =
      ag::uploadMeshBlocking(gpu.device, gpu.allocator, gpu.queue, mesh, wait_ns, description);
  if (!check(bool(result), label + ": uploadMeshBlocking")) {
    if (!result)
      std::fprintf(stderr, "%s: %s\n", test_name, result.error().code().message().c_str());
    return false;
  }
  check(result->plan.used_default_attributes == description.attributes.empty(),
        label + ": the plan reports which mapping was used");
  check(result->buffers.index.has_value() == result->plan.indexed(),
        label + ": an index buffer exactly when indexed");
  Bytes bytes;
  if (!readBack(gpu, result->buffers, result->plan, bytes, label))
    return false;
  check(bytes.vertex == result->plan.vertex_bytes && bytes.index == result->plan.index_bytes,
        label + ": the buffers hold the plan's bytes");
  check(sameBytes(bytes, explicit_bytes), label + ": same bytes as the explicit three steps");
  return true;
}

bool errorsPass(Gpu &gpu) {
  // An invalid mesh is the Asset error, unchanged, and nothing is submitted.
  auto broken = terreate::unwrap(asset::box({}));
  broken.attributes.clear();
  auto result = ag::uploadMeshBlocking(gpu.device, gpu.allocator, gpu.queue, broken, wait_ns);
  check(!result && result.error().code() == make_error_code(asset::MeshDataError::missing_position),
        "uploadMeshBlocking returns the asset error unchanged");
  return true;
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

  // Read-back usage on every buffer. An empty `attributes` is the default mapping.
  const auto with_readback = [](ag::MeshUploadDescription description) {
    description.extra_vertex_usage = vk::BufferUsageFlagBits::eTransferSrc;
    description.extra_index_usage = vk::BufferUsageFlagBits::eTransferSrc;
    return description;
  };
  const auto defaults = with_readback({});
  ag::MeshUploadDescription given_description;
  given_description.attributes = {{position, 3}, {texcoord, 0}};
  const auto given = with_readback(given_description);

  struct Case {
    std::string label;
    asset::MeshData mesh;
    ag::MeshUploadDescription description;
  };
  std::vector<Case> cases;
  const auto box = terreate::unwrap(asset::box({}));
  cases.push_back({"box default", box, defaults});
  cases.push_back({"box given", box, given});
  cases.push_back({"plane uint32 default",
                   terreate::unwrap(asset::plane({.segmentsX = 256, .segmentsZ = 256})), defaults});
  asset::MeshData flat = box;
  flat.indices = std::monostate{};
  flat.bounds.reset();
  cases.push_back({"non-indexed default", flat, defaults});

  for (const auto &c : cases) {
    // The explicit path uses the same mapping, spelled out as the application would.
    ag::MeshUploadDescription spelled = c.description;
    if (spelled.attributes.empty())
      spelled.attributes = ag::defaultMeshAttributeBindings(c.mesh);
    Bytes explicit_bytes;
    if (!explicitUpload(gpu, c.mesh, spelled, explicit_bytes, c.label))
      return EXIT_FAILURE;
    if (!submitPath(gpu, c.mesh, c.description, explicit_bytes, c.description.attributes.empty(),
                    c.label + " submit"))
      return EXIT_FAILURE;
    if (!blockingPath(gpu, c.mesh, c.description, explicit_bytes, c.label + " blocking"))
      return EXIT_FAILURE;
  }
  errorsPass(gpu);

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
  description.application_name = "terreate-asset-graphics-convenience";
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
    std::puts("integration.asset_graphics.convenience: convenience=PASS validation-layer=PASS");
    return EXIT_SUCCESS;
  }
  return skip("no device with a graphics queue and synchronization2");
}

} // namespace

int main() { return gpuTest(); }
