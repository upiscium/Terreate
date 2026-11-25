// Vertex layouts built from the application's own struct: stride, offsets (padding and mixed
// member types), explicit formats, the named validation errors, a second binding, applyTo and the
// typed uploadItem.
//
// `cpu` needs no device. `gpu` runs on a headless Instance with a software or real device and
// requires the validation layer: without it the test fails, never passes. No linker or platform
// mechanism is used.
#include <terreate/graphics/allocator.hpp>
#include <terreate/graphics/buffer.hpp>
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/graphics_pipeline.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <terreate/graphics/upload_batch.hpp>
#include <terreate/graphics/vertex_layout.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace terreate::graphics;

const char *test_name = "graphics.vertex_layout";
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

template <typename T> bool hasError(const terreate::Result<T> &result, VertexLayoutError error) {
  return !result && result.error().code() == make_error_code(error);
}

struct Vertex {
  std::array<float, 3> position;
  std::array<float, 2> uv;
};

// Padding: a byte, then a type that needs 4-byte alignment.
struct Padded {
  std::uint8_t tag;
  std::array<float, 2> value;
};

struct Colored {
  std::array<float, 3> position;
  std::array<std::uint8_t, 4> color;
  std::uint32_t id;
};

struct PerInstance {
  std::array<float, 2> offset;
};

constexpr auto vec3 = vk::Format::eR32G32B32Sfloat;
constexpr auto vec2 = vk::Format::eR32G32Sfloat;

