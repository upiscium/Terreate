#ifndef TERREATE_PLATFORM_RUNTIME_IMPL_HPP
#define TERREATE_PLATFORM_RUNTIME_IMPL_HPP

#include <cstddef>
#include <thread>

#include <terreate/core/diagnostics.hpp>
#include <terreate/core/result.hpp>
#include <terreate/platform/runtime.hpp>

namespace terreate::platform {

// Heap-owned, so its address is stable across Runtime moves; Windows keep a
// pointer to it for the thread check and the live-window count.
struct Runtime::Impl {
  RuntimeDescription requested{};
  PlatformBackend backend = PlatformBackend::automatic;
  std::thread::id thread{};
  terreate::DiagnosticSinkView sink{};
  std::size_t live_windows = 0;
  // The GLFW error callback installed before this Runtime: chained while the
  // Runtime is alive and restored when it is destroyed.
  void (*previous_error_callback)(int, const char *) = nullptr;
};

namespace detail {

[[nodiscard]] terreate::Error platformError(PlatformError error, const char *operation,
                                            const char *detail);

// A native GLFW error code as an Error in glfw_error_category.
[[nodiscard]] terreate::Error glfwError(int code, const char *operation, const char *description);

// The last GLFW error of this thread, as an Error in glfw_error_category, or
// `fallback` when GLFW reported none. Clears GLFW's last error.
[[nodiscard]] terreate::Error takeGlfwError(PlatformError fallback, const char *operation);

// Checks shared by every Runtime and Window operation.
[[nodiscard]] terreate::Result<void> checkThread(const Runtime::Impl *runtime,
                                                 const char *operation);

} // namespace detail
} // namespace terreate::platform

#endif // TERREATE_PLATFORM_RUNTIME_IMPL_HPP
