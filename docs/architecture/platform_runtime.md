# Platform Runtime, Window and events (#347, #348)

`<terreate/platform/runtime.hpp>` and `<terreate/platform/window.hpp>` give the
application explicit ownership of GLFW: the library state (`Runtime`) and each
window (`Window`). `<terreate/platform/events.hpp>` defines the events the
application collects when it chooses to (#348). Nothing here creates Vulkan
surfaces (#363) or owns a loop: the application decides when windows exist and
when events are processed.

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
that thread. Every Runtime and Window operation that calls GLFW
(`vulkanSupported`, `requiredInstanceExtensions`, `pollEvents`, `waitEvents`,
`createWindow` and the Window queries) fails with `wrong_thread` on another
thread, before any GLFW call. The one exception is `postEmptyEvent`, which GLFW
allows from any thread. Accessors that only read Terreate's own state (`valid`,
`requested`, `backend`, `liveWindowCount`, `configuration`, `nativeHandle`,
`id`, `correlatedWith`) call no GLFW function and are not checked. Destruction
cannot report errors, so
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

GLFW sets the close flag when the user asks to close the window, which it
learns while events are processed; the same request is also a `CloseRequested`
event.

### Destruction order

Every Window must be destroyed before its Runtime: `glfwTerminate` destroys the
remaining windows underneath their owners. As with Device before Instance, this
is a contract, not a check. `Runtime::liveWindowCount()` makes it observable.
Moving a Runtime into a new object, or moving a Window, keeps ownership and the
count. A Runtime cannot be move-assigned over another one: that would terminate
GLFW underneath the target's live Windows.

## Events (#348)

The application processes events when it chooses to, and reads them as plain
values. Terreate never calls application code from inside GLFW, so no
reentrancy or callback-lifetime rule reaches the application.

| Runtime function | GLFW call |
| --- | --- |
| `pollEvents()` | `glfwPollEvents`: process pending events and return |
| `waitEvents()` | `glfwWaitEvents`: sleep until an event arrives |
| `waitEvents(timeout)` | `glfwWaitEventsTimeout`; GLFW requires a positive finite timeout, so zero, negative or non-finite is `invalid_argument` (use `pollEvents` for zero) |
| `postEmptyEvent()` | `glfwPostEmptyEvent`: wake a waiting thread; allowed from any thread |

`createWindow` registers Terreate's own callbacks on each window, with the
window's internal state as the GLFW user pointer. The callbacks append values
to a store owned by the Runtime. GLFW calls them during poll and wait, and also
synchronously inside some other GLFW calls (for example a native
`glfwSetWindowSize`). Each poll or wait returns everything stored since the
previous one as an `EventBatch`, in arrival order, and empties the store.
If GLFW reports an error during the call, the Error is returned and the
stored events stay for the next call.

Each `WindowEvent` carries a `WindowId` and one value:

- window: `CloseRequested`, `WindowResized` (screen coordinates),
  `FramebufferResized` (pixels), `ContentScaleChanged`, `FocusChanged`,
  `IconifiedChanged`, `MaximizedChanged`, `WindowMoved`, `RefreshRequested`;
- raw input for #296: `KeyInput`, `MouseButtonInput`, `CursorMoved`,
  `CursorEntered`, `Scrolled`;
- text: `CharacterInput`, one Unicode code point. IME composition is #298.

Key, button and modifier values are GLFW's, passed through unchanged
(`GLFW_KEY_*`, `GLFW_MOUSE_BUTTON_*`, `GLFW_MOD_*`); there is no large common
enum and no logical mapping. `InputAction` mirrors GLFW's release, press and
repeat.

`WindowId` is never reused for the life of the process and is never a pointer,
so an event stays meaningful after its Window is destroyed: an event stored
before the destruction is still handed over with that id. Moving a Window keeps
its id.

`EventBatch::dropped` counts events that could not be stored, because of
allocation failure or an action value GLFW does not define. None is lost
silently.

Not handled here: gamepads and joysticks (global, not per window; #296),
monitors, drag and drop, clipboard, cursor modes and IME (#298).

Terreate reserves the user pointer and the callbacks it registers on each
window. Replacing them through native GLFW calls stops Terreate's events for
that window. This is documented, not detected.

## Tests

`platform.runtime` runs on the null backend, so it needs no display. It checks:

- the effective backend and the single-Runtime rule;
- two Windows, with their sizes, framebuffer sizes, content scale, close flags
  and distinct native handles;
- rejected descriptions, which leave no window owned and produce no GLFW error;
- `wrong_thread` for every operation from another thread;
- a GLFW error, triggered through the native handle, reaching the sink as a
  `PLATFORM` event;
- an error callback installed earlier still receiving errors and being restored,
  and errors raised during delivery (by that callback or by the sink) being
  delivered once more without recursing;
- moves and the live-window count.

The Vulkan hand-off reports SKIP there, because the null backend names no
surface extensions.

The test uses only portable observations (results, `liveWindowCount()` and the
diagnostic sink), with no linker-specific interception, so it builds with any
toolchain.

`platform.native_window` uses the automatic backend. Without `DISPLAY` or
`WAYLAND_DISPLAY` it reports SKIP, never PASS. With a display, it also hands the
surface extensions to a Graphics Instance and creates it.

`platform.events` runs on the null backend. GLFW has no API to inject input, so
it uses public GLFW only:

- Window events come from native calls the null backend reports: size,
  position, iconify and restore, maximize, focus.
- Input runs the real translation path. The test reads Terreate's registered
  callback back from the GLFW setter (which returns the previous callback),
  calls it with synthetic values, and restores it.

It checks:

- values, per-window ids and arrival order, and that nothing is handed over
  twice;
- that an undefined action is counted as dropped;
- that Terreate's close callback records each request and leaves the close
  flag as it was. GLFW sets the flag itself before the callback and offers no
  way to simulate a user's close request, so that part is GLFW's;
- `wrong_thread` for poll and wait, and that `postEmptyEvent` works from
  another thread and wakes a wait;
- timed waits, and zero, negative, infinite and NaN timeouts rejected (GLFW
  requires a positive finite timeout);
- ids kept across moves, events of a destroyed Window, and ids never reused.

`platform.native_events` waits for a real resize report on the automatic
backend, and reports SKIP without a display.
