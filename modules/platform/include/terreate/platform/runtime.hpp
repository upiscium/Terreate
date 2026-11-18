#ifndef TERREATE_PLATFORM_RUNTIME_HPP
#define TERREATE_PLATFORM_RUNTIME_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <terreate/core/diagnostics.hpp>
#include <terreate/core/result.hpp>
#include <terreate/platform/events.hpp>

namespace terreate::platform {

class Window;
struct WindowDescription;

enum class PlatformError : std::uint8_t {
  invalid_runtime = 1,
  already_initialized,
  backend_unavailable,
  init_failed,
  wrong_thread,
  vulkan_unavailable,
  invalid_description,
  invalid_window,
  native_failure,
  allocation_failed,
  // Appended in #348; existing values are unchanged.
  invalid_argument,
};

[[nodiscard]] const std::error_category &platform_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(PlatformError error) noexcept;

/// The category of native GLFW error codes (GLFW_PLATFORM_UNAVAILABLE, ...).
/// When a GLFW call fails and GLFW reported an error, the returned Error
/// carries that native code in this category and GLFW's description as its
/// detail. init_failed and native_failure are used only when GLFW reported
/// nothing.
[[nodiscard]] const std::error_category &glfw_error_category() noexcept;

/// The GLFW window-system backend. `automatic` lets GLFW choose; every other
/// value is a request that fails with backend_unavailable when GLFW cannot use
/// it, without falling back to another backend. `null` creates windows
/// without a display (GLFW 3.4), for headless tests and tools.
enum class PlatformBackend : std::uint8_t {
  automatic,
  x11,
  wayland,
  win32,
  cocoa,
  null,
};

struct RuntimeDescription {
  PlatformBackend backend = PlatformBackend::automatic;
};

/// The process's GLFW library state, owned explicitly by the application.
///
/// GLFW keeps its initialization, error callback and window hints per
/// process, so at most one Runtime is alive at a time; createRuntime fails with
/// already_initialized otherwise. Nothing else is a singleton: a Runtime owns
/// any number of Windows. It does not poll events and owns no loop.
///
/// The Runtime records the thread that created it. GLFW requires its calls on
/// that thread, so every Runtime and Window operation that calls GLFW
/// (vulkanSupported, requiredInstanceExtensions, pollEvents, waitEvents,
/// createWindow and the Window queries) fails with wrong_thread on another
/// thread, before any GLFW call. The one exception is postEmptyEvent, which
/// GLFW allows from any thread. Accessors that only read Terreate's own state
/// (valid, requested, backend, liveWindowCount, configuration, nativeHandle,
/// id, correlatedWith) call no GLFW function and are not checked. The Runtime
/// and its Windows
/// must also be destroyed on that thread, and every Window must be destroyed
/// before its Runtime (glfwTerminate would destroy them underneath their
/// owners). liveWindowCount() makes the second rule observable; neither is
/// checked at destruction.
///
/// A Runtime can be moved into a new object but not assigned over another:
/// assignment would terminate GLFW underneath the target's live Windows.
class Runtime final {
public:
  Runtime() = delete;
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(Runtime &&other) noexcept;
  Runtime &operator=(Runtime &&other) = delete;
  ~Runtime();

  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] bool valid() const noexcept { return implementation_ != nullptr; }

  [[nodiscard]] const RuntimeDescription &requested() const noexcept;
  /// The backend GLFW initialized, never `automatic` for a valid Runtime.
  [[nodiscard]] PlatformBackend backend() const noexcept;
  [[nodiscard]] std::size_t liveWindowCount() const noexcept;

  /// glfwVulkanSupported: whether GLFW found a Vulkan loader. Surface
  /// extensions may still be unavailable: GLFW 3.4's null backend, for
  /// example, reports a loader but names no surface extensions.
  [[nodiscard]] auto vulkanSupported() const -> terreate::Result<bool>;
  /// An owned copy of glfwGetRequiredInstanceExtensions. The application
  /// passes it to the Vulkan Instance request itself; Platform has no
  /// dependency on Graphics. Fails with vulkan_unavailable when GLFW names no
  /// surface extensions for this backend.
  [[nodiscard]] auto requiredInstanceExtensions() const
      -> terreate::Result<std::vector<std::string>>;

  /// Event processing (#348). The application decides when events are
  /// processed; Terreate never calls application code from inside GLFW.
  /// Terreate's per-window GLFW callbacks store plain values. GLFW calls them
  /// during these calls and also synchronously inside some other GLFW calls
  /// (for example a native glfwSetWindowSize). Each of these calls returns
  /// everything stored since the previous one, in arrival order, and empties
  /// the store. If GLFW reports an error, the Error is returned and the stored
  /// events stay for the next call. All three are thread-checked.
  ///
  /// glfwPollEvents: process pending events and return.
  [[nodiscard]] auto pollEvents() const -> terreate::Result<EventBatch>;
  /// glfwWaitEvents: sleep until at least one event arrives.
  [[nodiscard]] auto waitEvents() const -> terreate::Result<EventBatch>;
  /// glfwWaitEventsTimeout: sleep until an event arrives or `timeout` passes.
  /// GLFW requires a positive, finite timeout; zero, a negative or a
  /// non-finite value fails with invalid_argument. Use pollEvents for zero.
  [[nodiscard]] auto waitEvents(std::chrono::duration<double> timeout) const
      -> terreate::Result<EventBatch>;
  /// glfwPostEmptyEvent: wake a thread blocked in waitEvents. Unlike every
  /// other operation, GLFW allows this call from any thread, so it is not
  /// thread-checked; the Runtime must stay alive during the call.
  [[nodiscard]] auto postEmptyEvent() const -> terreate::Result<void>;

  struct Impl;

private:
  explicit Runtime(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_{};

  friend auto createRuntime(const RuntimeDescription &description,
                            terreate::DiagnosticSinkView sink) -> terreate::Result<Runtime>;
  friend class Window;
  friend auto createWindow(const Runtime &runtime, const WindowDescription &description)
      -> terreate::Result<Window>;
};

/// Initialize GLFW with the requested backend. GLFW errors reported while the
/// Runtime is alive are also forwarded to `sink` as error events with the
/// category "PLATFORM"; the borrowed sink target must outlive the Runtime.
///
/// The Runtime installs GLFW's process-wide error callback. A callback
/// installed before it keeps receiving every error (it is called after the
/// sink) and is restored when the Runtime is destroyed or creation fails. An
/// error raised while an error is being delivered (by the sink or that
/// callback) reaches the sink once more but is not passed on again, and deeper
/// nesting is dropped, so delivery cannot recurse without bound. The
/// GLFW_PLATFORM init hint is reset to GLFW's default after initialization:
/// GLFW offers no way to read an earlier value back.
[[nodiscard]] auto createRuntime(const RuntimeDescription &description = {},
                                 terreate::DiagnosticSinkView sink = {})
    -> terreate::Result<Runtime>;

} // namespace terreate::platform

namespace std {
template <> struct is_error_code_enum<terreate::platform::PlatformError> : true_type {};
} // namespace std

#endif // TERREATE_PLATFORM_RUNTIME_HPP
