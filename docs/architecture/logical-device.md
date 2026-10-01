# Logical Device and Queue Ownership

Graphics logical-device creation has three explicit stages:

```text
PhysicalDeviceCandidate + DeviceDescription
                  |
                  v
             resolveDevice
                  |
                  v
              DevicePlan
                  |
                  v
             createDevice
                  |
                  v
                Device
                  |
                  v
             borrowed Queue views
```

`resolveDevice` evaluates only the copied capability snapshot.  It does not
query Vulkan, mutate the request or snapshot, create a native object, or use
process-global selection state.  A successful `DevicePlan` owns the request,
support evidence, effective extension/feature values, queue allocations, and
all decisions.  A native failure therefore cannot rewrite or invalidate the
successful plan.

## Ownership boundaries

- `Instance` owns the Vulkan instance and its implementation identity.
- `PhysicalDevice` and `PhysicalDeviceCandidate` are borrowed views plus owned
  capability evidence; they never own or destroy a physical device.
- `Device` is an exclusive RAII owner of one `VkDevice`.  It is move-only and
  never performs an implicit `waitIdle`.  The `Instance` passed to
  `createDevice`, and the parent `Instance` of the selected `PhysicalDevice`,
  are borrowed parents and must outlive `Device`; application destruction
  order is therefore `Device` before `Instance`.
- That lifetime rule is a precondition, not hidden ownership: `Device` and its
  plan retain no `shared_ptr`, owning parent handle, or tombstone for either
  parent.  A selected physical-device view remains a borrowed Vulkan handle
  whose validity is tied to its live parent `Instance`.
- `Queue` is a plain, copyable, non-owning view containing its native handle,
  family/index coordinates, and a private Device-correlation pointer.  It has
  no `shared_ptr`, tombstone, or liveness state.  A view used after its Device
  lifetime is simply invalid by contract; the library does not try to detect
  that misuse.  Queue views follow the Device implementation through a Device
  move and must not be used after the old implementation is destroyed.

The parent identity is checked when applying a plan, so a plan resolved from
one instance cannot be applied to another.  Synthetic capability snapshots
are useful for deterministic resolution tests but intentionally do not
fabricate a native physical-device handle.

Native application also rejects a plan with zero effective queue allocations
before calling Vulkan: `VkDeviceCreateInfo::queueCreateInfoCount` must be
greater than zero.  Optional requests may still be declined in a plan; a
caller that intends to create a native device must provide at least one
effective allocation.

## Queue requests and global resolution

`DeviceDescription::queue_requests` contains one
`DeviceQueueRequest` for each logical queue demand.  Each request has:

- a unique caller ID and explicit `RequirementStrength`;
- required queue flags;
- a strict `allowed_family_indices` whitelist;
- an ordered `preferred_family_indices` list contained in that whitelist; and
- one finite Vulkan priority.

There is no request count, role/name compatibility alias, inferred family, or
hidden allow flag.  An optional request with no usable whitelisted family is
declined and its reason remains observable.  A request with multiple equally
valid families and no ordered preference is ambiguous; required ambiguity is a
resolution failure rather than an enumeration-order winner.

Alias relations are explicit.  A request that aliases names a target caller
ID and selects one of:

- `require_alias`;
- `prefer_alias_with_distinct_fallback`; or
- `prefer_distinct_with_alias_fallback`.

`forbid_alias` is the explicit distinct-only policy.  Aliasing requires the
target allocation to satisfy the request's flags and whitelist and requires
the same native priority.  The resolver searches the complete request graph
in canonical caller-ID order, while honoring alias dependencies, then scores
explicit optional acceptance, alias policy, and ordered family preferences.
The resulting allocation and alias relations are canonicalized by
`(family_index, queue_index)`, so shuffling input requests cannot change the
global solution.  Required distinct requests fail on a one-queue family;
the #228 case of required graphics plus optional compute accepts an explicit
compute-to-graphics alias at family 0, queue 0.

The native plan contains only unique allocations.  It also retains a
request-to-allocation assignment and an alias group for every allocation, so
the provenance of a shared queue is observable before native creation.

## Extension and feature decisions

Only explicit requests become effective.  Every extension and feature
decision exposes `requested`, `supported`, `effective`, `outcome`, `reason`,
and `strength`.  Feature decisions additionally identify their Vulkan
`structure` and `member`.  Unsupported required items fail resolution;
unsupported optional items are declined and retained in the plan.

Vulkan 1.0 through 1.3 request and effective structures are copied into the
plan as owned, pNext-free values.  The input description is never normalized in
place.  `createDevice` constructs its temporary native pNext chain from the
effective snapshot and applies the already-selected extensions, features,
queues, and priorities mechanically; it never re-resolves.

## Scope exclusions

Logical-device creation does not add surface capability queries, swapchains,
submission, resources/VMA ownership, or synchronization policy.  Those remain
outside this component and later work units.
