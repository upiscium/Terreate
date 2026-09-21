# Configuration and Capability Resolution Contract

This document is the architecture contract for Issue #228. It defines a
shared vocabulary and observable resolution model for Terreate domains. It is
intentionally a contract, not a C++ API design: the type shapes and field
names used below are conceptual and non-binding.

The same model must be usable by graphics, and eventually by Audio, Network,
Physics, and other domains. A domain may have different values and rules, but
it must not invent a second meaning for the resolution stages or hide its
defaults in a backend adapter. Related architecture work is tracked by #224
and #227.

## The resolution pipeline

Terreate resolves explicit user intent into backend configuration through three
distinct boundaries, immutable snapshots, and a separate effective value:

```text
context
   |
   v  queryCapabilities(context)
  / \
 v   v
Supported                 Capability Query Failure
(immutable,               (structured query diagnostic;
 successful snapshot)      no Supported snapshot)
   +
Requested (immutable, snapshot-capable)
   |
   v  resolve(Requested, Supported)
  / \
 v   v
Effective                 Resolution Failure
(deterministic             (structured resolution
 pre-native plan)           diagnostic; apply not reached)
   |
   v  apply(Effective)
  / \
 v   v
success                    native/backend application failure
                          (distinct post-resolution boundary)
```

The capability query is an observation of a particular backend, device,
surface, or environment. A successful `queryCapabilities(context)` produces a
`Supported` snapshot; it is not permission to enable everything observed. A
failed query produces a structured `Capability Query Failure` instead of an
empty support set and does not enter resolution. `Resolution` is the
deterministic decision between `Requested` and a successful `Supported`
snapshot. `Effective` is constructed as a separate, deterministic pre-native
plan, and neither input is mutated. Native/backend application is a later
boundary: it consumes `Effective` and reports either success or a distinct
application failure. A native/backend application failure does not change the
earlier resolution result or silently rewrite the effective plan.

## Terminology

| Term | Meaning | Must not be confused with |
| --- | --- | --- |
| **Requirement** | A named item of user intent, such as a capability, value, constraint, or queue role, together with its requested value and a `RequirementStrength`. | A capability that merely appears in `Supported`. |
| **RequirementStrength** | The strength of a `Requirement`: `required` must be satisfied or resolution fails; `optional` may be declined, with the decision and reason recorded. | An implementation priority or an inferred preference. |
| **Requested** | An immutable, snapshot-capable input containing the explicit requirements, context, and policy revision being evaluated. | Mutable caller state or a request reconstructed from backend support. |
| **Supported** | An immutable, snapshot-capable input containing the capability evidence and backend/context identity from a successful capability query for this evaluation. | A `Capability Query Failure` or a promise that every observed capability will be enabled. |
| **Resolution** | The deterministic evaluation of `Requested` against a successful `Supported` snapshot, producing an `Effective` value or a `Resolution Failure`. It ends before native application. | Capability querying, native object creation, or a generic `Result`/`Error` wrapper. |
| **Effective** | A separate output value containing the exact selected values, queue/resource mappings, and decisions that may be handed to native application after successful resolution. It is a deterministic pre-native plan and remains the successful resolution output if later application fails. | An in-place normalization of `Requested`, the support snapshot, a partial plan after a required failure, or the final native/applied state. |
| **Resolution Failure** | A structured error from request validation or resolution that preserves the failed requirement, evidence, policy decision, and resolution-stage detail. It records that native application was not reached. | A `Capability Query Failure`, a native/backend application failure, a missing `Effective` value with only an explanatory log, or an exception/termination policy. |
| **Requested configuration** | The intent explicitly supplied by the user or calling domain. It includes the distinction between required, optional, and unspecified requirements. | A list of everything the backend happens to support. |
| **Supported capabilities** | An immutable capability snapshot successfully returned by a query for a particular backend context. It can include features, extensions, queue families, formats, limits, versions, and other domain facts. | A `Capability Query Failure`, a promise that every capability will be enabled, or a default request. |
| **Effective configuration** | The exact, resolved plan that satisfies the request under the supplied capabilities and documented policy. It contains selected values and decisions, not merely a description of what was possible. It remains the successful resolution output even if a later native call fails to apply it. | The raw request, the full support set, or the final native/applied state. |
| **Effective description** | A human-readable and machine-observable explanation of the effective choices, omitted optional requirements, and defaults used. | An unstructured log message that cannot be correlated with the result. |
| **Required requirement** | A requirement whose absence makes resolution fail. It cannot be silently weakened or replaced. | An optional preference. |
| **Optional requirement** | A requested capability that may be accepted or declined without making resolution fail. Its outcome and reason are still recorded. | An unspecified field. |
| **Unspecified** | No user requirement was declared for that field. A documented default may choose a value only where the domain policy explicitly permits one. | Consent to infer a feature, extension, dependency, or preference. |
| **Default** | A policy value used for an omitted field. A default is provenance-bearing policy, not a hidden capability or implicit dependency. | A reason to override an explicit value. |
| **Capability Query Failure** | A structured failure from `queryCapabilities(context)`. It preserves the query context and available backend/query diagnostics without manufacturing a `Supported` snapshot. It is a distinct pre-resolution boundary. | A `Resolution Failure`, an empty `Supported` set, or a transport-specific `Result`/`Error` type. |
| **Native/backend application failure** | A failure after successful resolution when `apply(Effective)` cannot accept or complete the plan. It is a distinct post-resolution boundary; this contract does not prescribe a shared native failure taxonomy, code set, schema, or hierarchy. | A `Capability Query Failure`, a `Resolution Failure`, or a retroactive change to `Effective`. |
| **Native/applied state** | The final state reported by the backend after it receives the effective plan, including any state or diagnostic available after an application failure. | The resolver's promise that object creation will succeed. |

