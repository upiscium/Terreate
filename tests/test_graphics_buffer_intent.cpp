// Buffer memory intents: each intent gives exactly its allocation fields, the upload and readback
// defaults are defined through them, the preset is a plain value that can be edited, and buffers
// made from the intents are host visible and map (or are created, for device_local).
//
// `cpu` needs no device. `gpu` runs on a headless Instance with a software or real device and
// requires the validation layer: without it the test fails, never passes. No linker or platform
// mechanism is used.
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/readback_batch.hpp>
#include <terreate/graphics/upload_batch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.buffer_intent";
int failures = 0;

bool check(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "%s: assertion=FAIL: %s\n", test_name, message);
  }
  return condition;
}

int blocked(const char *message) {
  std::fprintf(stderr, "%s: validation=BLOCKED: %s\n", test_name, message);
  std::fflush(stderr);
  return EXIT_FAILURE;
}

bool same(const BufferAllocationDescription &a, const BufferAllocationDescription &b) {
  return a.usage == b.usage && a.flags == b.flags && a.required_flags == b.required_flags &&
         a.preferred_flags == b.preferred_flags && a.memory_type_bits == b.memory_type_bits;
}

int cpuTest() {
  const auto device_local = bufferAllocationFor(BufferMemoryIntent::device_local);
  check(device_local.usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE && device_local.flags == 0 &&
            !device_local.required_flags && !device_local.preferred_flags &&
            device_local.memory_type_bits == 0,
        "device_local: AUTO_PREFER_DEVICE and nothing else");
  const auto sequential = bufferAllocationFor(BufferMemoryIntent::host_sequential_write);
  check(sequential.usage == VMA_MEMORY_USAGE_AUTO &&
            sequential.flags == VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT &&
            sequential.required_flags == vk::MemoryPropertyFlagBits::eHostVisible &&
            !sequential.preferred_flags && sequential.memory_type_bits == 0,
        "host_sequential_write: AUTO, the sequential-write flag, required HostVisible");
  const auto random = bufferAllocationFor(BufferMemoryIntent::host_random_access);
  check(random.usage == VMA_MEMORY_USAGE_AUTO &&
            random.flags == VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT &&
            random.required_flags == vk::MemoryPropertyFlagBits::eHostVisible &&
            !random.preferred_flags && random.memory_type_bits == 0,
        "host_random_access: AUTO, the random-access flag, required HostVisible");

  // The two existing defaults are the same rule, written once.
  check(same(defaultUploadStagingAllocation(), sequential),
        "the upload staging default is host_sequential_write");
  check(same(defaultReadbackStagingAllocation(), random),
        "the readback staging default is host_random_access");

  // A plain value: editing one copy does not change the next call.
  auto edited = bufferAllocationFor(BufferMemoryIntent::host_sequential_write);
  edited.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  edited.preferred_flags = vk::MemoryPropertyFlagBits::eHostCached;
  check(same(bufferAllocationFor(BufferMemoryIntent::host_sequential_write), sequential) &&
            !same(edited, sequential),
        "the preset is a plain value: an edited copy does not leak into the next call");
  if (failures == 0)
    std::puts("graphics.buffer_intent.cpu: cpu=PASS");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
        std::fprintf(stderr, "graphics.buffer_intent: validation message: %s\n",
                     event.message.c_str());
        warnings_or_errors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
};

