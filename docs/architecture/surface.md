# Vulkan Surface ownership and the Window Integration (#363)

`<terreate/graphics/surface.hpp>` gives the application explicit ownership of a
`VkSurfaceKHR`. It knows nothing about windows: no Platform header, no GLFW. A
Surface is created through a caller-supplied function, so any window system, or
none, can provide it. Making one from a Platform `Window` is a separate
Integration (`Terreate::IntegrationPlatformGraphics`).

A Surface needs no logical Device or Queue. It can be created, and queried
through the physical-device surface functions, before a Device exists, which is
the order presentation needs: Instance, Surface, PhysicalDevice selection,
Device.

Headless Graphics is unaffected. `Surface` is optional and adds no link
dependency: the Vulkan loader already exports `vkDestroySurfaceKHR`. A headless
consumer never includes this header.

## API

| Function | What it does |
| --- | --- |
| `createSurface(instance, creator)` | calls `creator.create` once and takes ownership of the surface it returns |
| `adoptSurface(instance, handle, destroyer)` | takes ownership of a surface created elsewhere |

Both fail before any native call with:

| Error | When |
| --- | --- |
| `invalid_instance` | a moved-from or plan-less Instance |
| `surface_extension_not_enabled` | the Instance plan did not enable `VK_KHR_surface`. There is no silent no-op: the application enables it in the Instance request. |
| `invalid_creator` | `creator.create` is null |
| `invalid_surface` | the handle to adopt is null, or the creator returned `VK_SUCCESS` with a null handle |

A native `VkResult` failure from the creator is returned as that native error.
`function_unavailable` applies only when the default destroyer is used: it means
`vkDestroySurfaceKHR` could not be loaded through the Instance, so a custom
`SurfaceDestroyer` bypasses it. `allocation_failed` covers memory. All of these
checks, and the creator check, run before any native call; the loader lookup
comes last, after `invalid_creator`.

### `SurfaceCreator`: the escape hatch

```cpp
struct SurfaceCreator {
  VkResult (*create)(VkInstance, VkSurfaceKHR *, void *user);
  void *user;
  SurfaceDestroyer destroyer;   // optional
};
```

`create` is how the surface is made: `vkCreateXcbSurfaceKHR`,
`glfwCreateWindowSurface`, `vkCreateHeadlessSurfaceEXT`, or anything else. It is
called once, synchronously. Graphics wraps the result at once, so a raw handle
cannot leak between creation and ownership. A failing creator must leave no
surface behind, and a successful one must return a non-null handle.

### `SurfaceDestroyer`

By default a Surface is destroyed with `vkDestroySurfaceKHR(instance, surface,
nullptr)`, so a surface made with a custom allocator needs its own destroyer. A
backend whose surface owns more than the `VkSurfaceKHR` supplies one too. The
function must destroy the surface, and its `user` pointer must stay valid until
the Surface is destroyed.

## Surface

`Surface` is a move-only owner.

- `nativeHandle()` / `nativeSurface()` and `nativeInstance()` are the native
  escape hatches. The handle is borrowed; never destroy it.
- `correlatedWith(instance)` compares native `VkInstance` handles, like every
  Graphics `correlatedWith`.
- Moving transfers ownership and destroys nothing. Move assignment destroys the
  surface the target owned, exactly once.

### Destruction order

