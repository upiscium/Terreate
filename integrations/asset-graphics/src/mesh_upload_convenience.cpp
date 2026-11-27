#include <terreate/integration/asset_graphics/mesh_upload_convenience.hpp>

#include <array>
#include <atomic>
#include <optional>
#include <utility>

#include <terreate/graphics/command_pool.hpp>

namespace terreate::integration::asset_graphics {
namespace {

// Everything the GPU may still be using after a failed or timed-out wait. It is
// declared in the order of dependence, so it would be destroyed in a valid order.
struct Helpers {
  graphics::Fence fence;
  graphics::CommandPool pool;
  std::optional<MeshUploadSubmission> submission;
  Helpers *next = nullptr;
};

// The head of the list of abandoned helpers. Nothing ever frees them: this is the
// deliberate, visible leak of a timed-out upload.
std::atomic<Helpers *> abandoned{nullptr};

void abandon(Helpers *helpers) noexcept {
  helpers->next = abandoned.load(std::memory_order_relaxed);
  while (!abandoned.compare_exchange_weak(helpers->next, helpers, std::memory_order_release,
                                          std::memory_order_relaxed)) {
  }
}

} // namespace

auto defaultMeshAttributeBindings(const asset::MeshData &mesh)
    -> std::vector<MeshAttributeBinding> {
  using asset::AttributeSemantic;
  const std::array<MeshAttributeBinding, 5> table{{
      {{AttributeSemantic::position, 0, {}}, 0},
      {{AttributeSemantic::normal, 0, {}}, 1},
      {{AttributeSemantic::texcoord, 0, {}}, 2},
      {{AttributeSemantic::color, 0, {}}, 3},
      {{AttributeSemantic::tangent, 0, {}}, 4},
  }};
  std::vector<MeshAttributeBinding> bindings;
  bindings.reserve(table.size());
  for (const auto &entry : table)
    if (mesh.find(entry.key) != nullptr)
      bindings.push_back(entry);
  return bindings;
}

auto planMeshUploadWithDefaults(const asset::MeshData &mesh,
                                const MeshUploadDescription &description)
    -> terreate::Result<MeshUploadPlan> {
  if (!description.attributes.empty())
    return planMeshUpload(mesh, description);
  MeshUploadDescription resolved = description;
  resolved.attributes = defaultMeshAttributeBindings(mesh);
  auto plan = planMeshUpload(mesh, resolved);
  if (plan)
    plan->used_default_attributes = true;
  return plan;
}

auto submitMeshUpload(const graphics::Device &device, const graphics::Allocator &allocator,
                      graphics::Queue queue, graphics::CommandBuffer &command,
                      const graphics::Fence &fence, graphics::CompletionIdentity identity,
                      const asset::MeshData &mesh, const MeshUploadDescription &description)
    -> terreate::Result<MeshUploadSubmission> {
  auto plan = planMeshUploadWithDefaults(mesh, description);
  if (!plan)
    return std::unexpected(plan.error());
  auto buffers = createMeshBuffers(allocator, *plan);
  if (!buffers)
    return std::unexpected(buffers.error());
  auto items = meshUploadItems(*plan, *buffers);
  if (!items)
    return std::unexpected(items.error());
  auto staging =
      graphics::prepareUploadBatch(allocator, graphics::UploadBatchDescription{.buffers = *items});
  if (!staging)
    return std::unexpected(staging.error());
  if (auto recorded = graphics::recordUploadBatch(device, command, *staging); !recorded)
    return std::unexpected(recorded.error());
  if (auto ended = command.end(); !ended)
    return std::unexpected(ended.error());
  const std::array<const graphics::CommandBuffer *, 1> commands{&command};
  auto receipt =
      graphics::submitFenced(device, queue, fence, identity, {.command_buffers = commands});
  if (!receipt)
    return std::unexpected(receipt.error());
  return MeshUploadSubmission{std::move(*plan), std::move(*buffers), std::move(*staging), *receipt};
}

auto uploadMeshBlocking(const graphics::Device &device, const graphics::Allocator &allocator,
                        graphics::Queue queue, const asset::MeshData &mesh,
                        std::uint64_t timeout_ns, const MeshUploadDescription &description)
    -> terreate::Result<MeshUploadResult> {
  auto fence = graphics::createFence(device, {});
  if (!fence)
    return std::unexpected(fence.error());
  graphics::CommandPoolDescription pool_description;
  pool_description.queue_family_index = static_cast<std::uint32_t>(queue.familyIndex());
  auto pool = graphics::createCommandPool(device, pool_description);
  if (!pool)
    return std::unexpected(pool.error());
  auto command = pool->allocatePrimary();
  if (!command)
    return std::unexpected(command.error());
  if (auto begun = command->begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit); !begun)
    return std::unexpected(begun.error());

  // The identity only has to be valid and unique for this private fence.
  static constexpr int domain = 0;
  auto submission =
      submitMeshUpload(device, allocator, queue, *command, *fence, {&domain, 1}, mesh, description);
  if (!submission)
    return std::unexpected(submission.error());

  auto waited = fence->wait(timeout_ns);
  if (!waited || *waited != graphics::WaitStatus::satisfied) {
    // The GPU may still read the staging memory and write the buffers, and the
    // command buffer and fence are still in use: destroying any of them would be
    // undefined behavior. They are deliberately leaked.
    abandon(new Helpers{std::move(*fence), std::move(*pool), std::move(*submission)});
    if (!waited)
      return std::unexpected(waited.error());
    return std::unexpected(terreate::Error{make_error_code(MeshUploadError::wait_timeout),
                                           "uploadMeshBlocking",
                                           "the fence did not signal in time"});
  }
  // Complete: the staging, command buffer, pool and fence are destroyed here.
  return MeshUploadResult{std::move(submission->plan), std::move(submission->buffers)};
}

} // namespace terreate::integration::asset_graphics