`Requested` and `Supported` are values, not aliases to mutable caller or
backend state. Snapshot-capable means that the exact inputs used for the
decision can be retained for replay, comparison, and diagnostics. A resolver
may copy or retain those snapshots, but it must not normalize, append to,
remove from, or otherwise mutate either input. `Effective` is always a
separate Effective result; it may contain selected defaults and decisions, but
it never rewrites the inputs from which it was derived.

Every `Resolution` keeps requested, supported, and effective information
distinct. A description may summarize them, but must not collapse them into a
single “configuration” value. Any later native/backend application result is
recorded alongside, not inside, the `Resolution`.

## Invariants

The following invariants apply to every domain resolver.

1. **Explicit intent only.** A resolver does not infer an unrequested meaning,
   feature, extension, dependency, or quality level from hardware support or
   from a conventional backend setup. An available capability remains merely
   available until a declared request or documented policy selects it.
2. **No silent dependency expansion.** A backend prerequisite must be an
   explicit requirement, or a documented domain rule must derive it in a
   traceable decision. If neither is true, resolution rejects the request
   instead of adding the prerequisite invisibly.
3. **Required means required.** An unsupported required requirement, an
   unsatisfiable required combination, or an invalid required value produces a
   structured resolution failure. Falling back to a weaker value is not a
   success unless the request explicitly declared that fallback as an
   allowed alternative.
4. **Optional outcomes are visible.** An optional requirement may be
   accepted or declined, but the result records which outcome occurred and
   why. Optional choices must not displace required choices unless the domain
   policy explicitly says so.
5. **The support snapshot is evidence, not policy.** The resolver uses the
   supplied capability snapshot and does not query mutable global state while
   making a decision. Unknown or failed capability information is not treated
   as support. A failed query produces a `Capability Query Failure`, not an
   empty `Supported` snapshot.
6. **Resolution is deterministic.** Given the same request, capability
   snapshot, context, and policy revision, resolution produces the same
   outcome, selected values, failure details, and canonical ordering. It does
   not depend on time, randomness, process-global state, or incidental native
   enumeration order.
