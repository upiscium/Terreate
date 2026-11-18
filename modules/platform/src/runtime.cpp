#include <terreate/platform/runtime.hpp>

#include "runtime_impl.hpp"

#include <GLFW/glfw3.h>

#include <atomic>
#include <new>
#include <string>
#include <utility>

namespace terreate::platform {
namespace {
class PlatformErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "terreate.platform"; }
  std::string message(int value) const override {
    switch (static_cast<PlatformError>(value)) {
    case PlatformError::invalid_runtime:
      return "invalid Runtime";
    case PlatformError::already_initialized:
      return "a Runtime is already alive in this process";
    case PlatformError::backend_unavailable:
      return "the requested window-system backend is not available";
    case PlatformError::init_failed:
      return "GLFW initialization failed";
    case PlatformError::wrong_thread:
      return "called on a thread other than the Runtime's thread";
    case PlatformError::vulkan_unavailable:
      return "GLFW cannot provide Vulkan window surfaces";
    case PlatformError::invalid_description:
      return "invalid window description";
    case PlatformError::invalid_window:
      return "invalid Window";
    case PlatformError::native_failure:
      return "GLFW call failed";
    case PlatformError::allocation_failed:
      return "allocation failed";
    }
    return "unknown platform error";
  }
};

const char *glfwErrorName(int code) noexcept {
  switch (code) {
  case GLFW_NOT_INITIALIZED:
    return "GLFW_NOT_INITIALIZED";
  case GLFW_NO_CURRENT_CONTEXT:
    return "GLFW_NO_CURRENT_CONTEXT";
  case GLFW_INVALID_ENUM:
    return "GLFW_INVALID_ENUM";
  case GLFW_INVALID_VALUE:
    return "GLFW_INVALID_VALUE";
  case GLFW_OUT_OF_MEMORY:
    return "GLFW_OUT_OF_MEMORY";
  case GLFW_API_UNAVAILABLE:
    return "GLFW_API_UNAVAILABLE";
  case GLFW_VERSION_UNAVAILABLE:
    return "GLFW_VERSION_UNAVAILABLE";
  case GLFW_PLATFORM_ERROR:
    return "GLFW_PLATFORM_ERROR";
  case GLFW_FORMAT_UNAVAILABLE:
    return "GLFW_FORMAT_UNAVAILABLE";
  case GLFW_NO_WINDOW_CONTEXT:
    return "GLFW_NO_WINDOW_CONTEXT";
  case GLFW_CURSOR_UNAVAILABLE:
    return "GLFW_CURSOR_UNAVAILABLE";
  case GLFW_FEATURE_UNAVAILABLE:
    return "GLFW_FEATURE_UNAVAILABLE";
  case GLFW_FEATURE_UNIMPLEMENTED:
    return "GLFW_FEATURE_UNIMPLEMENTED";
  case GLFW_PLATFORM_UNAVAILABLE:
    return "GLFW_PLATFORM_UNAVAILABLE";
  default:
    return "GLFW_UNKNOWN_ERROR";
  }
}

class GlfwErrorCategory final : public std::error_category {
public:
  const char *name() const noexcept override { return "glfw"; }
  std::string message(int value) const override { return glfwErrorName(value); }
};

// GLFW's initialization and error callback are process-global. These mirror
// that: at most one live Runtime, and the error callback forwards to it.
std::atomic<bool> runtime_alive{false};
std::atomic<const Runtime::Impl *> error_target{nullptr};

void emitError(const terreate::DiagnosticSinkView &sink, int code,
               const char *description) noexcept {
  if (!sink)
    return;
  try {
    terreate::DiagnosticEvent event;
    event.severity = terreate::DiagnosticSeverity::error;
    event.categories = {"PLATFORM"};
    event.source = "glfw";
    event.code = terreate::DiagnosticCode{glfwErrorName(code), code};
    event.message = description == nullptr ? std::string{} : std::string{description};
    sink.emit(event);
  } catch (const std::bad_alloc &) {
    // An event that cannot be built is dropped; the failing call still
    // returns its own Error. A callback must never throw into GLFW.
    return;
  }
}

