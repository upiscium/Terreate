# Presentation support and requirements (#364)

`<terreate/graphics/presentation.hpp>` answers, before any Device exists: can
this physical device present to this Surface, with which queue families,
formats and present modes, and what must the Device request to do so. It needs
only the Graphics types (Instance, PhysicalDeviceCandidate, Surface); no
Platform header, no window. Headless Graphics is unaffected, since the header is
optional.

Order: Instance, Surface (#363), PhysicalDevice candidates, **observe**,
**resolve**, **apply** into `DeviceDescription` / `PhysicalDeviceRequirements`,
`resolveDevice`, `createDevice`. Creating the Swapchain is #273.

## Observe

`querySurfaceSupport(instance, candidate, surface)` returns a value-owned
`SurfaceSupport` and keeps no native handle:

| Field | Meaning |
| --- | --- |
| `present_queue_families` | the queue families that can present to the Surface (`index` and native `flags`), ascending |
| `capabilities` | `vk::SurfaceCapabilitiesKHR` (image-count bounds, extents, usage, transforms) |
| `formats`, `present_modes` | what the Surface offers |
| `swapchain_extension_available` | the device lists `VK_KHR_swapchain` |

A device that cannot present is a valid observation, not an error:
`present_queue_families` is empty and the other surface fields stay empty,
because Vulkan allows the capability, format and present-mode queries only for a
supported surface. This is a real case: on a machine whose NVIDIA GPU has no
display attached, a headless surface reports no present support for it, while a
software rasterizer does. The values are point-in-time (`currentExtent` follows
the window), so a Swapchain and its recreation query again.

Errors, before any native call: `invalid_instance`, `parent_mismatch` (the
candidate or the Surface belongs to another Instance, or the Surface was moved
from), then `function_unavailable` if a surface function cannot be loaded. A
native `VkResult` failure (for example `VK_ERROR_SURFACE_LOST_KHR`) is returned
as that native error.

## Resolve

`resolvePresentation(support, request)` is pure (no native call). The request
holds ordered `preferred_formats` (format and color space) and
`preferred_present_modes`, `require_*` flags, the queue `caller_id` (default
`"present"`) and `queue_flags` (default graphics: presenting needs no flag, but
a `DeviceQueueRequest` must name one). `invalid_request` is returned for an empty `queue_caller_id` or `queue_flags`, a `require_*` flag with no preferences, or a preferred format of `VK_FORMAT_UNDEFINED`. Otherwise errors come in this order: `no_present_queue_family`, `swapchain_extension_unavailable`, `no_supported_format`, `no_supported_present_mode`.

| Choice | Policy |
| --- | --- |
| format | the first supported preference; no preference: the first format the Surface lists (`default_policy`); none supported and not required: the same, recorded as `fallback_default`; none supported and required: `no_supported_format`. A Surface listing only `VK_FORMAT_UNDEFINED` accepts any format, so the first preference is used (`any_format_accepted`); with no preference that is `no_supported_format`. |
| present mode | the same shape; the default is FIFO, which Vulkan requires of every Surface that can present. Without FIFO and without a supported preference: `no_supported_present_mode`. |
| queue | `no_present_queue_family` when no family can present, or none that also has `queue_flags`; otherwise a required `DeviceQueueRequest` whose `allowed_family_indices` is exactly the present-capable families that have `queue_flags`, so a mismatch is found here and not later in `resolveDevice`. Alias policy, preferred families and priority stay editable. |
| device extension | `VK_KHR_swapchain` is required; `swapchain_extension_unavailable` when the device lacks it. |

Every choice is a decision record with `requested`, `supported`, `required`, `effective`,
`reason` and the winning `preference_index`; nothing falls back silently.
Image count and extent are deliberately not resolved: they depend on the window
when the Swapchain is created, so they are observed only.

## Apply

`applyPresentation(DeviceDescription&, resolution)` adds `VK_KHR_swapchain`
(deduplicated; an optional entry is promoted to required, since the Device
resolver treats a duplicate as a contradiction) and the queue request. A
`caller_id` already in `queue_requests` is `invalid_request` and nothing
changes. `requirePresentationDevice(PhysicalDeviceRequirements&)` does the same
for physical-device selection. Both edit the application's own structs and
retain nothing.

## Not in this change

Filtering and selecting among several candidates by present support (a helper
over `selectPhysicalDevice`), Swapchain creation and recreation, and
`VK_KHR_get_surface_capabilities2` / extended color spaces.