7. **Tie-breakers are declared.** Alternatives have an explicit priority and
   stable tie-breaker. If equally valid alternatives have no defined
   tie-breaker, the resolver reports an ambiguity rather than choosing
   arbitrarily.
8. **Resolution does not apply native state.** Querying and resolving do not
   create or mutate backend objects. Native application is a separate stage
   whose actual result is observable.
 9. **Native/backend application failure is not a Resolution Failure.** If a
    backend cannot apply an `Effective` plan, that failure is reported at the
    separate post-resolution application boundary. The original request,
    support evidence, and successful effective plan remain the resolution
    context; the resolution must not retroactively become a `Resolution
    Failure` or claim that a rejected value was never requested. The contract
    does not prescribe a shared native failure taxonomy.
10. **No partial success for hard requirements.** A diagnostic partial plan
    may be useful for explanation, but it is not an effective configuration
    that consumers may apply when a required requirement failed.
11. **Inputs are immutable and outputs are separate.** `Requested` and
    `Supported` are immutable, snapshot-capable inputs. `resolve` observes the
    supplied snapshots without mutating them or querying mutable global state,
    and constructs a separate `Effective` value on success. A failure also
    retains the unchanged input snapshots; it does not encode a normalized
    request or an altered support set.

## Conceptual resolution contract

The implementation may use different internal representations, but the
observable contract has the following conceptual values and operations. These
labels describe semantic boundaries; they are not proposed public class names,
transport types, or C++ ownership models.

```text
Requested {
    immutable requirement snapshot;
    explicit context;         // e.g. surface or device context
    policy revision;
}

Supported {
    immutable capability snapshot from a successful query;
    backend/context identity;
    query evidence;
}

Capability Query Failure {
    query context and stage;
    backend identity, when known;
    structured reason and available query diagnostics;
    no usable Supported snapshot;
}

Effective {
    exact selected values and allocations;
    requirement decisions;
    defaults and their provenance;
    deterministic pre-native plan;
}

Resolution Failure {
    stable category and failed stage within validation or resolution;
    requirement and requested constraint, when applicable;
    supported evidence, including any fact explicitly unknown in that snapshot;
    resolution-stage detail, including an explicit native-application
    not-reached status;
}

Resolution = Effective OR Resolution Failure
```

The conceptual operations are:

```text
queryCapabilities(context) -> Supported OR Capability Query Failure
resolve(Requested, Supported) -> Effective OR Resolution Failure
apply(Effective) -> success OR native/backend application failure
```

`Capability Query Failure` is a structured semantic record, not an empty
`Supported` value and not a transport-specific `Result`/`Error` type. A failed
query means that `resolve` has no valid support snapshot to consume, so
resolution and native application are not entered. `Resolution Failure`
describes a failure after a valid `Supported` snapshot is supplied and before
an applicable `Effective` value exists. A native/backend application failure
is produced only after successful resolution, retains the successful
pre-native `Effective` plan for correlation, and must not be reduced to a
`Resolution Failure` or rewrite that plan.

The contract deliberately does not define a shared native success/failure
taxonomy, native failure codes, application schema, or outcome hierarchy. The
backend/domain may report the application details appropriate to its native
operation. The shared Core `Error`/`Result` transport and explicit `unwrap`
behavior are provided by #349; this document does not replace those primitives
with a configuration-specific transport or termination policy.

A conforming flow proceeds conceptually as follows:

1. Call `queryCapabilities(context)`. On success, retain one immutable
   `Supported` snapshot with enough backend/context identity and evidence to
   explain what was evaluated. On failure, retain the structured `Capability
   Query Failure`, do not substitute an empty support set, and do not call
   `resolve`.
2. Validate the request and its declared context without adding requirements.
   Malformed, contradictory, or unknown request data is reported as a
   structured `Resolution Failure`.
3. Apply only documented defaults to unspecified fields. Record every default
   value and its policy source or revision.
