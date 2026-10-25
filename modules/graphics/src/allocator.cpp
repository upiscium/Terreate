// Compile this source twice: once for the VMA implementation, once for the
// Terreate Allocator API. This makes the API's VMA calls external relocations
// that the native test can intercept with the GNU ld --wrap hooks. There is
// exactly one VMA_IMPLEMENTATION object in the final Graphics archive.
#if defined(TERREATE_GRAPHICS_VMA_IMPLEMENTATION_ONLY)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#else
#include <terreate/graphics/allocator.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace terreate::graphics {
namespace {
class AllocatorErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.graphics.allocator"; }
  std::string message(int value) const override {
    switch (static_cast<AllocatorError>(value)) {
    case AllocatorError::invalid_parent:
      return "invalid allocator parent";
    case AllocatorError::parent_mismatch:
      return "allocator parents do not match";
    case AllocatorError::unsupported_api_version:
      return "unsupported allocator API version";
    case AllocatorError::incompatible_flags:
      return "incompatible allocator flags";
    case AllocatorError::invalid_allocator:
      return "invalid allocator";
    }
    return "unknown allocator error";
  }
};

auto failure(AllocatorError error, std::string detail) {
  return std::unexpected(
      terreate::Error{make_error_code(error), "graphics allocator", std::move(detail)});
}
} // namespace

const std::error_category &allocator_error_category() noexcept {
  static const AllocatorErrorCategory category;
  return category;
}
std::error_code make_error_code(AllocatorError error) noexcept {
  return {static_cast<int>(error), allocator_error_category()};
}

struct Allocator::Impl {
  // Keep the dispatch loader alive independently of wrapper object moves.
  vk::raii::Context context{};
  VmaAllocator handle = VK_NULL_HANDLE;
  AllocatorConfiguration configuration{};
  vk::PhysicalDeviceMemoryProperties memory{};
  ~Impl() {
    if (handle != VK_NULL_HANDLE) {
      vmaDestroyAllocator(handle);
    }
  }
};

Allocator::Allocator(std::unique_ptr<Impl> impl) noexcept : implementation_(std::move(impl)) {}
Allocator::Allocator(Allocator &&) noexcept = default;
Allocator &Allocator::operator=(Allocator &&) noexcept = default;
Allocator::~Allocator() = default;
bool Allocator::valid() const noexcept { return nativeHandle() != VK_NULL_HANDLE; }
VmaAllocator Allocator::nativeHandle() const noexcept {
  return implementation_ ? implementation_->handle : VK_NULL_HANDLE;
}
const AllocatorConfiguration *Allocator::configuration() const noexcept {
  return implementation_ ? &implementation_->configuration : nullptr;
}
VmaAllocatorCreateFlags Allocator::effectiveFlags() const noexcept {
  return configuration() ? configuration()->flags : 0;
}
const vk::PhysicalDeviceMemoryProperties *Allocator::memoryProperties() const noexcept {
  return implementation_ ? &implementation_->memory : nullptr;
}
std::span<const vk::MemoryHeap> Allocator::memoryHeaps() const noexcept {
  const auto *memory = memoryProperties();
  return memory
             ? std::span<const vk::MemoryHeap>{memory->memoryHeaps.data(), memory->memoryHeapCount}
             : std::span<const vk::MemoryHeap>{};
}
std::span<const vk::MemoryType> Allocator::memoryTypes() const noexcept {
  const auto *memory = memoryProperties();
  return memory
             ? std::span<const vk::MemoryType>{memory->memoryTypes.data(), memory->memoryTypeCount}
             : std::span<const vk::MemoryType>{};
}
terreate::Result<AllocatorBudgets> Allocator::budgets() const {
  if (!valid())
    return failure(AllocatorError::invalid_allocator, "budgets on moved-from allocator");
  AllocatorBudgets result{};
  result.heap_count = implementation_->memory.memoryHeapCount;
  result.memory_budget_enabled =
      (effectiveFlags() & VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT) != 0;
  vmaGetHeapBudgets(nativeHandle(), result.heaps.data());
  return result;
}
terreate::Result<VmaTotalStatistics> Allocator::statistics() const {
  if (!valid())
    return failure(AllocatorError::invalid_allocator, "statistics on moved-from allocator");
  VmaTotalStatistics result{};
  vmaCalculateStatistics(nativeHandle(), &result);
  return result;
}

