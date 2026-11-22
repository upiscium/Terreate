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
| queue | `no_present_queue_family` when no family can present, or none that also has `queue_flags`; otherwise a required `DeviceQueueRequest` whose `allowed_family_indices` is exactly the present-capable families that have `queue_flags`, so a mismatch is found here and not later in `resolveDevice`. Alias policy, preferred families and priority stay editable. The default `forbid_alias` needs a separate queue; when the application's own graphics queue and the present queue can only come from one single-queue family, edit `alias_policy` (and `alias_target`) before `applyPresentation`. |
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

## Select among candidates

`evaluateSurfaceCandidates(instance, candidates, surface, requirements, request)`
evaluates every candidate in the order given and returns one
`SurfaceCandidateEvaluation` each. `requirements` is copied and `VK_KHR_swapchain`
is required on the copy. A candidate that cannot be used is data, not an error,
and the others stay usable. Its `outcome` says why:

| Outcome | Meaning |
| --- | --- |
| `selectable` | meets the device requirements and `resolvePresentation` succeeded; `resolution` is set |
| `requirements_not_met` | fails the device requirements; the Surface is not queried for it |
| `cannot_present` | no queue family can present to the Surface |
| `presentation_unresolved` | it can present but the request cannot be met; `presentation_error` names the step |

`classifySurfaceCandidate(evaluation, support, request)` is the pure core of this
decision (no native call). An invalid `request` is an error for the whole call
and is reported even when every candidate would be declined earlier. A native
failure while querying a candidate (for example `VK_ERROR_SURFACE_LOST_KHR`) also
fails the whole call: the Surface itself is then in doubt.

`selectSurfacePhysicalDevice(instance, candidates, surface, requirements, request, policy)`
runs the evaluation and then the existing `selectPhysicalDevice` over the
selectable candidates only, with the same policy: one selectable candidate is
selected, several need an explicit UUID or UUID order (`ambiguous_match`, never
enumeration order), none is `no_selectable_candidate`. The result keeps every
evaluation; `chosen().resolution` goes to `applyPresentation`. When selection
fails, call `evaluateSurfaceCandidates` to see why each candidate was declined.
Candidates must have distinct `enumerationIndex()` values (one inventory, each
candidate once), else `invalid_request`; the Surface must belong to `instance`,
as for `querySurfaceSupport`.

## Not in this change

Swapchain creation and recreation, and `VK_KHR_get_surface_capabilities2` /
extended color spaces.