4. Evaluate required requirements and required combinations. Do not convert a
   missing capability into an optional choice.
5. Resolve declared alternatives using domain policy, explicit priority, and
   stable tie-breakers. A fallback is valid only when the request permits it.
6. Evaluate optional requirements. Record acceptance, decline, conflict, or
   omission for each requested optional item.
7. Construct a separate `Effective` value with exact selected values and a
   description of the decisions. Canonicalize collections for observation;
   canonicalization must not change their domain meaning or mutate either
   input snapshot. This is the complete deterministic pre-native plan.
8. Hand `Effective` to the backend/native layer only after successful
   resolution. This starts a separate application stage. `apply(effective)`
   reports success or a native/backend application failure. Any such failure
   remains post-resolution, retains the `Effective` plan for correlation, and
   does not change the `Resolution` outcome.

The query boundary must never turn “unknown” into “unsupported”: a query
failure is not an empty support set and is not a `Resolution Failure`.

## Capability Query Failure information

`Capability Query Failure` information is part of the architecture contract,
not a string assembled at the outermost call site. A structured query failure
should expose, as applicable:

- the failed `queryCapabilities(context)` operation and the context it was
  observing;
- backend/device/surface identity, when known;
- a stable machine-readable reason or backend/query diagnostic;
- any partial observation, explicitly marked incomplete and unusable as a
  `Supported` snapshot;
- an indication that no valid `Supported` snapshot was produced and that
  resolution/native application were not attempted; and
- a human-readable explanation and, when useful, remediation guidance.

These are semantic observability requirements, not a prescribed transport
type, error wrapper, serialization schema, or exhaustive reason enumeration.
The failure remains distinct from `Resolution Failure` even when the query
failure is caused by a backend or context problem.

## Resolution Failure information

`Resolution Failure` information is part of the contract, not a string
assembled at the outermost call site. A structured resolution error should
expose, as applicable:

- a stable machine-readable category/code, such as invalid request, missing
  required capability, unsatisfiable combination, or ambiguous choice;
- the stage that failed: validation or resolution. Capability querying and
  native application are not resolution stages;
- the requirement identifier and whether it was required or optional;
- the requested value or constraint;
- the relevant supported evidence, or an explicit indication that evidence
  was unavailable;
- the stable reason, conflict set, dependency path, or alternatives
  considered;
- the policy/default revision that affected the decision; and
- the relevant backend/context identity from `Supported`, plus an explicit
  indication that native application was not reached; and
- a human-readable explanation and, when useful, remediation guidance.

Required failures make the `Resolution` unsuccessful and produce no applicable
`Effective` value. An optional requirement decline is a recorded decision in
an otherwise successful resolution, unless it causes a separate required
combination to become impossible. A native/backend application failure occurs
only after successful resolution; it retains the `Effective` plan plus any
backend/native detail available at that later boundary and is never a
`Resolution Failure`.

## Native/backend application boundary

`apply(Effective)` is separate from `Resolution`. It is entered only after a
successful resolution and reports either success or a native/backend
application failure. An application failure does not remove or invalidate
`Effective`, add a native failure category to `Resolution Failure`, or claim
that resolution failed.

The application record or diagnostic should let consumers correlate the
attempt with the original `Requested` and successful `Supported` snapshots,
the exact `Effective` plan handed to the backend, and any native operation,
diagnostic, or applied state that the backend can observe. On success, the
backend owns the meaning of the applied state; on failure, the backend owns
the meaning and detail of the failure. This is an observability requirement,
not a shared native result schema: this contract deliberately defines no
native success/failure taxonomy, fixed code set, field schema, or hierarchy.

## Defaults and observability

Defaults are centralized domain policy. They follow these rules:

- an explicit value always outranks a default; an invalid explicit value
  fails rather than silently falling back;
- a default may fill an omitted choice, but may not create an undeclared
  required feature, extension, dependency, or queue role;
