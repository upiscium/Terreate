#ifndef TERREATE_GRAPHICS_INSTANCE_IMPL_HPP
#define TERREATE_GRAPHICS_INSTANCE_IMPL_HPP

#include <memory>
#include <optional>

#include "graphics_diagnostics.hpp"

#include <terreate/graphics/instance.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace terreate::graphics {

struct CallbackState {
  terreate::DiagnosticSinkView sink{};
};

struct InstanceStorage {
  std::unique_ptr<vk::raii::Context> context;
  std::unique_ptr<CallbackState> callback;
  vk::raii::Instance instance;
  std::optional<vk::raii::DebugUtilsMessengerEXT> messenger;
  InstancePlan plan;

  InstanceStorage(std::unique_ptr<vk::raii::Context> context_owner,
                  std::unique_ptr<CallbackState> callback_owner, vk::raii::Instance instance_owner,
                  std::optional<vk::raii::DebugUtilsMessengerEXT> messenger_owner,
                  const InstancePlan &instance_plan);

  void destroy() noexcept;
  ~InstanceStorage() noexcept;
};

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_INSTANCE_IMPL_HPP