// How deep this thread is inside error delivery. The sink or the earlier
// callback may call GLFW and fail again; without a bound that would recurse.
thread_local int delivery_depth = 0;

void forwardError(int code, const char *description) noexcept {
  const auto *target = error_target.load(std::memory_order_acquire);
  // An error raised while delivering an error reaches the sink once more but
  // is not passed on again; anything nested deeper is dropped.
  if (target == nullptr || delivery_depth >= 2)
    return;
  ++delivery_depth;
  emitError(target->sink, code, description);
  // Keep a callback the application or another GLFW user installed working.
  if (delivery_depth == 1 && target->previous_error_callback != nullptr)
    target->previous_error_callback(code, description);
  --delivery_depth;
}

int toGlfw(PlatformBackend backend) noexcept {
  switch (backend) {
  case PlatformBackend::automatic:
    return GLFW_ANY_PLATFORM;
  case PlatformBackend::x11:
    return GLFW_PLATFORM_X11;
  case PlatformBackend::wayland:
    return GLFW_PLATFORM_WAYLAND;
  case PlatformBackend::win32:
    return GLFW_PLATFORM_WIN32;
  case PlatformBackend::cocoa:
    return GLFW_PLATFORM_COCOA;
  case PlatformBackend::null:
    return GLFW_PLATFORM_NULL;
  }
  return -1;
}

PlatformBackend fromGlfw(int platform) noexcept {
  switch (platform) {
  case GLFW_PLATFORM_X11:
    return PlatformBackend::x11;
  case GLFW_PLATFORM_WAYLAND:
    return PlatformBackend::wayland;
  case GLFW_PLATFORM_WIN32:
    return PlatformBackend::win32;
  case GLFW_PLATFORM_COCOA:
    return PlatformBackend::cocoa;
  case GLFW_PLATFORM_NULL:
  default:
    return PlatformBackend::null;
  }
}

terreate::Result<Runtime> failure(terreate::Error error) {
  return std::unexpected(std::move(error));
}
} // namespace

const std::error_category &platform_error_category() noexcept {
  static const PlatformErrorCategory category;
  return category;
}
std::error_code make_error_code(PlatformError error) noexcept {
  return {static_cast<int>(error), platform_error_category()};
}
const std::error_category &glfw_error_category() noexcept {
  static const GlfwErrorCategory category;
  return category;
}

namespace detail {
terreate::Error platformError(PlatformError error, const char *operation, const char *detail) {
  try {
    return terreate::Error{make_error_code(error), operation, detail == nullptr ? "" : detail};
  } catch (const std::bad_alloc &) {
    // Code-only Error construction does not allocate while handling exhaustion.
    return terreate::Error{make_error_code(error)};
  }
}

terreate::Error takeGlfwError(PlatformError fallback, const char *operation) {
  const char *description = nullptr;
  const int code = glfwGetError(&description);
  if (code == GLFW_NO_ERROR)
    return platformError(fallback, operation, "GLFW reported no error");
  return glfwError(code, operation, description);
}

terreate::Error glfwError(int code, const char *operation, const char *description) {
  try {
    return terreate::Error{std::error_code{code, glfw_error_category()}, operation,
                           description == nullptr ? "" : description};
  } catch (const std::bad_alloc &) {
    return terreate::Error{std::error_code{code, glfw_error_category()}};
  }
}

terreate::Result<void> checkThread(const Runtime::Impl *runtime, const char *operation) {
  if (runtime == nullptr)
    return std::unexpected(platformError(PlatformError::invalid_runtime, operation, nullptr));
  if (std::this_thread::get_id() != runtime->thread)
    return std::unexpected(platformError(PlatformError::wrong_thread, operation,
                                         "GLFW requires the thread that created the Runtime"));
  return {};
}
} // namespace detail