- “best available” is not a default until the domain defines its ordering and
  tie-breaker;
- default selection is deterministic and independent of driver enumeration
  order; and
- the `Effective` value identifies the field as defaulted and records the
  policy source or revision.

At minimum, a diagnostic or resolution record must make these items
observable together:

1. the original requested configuration;
2. the successful supported capability snapshot or a stable reference to its
   evidence, or the structured `Capability Query Failure` when querying did
   not produce a snapshot;
3. the effective configuration and description when resolution succeeds;
4. the structured `Resolution Failure` when resolution does not produce an
   `Effective` value, including its failed stage and native-not-reached status;
5. every rejected required requirement and its reason;
6. every optional requirement and whether it was accepted or declined;
7. each defaulted field and its provenance; and
8. whether native application was attempted and, if so, whether it succeeded
   or produced a native/backend application failure. If it failed, the record
   should retain enough correlation to the successful `Effective` plan and
   available backend/native detail; the contract does not prescribe a native
   outcome taxonomy or transport schema.

Machine-readable resolution decisions and stable resolution reasons are
required for tests and diagnostics. Human-readable descriptions complement
them; logs alone are not the contract. Backend-native diagnostics remain
backend-specific. The record should use stable ordering so that two equivalent
resolutions can be compared without depending on container or driver order.

## Ownership boundaries

### Core ownership

Terreate Core owns the shared semantics of the contract: the distinction
between requested/supported/effective, required and optional outcomes,
structured failure and decision vocabulary, provenance conventions, and the
determinism/observability invariants. The shared `Error`/`Result` primitives
from #349 may carry failures at an implementation boundary, but this document
does not prescribe a configuration-specific transport API.

Core does **not** own Vulkan feature names, queue-family policy, audio codec
preferences, network transport policy, or any other domain-specific default.
It must not turn backend capabilities into user intent.

### Domain ownership

Each domain owns its intent vocabulary, capability adapter, legal combinations,
default policy, alternative priorities, tie-breakers, and mapping from its
effective configuration to a backend request. The domain must expose enough
decision information for the shared contract to remain observable. Domain
policy may derive a dependency only when that rule is explicit, stable, and
recorded in the decision trace.

### Backend/native ownership

The backend adapter owns querying actual capabilities, translating an
effective plan into native parameters, performing native creation, and
reporting the success or failure of the native/backend application with the
final native/applied state and native details it can observe. A failed query
produces a structured `Capability Query Failure`; it must not be represented
as an empty support set. The adapter does not own user-facing defaults or
silently repair a missing requirement. If it cannot apply a plan, it must not
quietly replace the plan or report the application failure as a `Resolution
Failure`. The shared contract does not prescribe the native failure's codes,
schema, or hierarchy.

This boundary lets future Audio, Network, and Physics adapters share the
contract without making Core know their native concepts, and prevents each
module from accumulating incompatible resolution rules.

## Usability follow-through

