# Architecture

This section states the stable contracts of the library. It is not a copy of the
Issues that produced them: the Issues track live work, these pages say what holds.

## Invariants

- The application owns `main()`, the application loop, frame scheduling and update
  timing. Terreate supplies mechanisms and explicit conveniences; it does not own
  application semantics.
- Automation only resolves a user-declared intent into backend objects. Defaults are
  documented, inspectable, overridable and predictable.
- Requested, supported and effective state are observable wherever a domain needs them.
- An owning resource is a move-only RAII value; a borrow is a non-owning view or handle.
- C++ object lifetime and GPU execution lifetime are different: destroying a C++ object
  never waits for the GPU, and the application proves completion before destruction.
- There is no hidden global singleton, service locator or application scheduler.
- The native/backend API stays reachable (native handles, escape hatches).
- A consumer that does not use an optional capability does not receive its dependency.

## Components and categories

One repository, one coordinated version, many independently selectable components.
The repository is a monorepo, not a monolith: there is no single large `Terreate`
target that gathers every optional dependency.

| Category | Meaning | Today |
| --- | --- | --- |
| Core | The minimum contract every component needs: `Result`/`Error`, diagnostics, configuration pattern. Depends on nothing above it. | `Terreate::Core` |
| Module | A domain capability. It need not be backend-neutral: Graphics states Vulkan as its backend. | `Terreate::Asset` (CPU-only), `Terreate::Platform` (GLFW), `Terreate::Graphics` (Vulkan) |
| Adapter | Connects an external backend to an existing Module contract. A Module never depends on an Adapter. | none yet |
| Integration | Optional glue that connects the public APIs of several components. | `Terreate::IntegrationPlatformGraphics` |
| Extension | An optional abstraction or development utility on top of existing components. The lower Modules stay fully usable without it. | none yet |

Dependency direction:

```text
              Integration / Extension
                 ↓        ↓
Adapter -----------> Module
                       ↓
                      Core
```

- Core never depends on a higher category; a Module never depends on an Adapter,
  Integration or Extension; Modules have no hidden semantic dependency on each other.
- An Adapter depends on its contract and its backend; an Integration depends only on the
  public APIs it connects; an Extension depends only on the public APIs it needs.
- A direct Module-to-Module dependency is allowed only with a reason that makes one the
  domain's lower layer.
- No component reads another component's private or internal headers.

What the build enforces: the options (a contradictory request is a configure error, and
the Integration needs Platform and Graphics), the `find_package` diagnostic for a disabled
component, and that documentation tooling is never exported. The dependency-direction
rules above and the private-header rule are review conventions: nothing in the repository
checks them yet.

## Public and internal boundary

```text
modules/<name>/
├── include/terreate/<name>/    public surface
└── src/                        private implementation
```

The API reference is generated from the `include/` trees only.

## Build, package and options

- Targets are per component: `Terreate::Core`, `Terreate::Asset`, `Terreate::Platform`,
  `Terreate::Graphics`, `Terreate::IntegrationPlatformGraphics`. A consumer writes
  `find_package(Terreate REQUIRED COMPONENTS Graphics)` and links the targets it needs.
- The source tree and the install tree expose the same public target names.
- Options select components (`TERREATE_BUILD_*`). Contradictory options are a configure
  error with an explanation, never a silent correction.
- Requesting a disabled component through `find_package` gives an explicit diagnostic.
- "Do not leak the backend" means: public headers do not expose unneeded backend types,
  and a consumer of an unused component is not asked for its backend. Link items that
  static or shared linkage really needs still propagate as correct CMake usage
  requirements.
- Documentation tooling (Doxygen, Sphinx) is never a dependency of the package.

`TERREATE_BUILD_*` also names the non-component switches `TERREATE_BUILD_EXAMPLES` and
`TERREATE_BUILD_DOCS`.

## Component contracts

The pages below fix the contract of each part that exists. The Platform-Graphics
Integration's contract is in [Surface ownership and the Window Integration](surface.md);
the other pages follow the topic they name and predate the
[component template](../development/component-template.md), which new components follow. Start with the cross-cutting
ones: [configuration](configuration-resolution.md), [errors](error-result.md),
[diagnostics](diagnostics.md).

```{toctree}
:maxdepth: 1
:glob:

configuration-resolution
error-result
diagnostics
platform_runtime
*
```
