// Getting started: bring Vulkan up through Terreate, headless, and look at what
// the library actually gave you.
//
// The order is always the same, and every step is a call this file makes:
//   1. ask the loader what exists (capabilities),
//   2. resolve a request against it (a plan: requested, supported and effective
//      state are all visible before anything is created),
//   3. create from the plan (an owning, move-only value),
//   4. repeat for the physical device and the logical Device.
// Nothing runs a loop, waits or retries for you; the objects below are destroyed
// in reverse order of creation when `main` returns.
#include <terreate/graphics/device.hpp>
#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace graphics = terreate::graphics;

namespace {

// Diagnostics (validation messages, for one) go to a sink you own and format.
struct PrintingSink {
  void operator()(const terreate::DiagnosticEvent &event) noexcept {
    std::fprintf(stderr, "example.getting_started: diagnostic: %s\n", event.message.c_str());
  }
};

int blocked(const char *what) {
  std::fprintf(stderr, "example.getting_started: bootstrap=BLOCKED: %s\n", what);
  return EXIT_FAILURE;
}

} // namespace

int main() {
  // 1. What does this machine's Vulkan loader offer?
  const auto capabilities = graphics::queryInstanceCapabilities();
  if (!capabilities)
    return blocked("the Vulkan loader cannot be queried");

  // 2. Request Vulkan 1.3. The validation layer is optional: used when present.
  graphics::InstanceDescription request;
  request.api_version = VK_API_VERSION_1_3;
  request.application_name = "terreate-getting-started";
  request.optional_layers = {"VK_LAYER_KHRONOS_validation"};
  const auto plan = graphics::resolveInstance(request, *capabilities);
  if (!plan)
    return blocked("the Instance request cannot be satisfied");

  // 3. Create the Instance. `sink` must outlive it.
  PrintingSink sink;
  auto instance = graphics::createInstance(*plan, terreate::DiagnosticSinkView::bind(sink));
  if (!instance)
    return blocked("the Instance cannot be created");

  // 4. Which physical devices can serve a Device with the features we need?
  const auto candidates = graphics::queryPhysicalDevices(*instance);
  if (!candidates || candidates->empty())
    return blocked("no physical device");
  for (const auto &candidate : *candidates) {
    graphics::DeviceDescription device_request;
    device_request.required_features_13.synchronization2 = VK_TRUE;
    device_request.required_features_13.dynamicRendering = VK_TRUE;
    // One Queue that can do graphics work, named by an id of your choosing: you look it
    // up by that id after the Device exists.
    graphics::DeviceQueueRequest queue_request;
    queue_request.caller_id = "main";
    queue_request.required_flags = vk::QueueFlagBits::eGraphics;
    device_request.queue_requests.push_back(queue_request);
    const auto device_plan = graphics::resolveDevice(candidate, device_request);
    if (!device_plan)
      continue; // this device cannot: try the next one (the policy is yours)

    auto device = graphics::createDevice(*instance, *device_plan);
    if (!device)
      return blocked("the Device cannot be created");

    // The Queue is looked up by the id given in the request.
    const graphics::Queue queue = device->queue("main");
    if (!queue.valid())
      return blocked("the requested Queue is missing");

    // The resolved plan states what is enabled, not only what was asked for.
    std::printf("example.getting_started: device=%s layers-enabled=%zu\n",
                candidate.capabilities().properties.deviceName.data(),
                plan->enabledLayers().size());
    std::puts("example.getting_started: bootstrap=PASS");
    return EXIT_SUCCESS;
  }
  return blocked("no physical device satisfies the Device request");
}
