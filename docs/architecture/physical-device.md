# Graphics Physical-Device Borrow and Conformance Contract

This is the architecture contract for Issue #269's physical-device boundary.
Graphics observes Vulkan physical devices and evaluates explicit requirements;
it does not create a logical device, enable features, allocate queues, or own a
physical device as an application-destroyable resource.

## Library-controlled values

`PhysicalDevice` is a small borrowed view. Vulkan owns the physical device
through its parent `Instance`, and the view never transfers ownership or
provides a destroy operation. The native handle may be observed only while
the parent `Instance` is live, and must never be destroyed by the caller.
Moving an `Instance` moves its stable implementation owner, so views belonging
to the source follow that implementation to the destination. Destroying the
parent, or displacing it by move assignment, ends the borrow; an affected view
must not be used again. The view carries a typed, non-owning `Instance::Impl`
parent identity; that identity is compare-only and is never dereferenced while
checking a borrow.

The default `PhysicalDevice` is an invalid empty view. Its native-handle
constructor is private and the query function is the only library path that
creates a view with a typed, non-owning `Instance::Impl` parent identity.
`PhysicalDeviceCandidate` is likewise constructed only by the query path. It
contains one borrowed view, one owned `PhysicalDeviceCapabilities` snapshot,
and its enumeration provenance. The three members are declared `const`, so the
value is physically immutable after construction, including in copied
candidates. Candidate assignment is deleted; copies and moves construct
another value without exposing replacement or splice operations.

`PhysicalDeviceInventory` has a public default constructor because a
successful query may validly contain zero devices. That constructor produces
only an empty result. Its candidate storage, append operation, and all
candidate observers are library-controlled/read-only; callers cannot inject or
replace a candidate. Capability snapshots are value-owned and pNext-free, but
they remain observations, not implicit feature-enable requests.

## Ordinary borrow and observer lifetime

The API uses ordinary C++ borrow rules rather than hidden ownership. A
reference returned by `device()`, `capabilities()`, or an inventory observer
is valid only while the object that owns the observed value remains alive and
is not replaced. A copied candidate owns an independent copy of its capability
snapshot and a copied borrowed view with the same typed, non-owning parent
identity; neither copy extends the parent `Instance` lifetime. A caller must
reacquire observations after moving or assigning the owning object where the
relevant documentation says the borrow ends.

Casting away const from a candidate observer and writing through it is invalid
and has undefined behavior because the candidate subobjects are physically
const.
To observe a different device or snapshot, issue another query; do not splice
independently obtained values into a candidate.

## Conforming installed-consumer boundary

The construction, read-only candidate-value, and ordinary live-parent borrow
properties above are API guarantees for conforming consumers that compile
against the installed declarations. The public declarations do not expose a valid raw
`PhysicalDevice` constructor, candidate constructor, candidate storage, or
mutable inventory entry. The package smoke tests therefore compile installed
consumer probes that must fail for raw native-handle construction, mutation,
and candidate assignment, alongside a genuine query-and-selection consumer
that must build and run successfully.

Defining or replacing a Terreate-owned non-inline function or member symbol is
outside the supported API and violates the program's ODR contract. Terreate
does not promise to defend that deliberate program violation with static
archive extraction, forced extraction, object co-location, duplicate-
definition checks, linker interposition, or a shared-library policy. In
particular, **static archives do not create a hostile-linker security
boundary**: archive member extraction is a linker selection behavior, not a
security mechanism. The library's claim is about the installed
API and conforming source consumers, not about an adversarial linker or an
ODR-violating replacement build.

## Explicit exclusions

This boundary intentionally does not restore or introduce:

* shared ownership or a hidden lifetime registry for borrowed views; the parent
  `Instance` must remain live while a view is used;
* hidden capability matching or mutable replacement of a candidate's
  value-owned snapshot;
* forced static extraction, co-location of unrelated implementation state,
  duplicate-definition/interposition/linker workarounds, or a DSO policy; or
* logical-device creation, feature enable chains, queue allocation/aliasing,
  surfaces, or resource/VMA ownership (those remain outside #269).

Selection validates the borrowed view and evaluates the candidate's value-owned
snapshot, but it does not claim to make a non-conforming link or hostile
linker safe.

## Format properties (#438)

`<terreate/graphics/format_properties.hpp>` answers "what does this device support for this format?"
without creating anything, so an application chooses a format by its own policy instead of by
trying `createImage` and failing.

- `queryFormatProperties(instance, candidate, format)` makes one native query and returns a
  `FormatProperties` value: the format and its linear-tiling, optimal-tiling and buffer features as
  64-bit `vk::FormatFeatureFlags2`. With Vulkan 1.3 on both the Instance and the device the
  `VkFormatProperties3` flags are used; otherwise the 32-bit flags are widened (their bits keep their
  values), so the same code runs. The 64-bit flags contain every 32-bit bit of the native query and
  may report more. A format the device does not support, `vk::Format::eUndefined` and a value no table
  knows give **empty flags, not an error**.
- `FormatProperties::supports(tiling, required)` and `supportsBuffer(required)` are true when every
  required bit is present (an empty requirement is always satisfied; a tiling other than linear and
  optimal has no features).
- `firstSupportedFormat(instance, candidate, tiling, required, preferences)` tries the application's
  own list in order and returns the first format that supports `required`, or nothing. The library
  chooses no format: the list and the features are the caller's. It makes one query per tried format
  and returns the query's error unchanged.
- The query is per call, not a snapshot on the candidate: the set of formats is open-ended.
- Errors (`FormatPropertiesError`): `invalid_instance`, `device_mismatch` (the candidate is of another
  Instance), `function_unavailable`.

Not covered: image-format limits (maximum extent, mip levels, sample counts: `createImage` still
checks them and reports `unsupported_configuration`), sparse and external-memory properties.

```cpp
constexpr std::array depth_preferences{vk::Format::eD32Sfloat, vk::Format::eX8D24UnormPack32, vk::Format::eD16Unorm};
const auto depth = graphics::firstSupportedFormat(instance, candidate, vk::ImageTiling::eOptimal,
                                                  vk::FormatFeatureFlagBits2::eDepthStencilAttachment,
                                                  depth_preferences);
```
