#ifndef TERREATE_GRAPHICS_INSTANCE_IMPL_HPP
#define TERREATE_GRAPHICS_INSTANCE_IMPL_HPP

#include <memory>
#include <optional>

#include "graphics_diagnostics.hpp"

#include <terreate/graphics/instance.hpp>
#include <terreate/graphics/physical_device.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

struct Instance::LifetimeToken {
  bool active = true;
  std::unique_ptr<LifetimeToken> next{};
};

/// One private, parent-owned capability-query token.  A PhysicalDevice keeps
/// only a pointer to this record; the record retains the exact native handle,
/// implementation identity, and pNext-free capability snapshot needed to
/// validate that borrowed view and its public observation.
struct Instance::PhysicalDeviceToken {
  vk::PhysicalDevice native_handle{};
  const Instance::Impl *parent_identity = nullptr;
  PhysicalDeviceCapabilities capabilities{};
  bool active = true;
  std::unique_ptr<PhysicalDeviceToken> next{};
};

struct CallbackState {
  terreate::DiagnosticSinkView sink{};
};

struct Instance::Impl {
  std::unique_ptr<Instance::LifetimeToken> lifetime;
  std::unique_ptr<Instance::PhysicalDeviceToken> physical_device_tokens;
  std::unique_ptr<vk::raii::Context> context;
  std::unique_ptr<CallbackState> callback;
  vk::raii::Instance instance;
  std::optional<vk::raii::DebugUtilsMessengerEXT> messenger;
  InstancePlan plan;

  Impl(std::unique_ptr<vk::raii::Context> context_owner,
       std::unique_ptr<CallbackState> callback_owner, vk::raii::Instance instance_owner,
       std::optional<vk::raii::DebugUtilsMessengerEXT> messenger_owner,
       const InstancePlan &instance_plan);

  ~Impl() noexcept;
};

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_INSTANCE_IMPL_HPP