auto createRuntime(const RuntimeDescription &description, terreate::DiagnosticSinkView sink)
    -> terreate::Result<Runtime> {
  constexpr auto operation = "createRuntime";
  const int platform = toGlfw(description.backend);
  if (platform < 0)
    return failure(detail::platformError(PlatformError::invalid_description, operation,
                                         "unknown PlatformBackend value"));
  if (runtime_alive.exchange(true, std::memory_order_acq_rel))
    return failure(detail::platformError(PlatformError::already_initialized, operation,
                                         "GLFW state is per process"));
  // Release the process slot on every failure below.
  struct Slot {
    bool keep = false;
    ~Slot() {
      if (!keep)
        runtime_alive.store(false, std::memory_order_release);
    }
  } slot;

  std::unique_ptr<Runtime::Impl> impl;
  try {
    impl = std::make_unique<Runtime::Impl>();
  } catch (const std::bad_alloc &) {
    return failure(terreate::Error{make_error_code(PlatformError::allocation_failed)});
  }
  if (description.backend != PlatformBackend::automatic &&
      glfwPlatformSupported(platform) != GLFW_TRUE)
    return failure(detail::platformError(PlatformError::backend_unavailable, operation,
                                         "GLFW was built without this backend"));

  impl->requested = description;
  impl->sink = sink;
  impl->thread = std::this_thread::get_id();
  error_target.store(impl.get(), std::memory_order_release);
  impl->previous_error_callback = glfwSetErrorCallback(&forwardError);
  glfwInitHint(GLFW_PLATFORM, platform);
  const bool initialized = glfwInit() == GLFW_TRUE;
  // Init hints are process-global too. GLFW has no getter for them, so an
  // earlier value cannot be restored; reset to GLFW's default instead.
  glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
  if (!initialized) {
    auto error = detail::takeGlfwError(PlatformError::init_failed, operation);
    glfwSetErrorCallback(impl->previous_error_callback);
    error_target.store(nullptr, std::memory_order_release);
    return failure(std::move(error));
  }
  impl->backend = fromGlfw(glfwGetPlatform());
  slot.keep = true;
  return Runtime{std::move(impl)};
}

Runtime::Runtime(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

Runtime::Runtime(Runtime &&other) noexcept = default;

Runtime::~Runtime() {
  if (implementation_ == nullptr)
    return;
  glfwTerminate();
  glfwSetErrorCallback(implementation_->previous_error_callback);
  error_target.store(nullptr, std::memory_order_release);
  runtime_alive.store(false, std::memory_order_release);
}

const RuntimeDescription &Runtime::requested() const noexcept {
  static const RuntimeDescription none{};
  return implementation_ == nullptr ? none : implementation_->requested;
}

PlatformBackend Runtime::backend() const noexcept {
  return implementation_ == nullptr ? PlatformBackend::automatic : implementation_->backend;
}

std::size_t Runtime::liveWindowCount() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->live_windows;
}

auto Runtime::vulkanSupported() const -> terreate::Result<bool> {
  if (auto checked = detail::checkThread(implementation_.get(), "vulkanSupported"); !checked)
    return std::unexpected(std::move(checked.error()));
  return glfwVulkanSupported() == GLFW_TRUE;
}

auto Runtime::requiredInstanceExtensions() const -> terreate::Result<std::vector<std::string>> {
  constexpr auto operation = "requiredInstanceExtensions";
  if (auto checked = detail::checkThread(implementation_.get(), operation); !checked)
    return std::unexpected(std::move(checked.error()));
  std::uint32_t count = 0;
  const char **names = glfwGetRequiredInstanceExtensions(&count);
  if (names == nullptr) {
    const char *description = nullptr;
    glfwGetError(&description);
    return std::unexpected(
        detail::platformError(PlatformError::vulkan_unavailable, operation, description));
  }
  try {
    std::vector<std::string> extensions;
    extensions.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
      extensions.emplace_back(names[index]);
    return extensions;
  } catch (const std::bad_alloc &) {
    return std::unexpected(terreate::Error{make_error_code(PlatformError::allocation_failed)});
  }
}

} // namespace terreate::platform