A Surface must be destroyed before its Instance, before the native window it was
created for, and after every `VkSwapchainKHR` created for it has been destroyed
(`VUID-vkDestroySurfaceKHR-surface-01266`). As with Device before Instance, this
is a contract, not a check, and the Instance is borrowed, never retained. The
Swapchain (#273) is what will hold a Surface; it will state the same order from
its side.

## Creating a Surface from a Window: the Integration

`<terreate/integration/platform_graphics/surface.hpp>` is the one place Platform
and Graphics meet. It is the repository's first Integration (#227): optional
glue that connects two components through their public APIs only. The target is
`Terreate::IntegrationPlatformGraphics`, in `integrations/platform-graphics/`.
Platform stays Vulkan-free, Graphics knows nothing of windows, and neither
depends on the Integration.

```cpp
auto surface = terreate::integration::platform_graphics::createWindowSurface(
    instance, runtime, window);
```

It wraps `glfwCreateWindowSurface` as a `SurfaceCreator`, so the result is an
ordinary `graphics::Surface` with the ownership rules above. No Device or Queue
is needed. Every check runs before the native call, in this order:

| Error | When |
| --- | --- |
| `parent_mismatch` | the Window was not created by this Runtime, or one is moved-from |
| Platform `wrong_thread` | called on a thread other than the Runtime's; the thread rule comes from `runtime.requiredInstanceExtensions()` and is returned unchanged |
| Platform `vulkan_unavailable` | GLFW names no surface extensions, as on the null backend: an explicit error, never a silent success |
| `required_extension_not_enabled` | the Instance did not enable an extension GLFW needs; the Error's detail names it |
| Graphics errors | an invalid Instance, `VK_KHR_surface` not enabled |

A native `VkResult` failure from GLFW is returned as that native error, and GLFW's
own errors also reach the Runtime's diagnostic sink. The application passes
`runtime.requiredInstanceExtensions()` to `InstanceDescription::required_extensions`
when it creates the Instance, before the Surface:

```cpp
auto extensions = runtime->requiredInstanceExtensions();
description.required_extensions = *extensions;
```

The `Runtime` argument is explicit because a Window does not expose its Runtime.
It also lets the Integration use Platform's thread-checked public API instead of
anything private.

### Destruction order

The Surface is destroyed before the Window, before the Instance, and after every
swapchain created for it. Nothing checks this. The Window is borrowed only during
the call.

### Build and package

- Option `TERREATE_BUILD_INTEGRATION_PLATFORM_GRAPHICS`: on by default only when
  Platform and Graphics are on. Turning it on while either is off is a configure
  error that names the missing option; the component is never enabled or
  disabled behind the application's back (#227).
- Package component `IntegrationPlatformGraphics`. Requesting it also imports
  Platform and Graphics and discovers GLFW and Vulkan, with the same
  required-or-optional strength as the request itself. Core-only, Platform-only
  and Graphics-only installs have no Integration, and requesting it there gives
  the usual disabled-component diagnostic naming
  `TERREATE_BUILD_INTEGRATION_PLATFORM_GRAPHICS=ON`.

## Tests

`graphics.surface` needs no window system. It makes surfaces with
`VK_EXT_headless_surface` through the creator, and reports SKIP, never PASS,
when that extension is unavailable. It checks:

- an Instance without `VK_KHR_surface` is refused, with the creator never
  called, by `createSurface` and `adoptSurface`;
- a null creator, a native creator failure (returned as that native error, with
  nothing destroyed) and a null handle;
- a successful creation: one creator call with the Instance's native handle, the
  native handles, correlation, and moves;
- exact destruction counts. The test supplies the destroyer, counts, and
  forwards to `vkDestroySurfaceKHR`, so no linker interception is needed: a move
  destroys nothing, move assignment destroys the old surface once, and leaving
  scope destroys the rest once;
- `adoptSurface` with and without a destroyer;
- a physical-device surface query on the Surface with no Device created;
- a clean run under the validation layer.

The package smoke checks the installed header and the explicit
`surface_extension_not_enabled` error from an installed consumer.

`integration.platform_graphics` runs on GLFW's null backend, which names no
surface extensions, so it checks the explicit paths: `vulkan_unavailable`, the
thread rule (`wrong_thread` from another thread), and `parent_mismatch` for a
moved-from Window or Runtime, including that this check comes first.
`integration.native_platform_graphics` needs a display, and reports SKIP, never
PASS, without one or when the Vulkan loader lacks GLFW's extensions. With a
display it checks that an Instance without GLFW's extensions is refused with the
missing name, then creates a real Surface from a real Window before any Device
exists, and destroys it before the Window and Instance under the validation
layer. Neither test uses linker interception.

Not part of this issue: present support, formats and capabilities (#364) and the
Swapchain (#273 onward). The Surface type is shaped for them: they take
`const Surface &`.
