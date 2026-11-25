# Getting started

The program below brings Vulkan up through Terreate, headless: it asks the loader what
exists, resolves a request against it, creates the Instance, picks a physical device and
creates the Device. It is the file `examples/getting_started/main.cpp`, included here
unchanged. It is compiled and run by the CTest `example.getting_started`, and the
installed-package smoke test builds the same file as an external consumer of the installed
package, so this page cannot drift from code that works.

```{literalinclude} ../examples/getting_started/main.cpp
:language: cpp
:linenos:
```

## Build it

With the repository's development environment (`nix develop`), configure with
`TERREATE_BUILD_EXAMPLES=ON` (the `default` preset does; the examples are built together
with the Platform-Graphics Integration, so Platform, Graphics and the Integration must be on)
and run `just project::build`; the program is `terreate_example_getting_started` in the build
directory. In your own project, link `Terreate::Graphics`:

```cmake
find_package(Terreate REQUIRED COMPONENTS Graphics)
target_link_libraries(app PRIVATE Terreate::Graphics)
```

Without a Vulkan device the program reports `bootstrap=BLOCKED` and fails, like the other
headless tests (the [test matrix](development/graphics-test-matrix.md) explains SKIP and
BLOCKED).

## What to read next

- [Examples](examples.md): a windowed triangle with an application-owned frame loop.
- [Configuration and capability resolution](architecture/configuration-resolution.md): why
  every step above is a request, a plan and an effective result.
- [Errors and results](architecture/error-result.md).