int cpuTest() {
  // Stride, offsets, formats, binding 0 at per-vertex rate.
  const auto layout = vertexLayout(vertexAttribute<&Vertex::position>(0, vec3),
                                   vertexAttribute<&Vertex::uv>(1, vec2));
  if (check(bool(layout), "a valid struct layout is accepted")) {
    const auto bindings = layout->bindings();
    const auto attributes = layout->attributes();
    check(bindings.size() == 1 && bindings[0].binding == 0 &&
              bindings[0].stride == sizeof(Vertex) &&
              bindings[0].inputRate == vk::VertexInputRate::eVertex,
          "binding 0, per-vertex, stride sizeof(Vertex)");
    check(attributes.size() == 2 && attributes[0].location == 0 && attributes[0].binding == 0 &&
              attributes[0].format == vec3 && attributes[0].offset == offsetof(Vertex, position) &&
              attributes[1].location == 1 && attributes[1].format == vec2 &&
              attributes[1].offset == offsetof(Vertex, uv),
          "attributes carry the stated formats and the measured offsets");
  }

  // Padding and mixed member types: the offset is the compiler's, not a sum of sizes.
  const auto padded = vertexLayout(vertexAttribute<&Padded::value>(0, vec2));
  check(padded && padded->attributes()[0].offset == offsetof(Padded, value) &&
            padded->bindings()[0].stride == sizeof(Padded),
        "an attribute after padding gets the compiler's offset, the stride includes the padding");
  const auto colored = vertexLayout(vertexAttribute<&Colored::position>(0, vec3),
                                    vertexAttribute<&Colored::color>(1, vk::Format::eR8G8B8A8Unorm),
                                    vertexAttribute<&Colored::id>(2, vk::Format::eR32Uint));
  check(colored && colored->attributes()[1].offset == offsetof(Colored, color) &&
            colored->attributes()[2].offset == offsetof(Colored, id) &&
            colored->attributes()[1].format == vk::Format::eR8G8B8A8Unorm,
        "mixed member types: the format is the one the caller stated");

  // A binding number and a rate other than the defaults.
  const auto instance = vertexLayout(VertexBindingSpec{3, vk::VertexInputRate::eInstance},
                                     vertexAttribute<&PerInstance::offset>(5, vec2));
  check(instance && instance->bindings()[0].binding == 3 &&
            instance->bindings()[0].inputRate == vk::VertexInputRate::eInstance &&
            instance->attributes()[0].binding == 3 && instance->attributes()[0].location == 5,
        "an explicit binding number and rate are used");

  // The named validation errors.
  check(hasError(makeVertexLayout(sizeof(Vertex), {}, {}), VertexLayoutError::no_attributes),
        "no attributes is refused");
  check(hasError(vertexLayout(vertexAttribute<&Vertex::uv>(0, vec3)),
                 VertexLayoutError::format_size_mismatch),
        "a format whose block size differs from the member is refused");
  check(hasError(vertexLayout(vertexAttribute<&Vertex::uv>(0, vk::Format::eUndefined)),
                 VertexLayoutError::unsupported_format),
        "an undefined format is refused");
  check(hasError(vertexLayout(vertexAttribute<&Vertex::uv>(0, vk::Format::eBc1RgbUnormBlock)),
                 VertexLayoutError::unsupported_format),
        "a compressed format is refused");
  check(hasError(vertexLayout(vertexAttribute<&Vertex::position>(0, vec3),
                              vertexAttribute<&Vertex::uv>(0, vec2)),
                 VertexLayoutError::duplicate_location),
        "a repeated location is refused");
  check(hasError(vertexLayout(vertexAttribute<&Vertex::uv>(0, vec2),
                              vertexAttribute<&Vertex::uv>(1, vec2)),
                 VertexLayoutError::attributes_overlap),
        "two attributes on the same bytes are refused");

  // combine: a second binding joins; a repeated binding or location is refused.
  if (layout && instance) {
    const auto both = combine(*layout, *instance);
    check(both && both->bindings().size() == 2 && both->attributes().size() == 3 &&
              both->bindings()[1].binding == 3 && both->attributes()[2].location == 5,
          "combine appends the second binding and its attributes");
    check(hasError(combine(*layout, *layout), VertexLayoutError::duplicate_binding),
          "combining a binding with itself is refused");
    const auto clash = vertexLayout(VertexBindingSpec{1, vk::VertexInputRate::eInstance},
                                    vertexAttribute<&PerInstance::offset>(1, vec2));
    check(clash && hasError(combine(*layout, *clash), VertexLayoutError::duplicate_location),
          "combining layouts that share a location is refused");
  }

  // applyTo fills exactly the native structures the pipeline description takes.
  if (layout) {
    GraphicsPipelineDescription description;
    description.vertex_bindings = {
        vk::VertexInputBindingDescription{9, 1, vk::VertexInputRate::eVertex}};
    layout->applyTo(description);
    const std::vector<vk::VertexInputBindingDescription> expected_bindings{
        {0, sizeof(Vertex), vk::VertexInputRate::eVertex}};
    const std::vector<vk::VertexInputAttributeDescription> expected_attributes{
        {0, 0, vec3, static_cast<std::uint32_t>(offsetof(Vertex, position))},
        {1, 0, vec2, static_cast<std::uint32_t>(offsetof(Vertex, uv))}};
    check(description.vertex_bindings == expected_bindings &&
              description.vertex_attributes == expected_attributes,
          "applyTo replaces the bindings and attributes with the layout's");
  }

  // The data and the layout come from one type: the typed span's size is what the layout strides.
  constexpr std::array<Vertex, 4> vertices{};
  check(layout && std::as_bytes(std::span{vertices}).size() == 4 * layout->bindings()[0].stride,
        "typed vertex data and the layout's stride agree");

  if (failures == 0)
    std::puts("graphics.vertex_layout.cpu: cpu=PASS");
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
        std::fprintf(stderr, "graphics.vertex_layout: validation message: %s\n",
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
  constexpr std::array<Vertex, 3> vertices{};
  BufferDescription description;
  description.size = sizeof(vertices);
  description.usage =
      vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
  auto buffer = createBuffer(*allocator, description);
  if (!buffer)
    return blocked("vertex Buffer");
  const BufferUploadItem item = uploadItem(*buffer, std::span<const Vertex>{vertices}, 16);
  check(item.destination == &*buffer && item.destination_offset == 16 &&
            item.bytes.data() == reinterpret_cast<const std::byte *>(vertices.data()) &&
            item.bytes.size() == sizeof(vertices),
        "uploadItem maps the destination, the offset and the bytes of a typed span");
  const BufferUploadItem item_default = uploadItem(*buffer, std::span<const Vertex>{vertices});
  check(item_default.destination_offset == 0, "the destination offset defaults to 0");
  // Any contiguous range, with its fixed or dynamic extent, not only a dynamic span.
  const std::vector<Vertex> dynamic(2);
  const BufferUploadItem from_array = uploadItem(*buffer, vertices);
  const BufferUploadItem from_vector = uploadItem(*buffer, dynamic);
  check(from_array.bytes.size() == sizeof(vertices) &&
            from_vector.bytes.size() == 2 * sizeof(Vertex) &&
            from_vector.bytes.data() == reinterpret_cast<const std::byte *>(dynamic.data()),
        "uploadItem takes a std::array and a std::vector directly");
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
  description.application_name = "terreate-vertex-layout";
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
      std::puts("graphics.vertex_layout: vertex_layout=PASS validation-layer=PASS");
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
  std::fprintf(stderr, "usage: terreate_graphics_vertex_layout_tests [cpu|gpu]\n");
  return EXIT_FAILURE;
}