int exercise(const Instance &instance, const PhysicalDeviceCandidate &candidate, Device &device,
             ValidationRecorder &recorder) {
  auto allocator = createAllocator(instance, candidate, device);
  if (!allocator)
    return blocked("Allocator");

  const auto make = [&](BufferMemoryIntent intent, vk::BufferUsageFlags usage) {
    BufferDescription description;
    description.size = 256;
    description.usage = usage;
    description.allocation = bufferAllocationFor(intent);
    return createBuffer(*allocator, description);
  };

  // The host intents give host-visible memory, which maps and can be written.
  auto sequential =
      make(BufferMemoryIntent::host_sequential_write, vk::BufferUsageFlagBits::eUniformBuffer);
  if (check(bool(sequential), "a host_sequential_write buffer is created")) {
    const auto info = sequential->allocationInfo();
    check(info && (info->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible),
          "its memory is host visible");
    const auto mapped = sequential->map();
    if (check(bool(mapped), "it maps")) {
      std::memset(*mapped, 0x5A, 256);
      check(bool(sequential->flush()), "a written range can be flushed");
    }
  }
  auto random = make(BufferMemoryIntent::host_random_access, vk::BufferUsageFlagBits::eTransferDst);
  if (check(bool(random), "a host_random_access buffer is created")) {
    const auto info = random->allocationInfo();
    check(info && (info->memory_properties & vk::MemoryPropertyFlagBits::eHostVisible),
          "its memory is host visible");
    const auto mapped = random->map();
    check(bool(mapped), "it maps");
    if (mapped)
      check(bool(random->invalidate()), "it can be invalidated before the host reads it");
  }

  // device_local: created with transfer-destination and vertex usage, and its allocation is
  // observable. No claim about host visibility, and no transfer happens here: the textured quad
  // example uploads into device_local buffers under the gate.
  auto local = make(BufferMemoryIntent::device_local,
                    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst);
  if (check(bool(local), "a device_local buffer is created")) {
    const auto info = local->allocationInfo();
    check(bool(info), "its allocation can be observed");
  }

  // An edited preset is still an ordinary description.
  BufferDescription edited;
  edited.size = 64;
  edited.usage = vk::BufferUsageFlagBits::eUniformBuffer;
  edited.allocation = bufferAllocationFor(BufferMemoryIntent::host_sequential_write);
  edited.allocation.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  check(bool(createBuffer(*allocator, edited)), "a preset with an added raw VMA flag is accepted");

  check(recorder.warnings_or_errors.load() == 0, "the validation layer reports nothing");
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int gpuTest() {
  const auto capabilities = queryInstanceCapabilities();
  if (!capabilities)
    return blocked("Vulkan loader capability query");
  if (!hasName(capabilities->available_layers, "VK_LAYER_KHRONOS_validation") ||
      !hasName(capabilities->available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    return blocked("the validation layer and VK_EXT_debug_utils are required");
  InstanceDescription description;
  description.api_version = VK_API_VERSION_1_3;
  description.application_name = "terreate-buffer-intent";
  description.required_layers = {"VK_LAYER_KHRONOS_validation"};
  description.debug_utils = DebugUtilsMode::required;
  const auto plan = resolveInstance(description, *capabilities);
  if (!plan)
    return blocked("Vulkan 1.3 Instance");
  ValidationRecorder recorder;
  auto instance = createInstance(*plan, terreate::DiagnosticSinkView::bind(recorder));
  if (!instance)
    return blocked("Vulkan Instance");
  auto inventory = queryPhysicalDevices(*instance);
  if (!inventory)
    return blocked("physical device enumeration");
  for (const auto &candidate : *inventory) {
    DeviceDescription request;
    DeviceQueueRequest queue_request;
    queue_request.caller_id = "main";
    queue_request.required_flags = vk::QueueFlagBits::eGraphics;
    request.queue_requests = {queue_request};
    const auto device_plan = resolveDevice(candidate, request);
    if (!device_plan)
      continue;
    int status = EXIT_FAILURE;
    {
      auto device = createDevice(*instance, *device_plan);
      if (!device)
        return blocked("Device");
      status = exercise(*instance, candidate, *device, recorder);
    }
    if (status != EXIT_SUCCESS)
      return status;
    check(recorder.warnings_or_errors.load() == 0,
          "the validation layer reports nothing after destruction");
    if (failures == 0)
      std::puts("graphics.buffer_intent: buffer_intent=PASS validation-layer=PASS");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  return blocked("no device with a graphics queue");
}

} // namespace

int main(int argc, char **argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "gpu";
  if (mode == "cpu")
    return cpuTest();
  if (mode == "gpu")
    return gpuTest();
  std::fprintf(stderr, "usage: terreate_graphics_buffer_intent_tests [cpu|gpu]\n");
  return EXIT_FAILURE;
}