The contract is intended to be usable by the downstream work in
[#268](https://github.com/upiscium/Terreate/issues/268),
[#269](https://github.com/upiscium/Terreate/issues/269), and
[#270](https://github.com/upiscium/Terreate/issues/270), not just by an
internal resolver. Those consumers must
keep the same observable boundaries:

- **#268** must be able to show the caller's immutable `Requested` snapshot
  and the `Effective` choices without presenting backend support as user
  intent.
- **#269** must be able to explain optional acceptance or decline,
  defaults, and queue/resource decisions from the `Resolution` record rather
  than from unstructured logs.
- **#270** must be able to surface either a `Resolution Failure` with its
  failed requirement, retained `Supported` evidence, and explicit native-not-
  reached status; a `Capability Query Failure` when no support snapshot could
  be produced; or a native/backend application failure with the successful
  `Effective` plan and backend/native detail. These boundaries must let a user
  distinguish an unsupported requirement from a query failure and from a
  post-resolution application failure.

These links are usability obligations: the three consumers may choose their
own presentation and API shapes, but they must not mutate the inputs, hide an
optional decline, or invent a second resolution vocabulary.

## Vulkan examples

The following examples illustrate the contract only. They are not a promise
of exact defaults, type names, or Vulkan implementation APIs.

### Vulkan Instance

An application may explicitly request a minimum API version, required instance
extensions for an explicitly supplied surface/window context, and optional
validation layers or diagnostic extensions. The instance capability query can
report the loader API version, available instance extensions and layers, and
context-specific surface integration facts. It must not infer a window-system
requirement from an unrelated host property or enable every reported
diagnostic extension.

If `queryCapabilities(context)` cannot obtain a trustworthy loader or
context-specific snapshot, it returns a structured `Capability Query Failure`.
That failure is not an empty `Supported` value or a `Resolution Failure`, and
`resolve` is not called for that attempt.

#### Vulkan Instance success

For a concrete success case, let the immutable `Requested` snapshot contain:

- API version `VK_API_VERSION_1_3` as a required minimum;
- required `VK_KHR_surface` and `VK_KHR_xcb_surface` for an explicitly
  supplied XCB surface context; and
- optional `VK_LAYER_KHRONOS_validation`.

Let the immutable `Supported` snapshot report loader API 1.3 or newer, both
required extensions, and the optional validation layer. `resolve(requested,
supported)` returns a separate `Effective` value containing exactly API 1.3,
the two required extensions, and the selected validation layer. The
`Resolution` records that all required requirements were satisfied and that
the optional layer was accepted. Native creation receives only that
`Effective` value; if the backend accepts it, `apply` reports success. Neither
input snapshot is modified. If native creation cannot apply it, `apply`
reports a distinct native/backend application failure.

#### Vulkan Instance unsupported-required failure

With the same request, suppose `Supported` reports the loader API and
`VK_KHR_surface` but not `VK_KHR_xcb_surface`. `resolve(requested, supported)`
returns no applicable `Effective` value and instead returns a
`Resolution Failure` identifying `VK_KHR_xcb_surface` as a missing `required`
`Requirement`, retaining the supported extension evidence and the backend
identity. The supported evidence says that the required extension was not
reported by the loader for the supplied context, and native instance creation
was not attempted. The failure must not silently drop the extension, weaken it
to optional, or mutate `Requested`.

If native instance creation rejects a successfully resolved plan, that native
failure is distinct from “the extension was not requested” or “the loader did
not report it.” The successful `Resolution` remains `Effective`, and the
application stage reports a distinct native/backend application failure,
retaining the `Requested` and `Supported` snapshots, the `Effective` plan, the
final native-enabled state when available, and the native diagnostic. It is not
a diagnostic `Resolution Failure`.

### Vulkan Device

An application may explicitly request a presentation-capable device with:

- required device extensions and features;
- optional features or a preference for a dedicated compute queue;
- required queue roles such as graphics, compute, and present; and
- an allowed set or preference ordering for surface formats and present
  modes.

The device capability query can report physical-device features and limits,
extension support, queue-family properties and counts, per-surface queue
support, and compatible formats and present modes. A supported feature is not
enabled unless the request or documented policy selects it.

Resolution rejects a missing required feature, extension, queue role, or
compatible surface choice with structured evidence. It may drop an optional
feature or preference and explain why. The effective device plan records the
exact feature/extension selections, format and present-mode choice, and a
queue plan. The native boundary then reports success or a distinct
native/backend application failure with the final enabled feature set, created
queues, and any native diagnostic available from that boundary.

#### Device required feature A with optional feature B declined observably

For a concrete feature case, let `Requested` require feature A
(`samplerAnisotropy`) and optionally request feature B (`shaderInt64`). Let
`Supported` report feature A as supported and feature B as unsupported. The
successful `Resolution` returns an `Effective` device plan that enables A and
does not enable B. It also contains an observable optional decision for B:
`declined`, reason `unsupported`, with the supporting feature evidence. This
is a resolution-level decline, not a native/backend application failure. It is
not a missing field or a silent fallback; callers and diagnostics can inspect
that B was requested, declined during resolution, and why. Feature A remains
required, and the input snapshots remain unchanged.

#### Queue-role aliasing

Queue roles are semantic demands; a queue family and queue handle are native
resources. A graphics role and a present role may be served by the same queue
when the queried family supports both and the declared policy permits
aliasing. That is an explicit resolution decision, not an assumption caused
by seeing the same family index twice.

The effective queue plan therefore records, conceptually:

- each requested role;
- the selected family for that role;
- the unique queue allocation(s); and
- an alias group or equivalent relation showing which roles share one
  allocation.

If the request requires distinct queues, aliasing is forbidden and an
insufficient queue count is a required failure. If aliasing is allowed, the
resolver still checks the domain's synchronization and ownership constraints;
it must not alias merely to make an unsatisfied request appear to fit. A
preference for a dedicated family may fall back only when that fallback was
declared optional and the decision is recorded. Family selection and queue
allocation use explicit priorities and stable tie-breakers rather than
incidental native enumeration order.

The native plan should contain only unique queue allocations while retaining
the role-to-allocation mapping for diagnostics. This makes aliasing visible
both before native creation and in the final applied-state report.

#### Graphics plus optional compute aliasing family 0 queue 0

For a concrete aliasing case, let `Requested` require a graphics role and
optionally request a compute role, with aliasing permitted and a distinct
compute queue not required. Let `Supported` report queue family 0 as supporting
both graphics and compute with one available queue. The `Effective` queue plan
must explicitly map **graphics -> family 0, queue 0** and **optional compute ->
family 0, queue 0**, with one alias group containing both roles. Thus graphics
plus optional compute explicitly aliases family 0 queue 0; it is not an
accidental reuse of a family index. The mandated case is: graphics plus
optional compute explicitly aliasing family 0 queue 0 under the declared
policy. The observable optional decision is
`accepted by aliasing`, and the native plan contains one unique queue
allocation plus both role mappings. If the request instead requires distinct
queues, this same support snapshot produces a required insufficient-queue
`Resolution Failure`.

## Mandated downstream implementation and test cases

Before claiming a resolver is complete, downstream consumers must cover each
of these exact cases:

1. **Immutable snapshots and a separate output:** pass snapshot-capable
   `Requested` and `Supported` values to `resolve(requested, supported)`, prove
   that neither input changes, and inspect a separately constructed `Effective`
   value on success.
2. **Deterministic replay:** resolving identical request and support snapshots
   twice produces identical `Effective` values, decisions, `Resolution Failure`
   details, and canonical ordering; changing only capability evidence changes
   the outcome only through a documented rule.
3. **No inferred enablement:** an unrequested supported feature, extension,
   dependency, or queue role is not enabled, and a required backend prerequisite
   is never silently added.
4. **Vulkan Instance success:** with API 1.3, required
   `VK_KHR_surface`/`VK_KHR_xcb_surface`, and optional validation support in
   `Supported`, the required extensions and optional layer are reflected in
   `Effective` and the accepted optional decision is observable.
5. **Vulkan Instance unsupported-required failure:** when
   `VK_KHR_xcb_surface` is absent, `resolve` returns no `Effective` value and a
   structured `Resolution Failure` naming that required requirement, retaining
   support evidence and backend/context identity, with native application not
   reached and without mutating `Requested`.
6. **Required failures and query failures:** invalid input, missing required
   capability, unsatisfiable required combinations, and ambiguity each produce
   a structured `Resolution Failure`; a failed capability query produces a
   structured `Capability Query Failure` instead. Both preserve useful
   diagnostics at their own boundary, and a failed query never substitutes an
   empty support set for unknown evidence.
7. **Optional decisions:** an optional requirement is tested once when
   accepted and once when declined, with the outcome and reason observable in
   the successful `Resolution` unless a required combination is thereby made
   impossible.
8. **Vulkan Device feature A/B:** with required feature A
   (`samplerAnisotropy`) supported and optional feature B (`shaderInt64`)
   unsupported, `Effective` enables A, omits B, and observably records B as
   declined during resolution for the unsupported reason; this is not a
   native/backend application failure.
9. **Defaults and alternatives:** explicit values beat defaults; omitted values
   use only documented defaults with provenance; declared priorities and stable
   tie-breakers decide alternatives, while an equal-priority case with no
   tie-breaker reports ambiguity rather than choosing arbitrarily.
10. **Backend/native divergence:** a backend rejection after successful
     resolution is reported as a distinct native/backend application failure,
     not a `Resolution Failure`. It retains the original `Requested` and
     `Supported` snapshots, the successful `Effective` plan, and the
     backend/native error detail available at that boundary. The resolution
     remains `Effective` and is not rewritten as an unrequested value.
11. **Vulkan Device selection:** required and optional device extensions,
    feature support, surface format and present-mode choices, queue-family
    constraints, required versus optional dedicated queues, and insufficient
    queue counts are each observable in the resolution decision.
12. **Explicit queue aliasing:** when family 0 is the only family with one
    queue supporting graphics and compute, a required graphics role plus
    optional compute explicitly maps both roles to **family 0, queue 0** and
    records one alias group; a request for distinct queues fails instead.
13. **Usability consumers:** #268, #269, and #270 can separately expose
     `Requested`, `Supported`, `Effective`, optional decisions, structured
     `Capability Query Failure` query detail, `Resolution Failure` resolution
     detail, and native/backend application failure detail without copying
     graphics-only policy into another domain.
14. **Cross-domain reuse:** a second domain reuses the shared
     `Requirement`/`RequirementStrength`/`Resolution`/`Resolution Failure`
     semantics, the separate capability-query boundary, and the separate
     native application boundary without copying a graphics-specific default
     or capability rule.

## Boundary with #349

Issue #228 defines the resolution-stage architecture. #349 provides the shared,
dependency-free Core `Error`/`Result`/`unwrap` primitives that callers may use
to transport and handle failures from `queryCapabilities(context)`, `resolve`,
and `apply`. Those transport primitives do not collapse a `Capability Query
Failure` into a `Resolution Failure`, and a native/backend application failure
still follows a successful `Resolution` rather than becoming a `Resolution
Failure`. The configuration contract remains responsible for those semantic
stage distinctions; the Core error-result contract defines the transport and
explicit fail-fast behavior.

#349 is not the implementation issue for Vulkan resolvers. Vulkan resolver
algorithms, backend adapters, native calls, and their production handlers are
outside this document and outside Issue #228; they must not be smuggled into
#349 transport primitives by treating a missing `Effective` value as an API
design.

## Strict exclusions

This contract deliberately does **not**:

- define production C++ classes, function signatures, public ABI, or exact
  serialization schema;
- implement resolver algorithms, backend adapters, native Vulkan handlers,
  device creation, queue creation, or synchronization;
- choose final product defaults, feature sets, extension sets, or queue
  priorities for a particular deployment;
- change CMake targets, build architecture, package exports, linkage, or
  dependency discovery;
- change `flake.nix`/`flake.lock`, Automation Core, or repository workflow
  configuration; or
- replace the ownership and scope of #224, #227, #349's Core primitives, or any
  future domain implementation issue.

This is documentation-only scope: it does not modify production files,
Automation files, or native Vulkan behavior. The `Result`/`Error`/`unwrap`/
termination model is defined by #349; this document only describes how those
transport primitives relate to resolution boundaries.

The purpose of Issue #228 is to make later implementations agree on what is
requested, what is supported, what became effective, why a requirement was
rejected during resolution, and whether the backend application succeeded or
failed after resolution—without prematurely committing the repository to a
production API.
