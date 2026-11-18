# Platform Runtime and Window (#347)

`<terreate/platform/runtime.hpp>` and `<terreate/platform/window.hpp>` give the
application explicit ownership of GLFW: the library state (`Runtime`) and each
window (`Window`). They do not process events (#348), create Vulkan surfaces
(#363) or own a loop. The application decides when windows exist and, later,
when events are polled.

Platform is the only component that depends on GLFW (3.4). The public headers
forward-declare `GLFWwindow` and never include GLFW, but the static library
links `glfw` publicly. Core and Graphics consumers never receive GLFW.

## Runtime

`createRuntime(description, sink)` initializes GLFW and returns a move-only
`Runtime` that terminates it on destruction.

GLFW keeps its initialization, error callback, init hints and window hints per
process. The Runtime mirrors that instead of hiding it:

- At most one Runtime is alive at a time. A second `createRuntime` fails with
  `already_initialized`. After the first is destroyed, a new one may be created.
  Nothing else is a singleton: one Runtime owns any number of Windows.
- `RuntimeDescription::backend` selects the window-system backend:
  `automatic`, `x11`, `wayland`, `win32`, `cocoa` or `null`. A specific request
  that GLFW cannot use fails with `backend_unavailable` (or GLFW's own error
  from `glfwInit`); there is no fallback to another backend. `backend()` reports
  the backend GLFW actually initialized. GLFW has no getter for init hints,
  so an earlier `GLFW_PLATFORM` value cannot be restored; the Runtime resets it
  to GLFW's default after initialization.
- `null` (GLFW 3.4) creates windows without a display. Tests and tools use it.

### Thread contract

GLFW requires its calls on the thread that initialized it. The Runtime records
that thread. Every Runtime and Window operation called on another thread fails
with `wrong_thread` before any GLFW call. Destruction cannot report errors, so
the Runtime and its Windows must also be destroyed on that thread.

### Errors and diagnostics

While a Runtime is alive, it owns GLFW's process-wide error callback and
forwards each GLFW error to its `DiagnosticSinkView` as a Core
`DiagnosticEvent`: severity `error`, category `PLATFORM`, source `glfw`, code
`{"GLFW_INVALID_ENUM", 0x10003}` (for example), and GLFW's description as the
message. The borrowed sink target must outlive the Runtime.

An error callback installed before the Runtime is not lost. It keeps
receiving every error, after the sink, and it is reinstalled when the Runtime
is destroyed or when creation fails.

The sink or that earlier callback may itself call GLFW and fail. Such a nested
error reaches the sink once more but is not passed to the earlier callback
again, and anything nested deeper is dropped. Delivery therefore cannot recurse
without bound.

A failing operation also returns an Error. When GLFW reported an error, its
code is in `glfw_error_category()` and GLFW's description is the detail;
`init_failed` and `native_failure` are used only when GLFW reported nothing.
Before each checked GLFW call, any error left by an earlier call is dropped
(it already reached the sink), so it cannot fail an unrelated query.

### Vulkan

| Function | GLFW call |
| --- | --- |
| `vulkanSupported()` | `glfwVulkanSupported` |
| `requiredInstanceExtensions()` | `glfwGetRequiredInstanceExtensions` (owned copy) |

`vulkanSupported()` only means that GLFW found a Vulkan loader.
`requiredInstanceExtensions()` returns the instance extensions window surfaces
need on the effective backend, or `vulkan_unavailable` when GLFW names none.
GLFW 3.4's null backend is such a case: it reports a loader but no surface
extensions.

Platform does not depend on Graphics. The application copies the list into
`InstanceDescription::required_extensions` before creating the Instance:

```cpp
auto runtime = terreate::platform::createRuntime();
auto extensions = runtime->requiredInstanceExtensions();
terreate::graphics::InstanceDescription description;
description.required_extensions = *extensions;
```

## Window

`createWindow(runtime, description)` creates a move-only `Window`:

- The window always has no client API (`GLFW_NO_API`), so Vulkan can present
  to it.
- All window hints are reset before creation and again afterwards, so no
  setting carries over between windows or into native GLFW calls.
- A zero size, a size above `INT_MAX`, or a title containing NUL is rejected
  with `invalid_description` before GLFW creates anything.
- `configuration()` keeps the requested description and the effective one read
  back after creation (size and the resizable, visible, decorated and focused
  attributes), since the window manager or backend may change them.

| Query | GLFW call |
| --- | --- |
| `size()` | `glfwGetWindowSize` (screen coordinates) |
| `framebufferSize()` | `glfwGetFramebufferSize` (pixels; the swapchain extent) |
| `contentScale()` | `glfwGetWindowContentScale` |
| `shouldClose()` / `setShouldClose(value)` | `glfwWindowShouldClose` / `glfwSetWindowShouldClose` |
| `nativeHandle()` | the `GLFWwindow*` escape hatch; never destroy it |

The close flag is set by GLFW only while events are processed (#348).

### Destruction order

Every Window must be destroyed before its Runtime: `glfwTerminate` destroys the
remaining windows underneath their owners. As with Device before Instance, this
is a contract, not a check. `Runtime::liveWindowCount()` makes it observable.
Moving a Runtime into a new object, or moving a Window, keeps ownership and the
count. A Runtime cannot be move-assigned over another one: that would terminate
GLFW underneath the target's live Windows.

## Tests

`platform.runtime` runs on the null backend, so it needs no display. It checks:

- the effective backend and the single-Runtime rule;
- two Windows, with their sizes, framebuffer sizes, content scale, close flags
  and distinct native handles;
- rejected descriptions, which create no native window (`glfwCreateWindow` is
  wrapped and counted);
- `wrong_thread` for every operation from another thread;
- a GLFW error, triggered through the native handle, reaching the sink as a
  `PLATFORM` event;
- an error callback installed earlier still receiving errors and being restored,
  and errors raised during delivery (by that callback or by the sink) being
  delivered once more without recursing;
- moves and the live-window count.

The Vulkan hand-off reports SKIP there, because the null backend names no
surface extensions.

`platform.native_window` uses the automatic backend. Without `DISPLAY` or
`WAYLAND_DISPLAY` it reports SKIP, never PASS. With a display, it also hands the
surface extensions to a Graphics Instance and creates it.