auto createAllocator(const Instance &instance, const PhysicalDeviceCandidate &candidate,
                     const Device &device, const AllocatorDescription &description)
    -> terreate::Result<Allocator> {
  if (!instance.valid() || !device.valid() || !candidate.device().valid() || !instance.plan() ||
      !device.plan()) {
    return failure(AllocatorError::invalid_parent, "live Instance, candidate and Device required");
  }
  if (!candidate.device().correlatedWith(instance) || !device.correlatedWith(instance) ||
      device.plan()->physicalDevice().nativeHandle() != candidate.device().nativeHandle()) {
    return failure(AllocatorError::parent_mismatch,
                   "Device must use the supplied candidate and Instance");
  }
  const auto version = description.api_version.value_or(default_allocator_api_version);
  if ((version != VK_API_VERSION_1_0 && version != VK_API_VERSION_1_1 &&
       version != VK_API_VERSION_1_2 && version != VK_API_VERSION_1_3) ||
      version > instance.plan()->effectiveApiVersion() ||
      version > device.plan()->capabilities().api_version) {
    return failure(AllocatorError::unsupported_api_version,
                   "request must be supported Vulkan 1.0--1.3, patch zero");
  }
  const auto flags = description.flags;
  constexpr VmaAllocatorCreateFlags supported =
      VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT |
      VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT |
      VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT | VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT |
      VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
  if ((flags & ~supported) != 0) {
    return failure(AllocatorError::incompatible_flags,
                   "unknown flags or prerequisites not represented by DevicePlan (including AMD "
                   "coherent memory, memory priority and maintenance5)");
  }
  const auto enabled = [&](const char *name) {
    const auto &extensions = device.plan()->enabledExtensions();
    return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
  };
  const auto requested = [&](VmaAllocatorCreateFlags flag) { return (flags & flag) != 0; };
  if (requested(VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT) &&
      version < VK_API_VERSION_1_1 &&
      (!enabled("VK_KHR_dedicated_allocation") || !enabled("VK_KHR_get_memory_requirements2"))) {
    return failure(AllocatorError::incompatible_flags,
                   "dedicated allocation requires Vulkan 1.1 or both KHR extensions enabled");
  }
  if (requested(VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT) && version < VK_API_VERSION_1_1 &&
      !enabled("VK_KHR_bind_memory2")) {
    return failure(AllocatorError::incompatible_flags,
                   "bind memory2 requires Vulkan 1.1 or VK_KHR_bind_memory2 enabled");
  }
  const auto &instance_extensions = instance.plan()->enabledExtensions();
  const bool properties2 =
      version >= VK_API_VERSION_1_1 ||
      std::find(instance_extensions.begin(), instance_extensions.end(),
                "VK_KHR_get_physical_device_properties2") != instance_extensions.end();
  if (requested(VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT) &&
      (!enabled("VK_EXT_memory_budget") || !properties2)) {
    return failure(AllocatorError::incompatible_flags,
                   "memory budget requires enabled EXT_memory_budget and properties2 access");
  }
  if (requested(VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT) &&
      (!device.plan()->enabledFeatures12().bufferDeviceAddress ||
       (version < VK_API_VERSION_1_2 && !enabled("VK_KHR_buffer_device_address")))) {
    return failure(AllocatorError::incompatible_flags,
                   "buffer device address requires enabled feature and core 1.2 or KHR extension");
  }
  if (requested(VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT) &&
      (!device.plan()->enabledFeatures13().maintenance4 ||
       (version < VK_API_VERSION_1_3 && !enabled("VK_KHR_maintenance4")))) {
    return failure(AllocatorError::incompatible_flags,
                   "maintenance4 requires enabled feature and core 1.3 or KHR extension");
  }
  try {
    auto impl = std::make_unique<Allocator::Impl>();
    impl->configuration = {description, flags, version};
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = impl->context.getDispatcher()->vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(functions.vkGetInstanceProcAddr(
            static_cast<VkInstance>(instance.nativeHandle()), "vkGetDeviceProcAddr"));
    if (!functions.vkGetDeviceProcAddr) {
      return failure(AllocatorError::invalid_parent, "vkGetDeviceProcAddr unavailable");
    }
    VmaAllocatorCreateInfo info{};
    info.flags = flags;
    info.instance = static_cast<VkInstance>(instance.nativeHandle());
    info.physicalDevice = static_cast<VkPhysicalDevice>(candidate.device().nativeHandle());
    info.device = static_cast<VkDevice>(device.nativeHandle());
    info.vulkanApiVersion = version;
    info.pVulkanFunctions = &functions;
    const VkResult result = vmaCreateAllocator(&info, &impl->handle);
    if (result != VK_SUCCESS) {
      return std::unexpected(terreate::Error{vk::make_error_code(static_cast<vk::Result>(result)),
                                             "vmaCreateAllocator",
                                             "native allocator creation failed"});
    }
    const VkPhysicalDeviceMemoryProperties *memory = nullptr;
    vmaGetMemoryProperties(impl->handle, &memory);
    impl->memory = vk::PhysicalDeviceMemoryProperties{*memory};
    return Allocator{std::move(impl)};
  } catch (const vk::SystemError &error) {
    return std::unexpected(terreate::Error{error.code(), "createAllocator", error.what()});
  }
}

} // namespace terreate::graphics

#endif // TERREATE_GRAPHICS_VMA_IMPLEMENTATION_ONLY
