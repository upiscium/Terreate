# Graphics validation and integration test matrix

This page records how Terreate's Graphics tests are classified, what each class needs, what the outcome words mean, and how the validation layer is treated. It describes the tests as registered in `CMakeLists.txt`; it does not add behaviour. The rule that matters most: **an environment that lacks something a test needs is reported as SKIP or BLOCKED, and is never counted as a pass.**

## Test classes

| Class | Needs | Registered as | Examples |
| --- | --- | --- | --- |
| CPU | the compiler and linker; no device. Some use GLFW's null backend (`platform.runtime`, `platform.events`, `integration.platform_graphics`). Several tests link with GNU-ld `-Wl,--wrap`, so they need a GNU-compatible linker | `.cpu` suffix, `graphics.spirv_binary`, core and smoke tests, the null-backend tests | `core.error_result`, `graphics.pipeline.cpu`, `graphics.swapchain.cpu` |
| Headless GPU | a Vulkan 1.3 device (the dev shell's Mesa software device is enough) and the validation layer | `graphics.*` without a window | `graphics.buffer_roundtrip`, `graphics.headless_triangle`, `graphics.synchronization` |
| Window / presentation | a display (X11 or Wayland) and a Vulkan device that can present to it | tests that print `…=SKIP` without a display | `platform.native_events`, `platform.native_window`, `integration.native_platform_graphics`, `integration.two_window_swapchain` |
| Example | as the window class | scripted run of an example | `example.triangle` |
| Package and CMake | CMake, a temporary install prefix | `cmake.*` | `cmake.component_matrix`, `cmake.package_smoke` |

Notes:

- `graphics.surface`, `graphics.presentation` and the `graphics.swapchain*` tests do not open a window: they use `VK_EXT_headless_surface` (SKIP if the device lacks it) and ran in a no-display run, so they belong to the headless class. A display adds the window class above, which is the only coverage of a real window system and compositor.
- `graphics.headless_triangle` is the offscreen end-to-end test (Shader, Pipeline, Command, Submission, readback) and needs no window.
- `graphics.synchronization`, `graphics.swapchain_*` and `example.triangle` additionally need the layer: without it they fail (see below).
- Some headless tests depend on a capability, not on the environment: timeline semaphores (`graphics.submission.timeline`, `graphics.retirement.timeline`), two distinct queues (`graphics.retirement.multiqueue`), an image format feature (`graphics.image`), `VK_EXT_debug_utils` (`graphics.debug_utils`).

## Outcome words

| Word | Meaning | Gate result |
| --- | --- | --- |
| PASS | The test ran its checks and they held. The test prints its own PASS line. | passes |
| SKIP | The environment lacks an optional thing the test is allowed to need (a display, a capability). The test prints why. | shown as *Skipped*, not as passed |
| BLOCKED | A prerequisite the test requires is missing (for example no usable device, or no layer where the test claims validation). | fails |
| FAIL | A check did not hold. | fails |

How CTest sees them. The tests use three mechanisms; which one decides depends on the test:

1. `PASS_REGULAR_EXPRESSION`: the test passes only if its own PASS line is printed, and CTest then ignores the exit code. A test with this and no SKIP mechanism fails on a SKIP or BLOCKED line (no match). A test that prints PASS and then exits non-zero still passes unless a `FAIL_REGULAR_EXPRESSION` matches.
2. `SKIP_REGULAR_EXPRESSION` or `SKIP_RETURN_CODE 77`: a matching SKIP is reported as *Skipped* and takes precedence over the PASS check. A skipped test is never summed into passed.
3. `FAIL_REGULAR_EXPRESSION`: `assertion=FAIL` and `BLOCKED` lines fail the test even if the exit code is 0. Some tests also put `validation-layer=SKIP` here (see below).

`tests/cmake/test_outcomes.cmake` (the `cmake.test_outcomes` test) enforces the rule: every test has a `PASS_REGULAR_EXPRESSION` unless it is listed there as exit-code-only (CPU and CMake-driven tests), and a test that can SKIP must have one. Registering a native test without a PASS line fails the gate.

## Validation layer policy

The validation-aware tests (`graphics.headless_triangle`, `graphics.swapchain_frame`, `graphics.synchronization`, `example.triangle`, and others that print `validation-layer=PASS`) count messages of severity warning or error from the layer through the diagnostics sink, and any such message fails the test. In `example.triangle` the sink is checked after the Terreate objects are destroyed, so teardown is covered; this has not been verified for every other test.

When the layer is missing there are two tiers today:

- **Required**: `graphics.descriptor_set_layout`, `graphics.descriptor_pool`, `graphics.descriptor_write` need `validation-layer=PASS` in their PASS line; `graphics.swapchain_frame`, `graphics.swapchain_rendering`, `graphics.swapchain_recreation`, `graphics.swapchain_retirement`, `graphics.synchronization` fail on `validation-layer=SKIP`; `example.triangle` runs with `--require-validation`. Without the layer these fail.
- **Optional**: other headless tests print `validation-layer=SKIP reason=…` when the layer is unavailable and can still pass on their functional check. A layer-less run therefore proves less than a run with the layer; the output says so.

The dev shell provides the layer (`VK_LAYER_PATH` is exported), so the gate runs the strict form, but nothing in the gate checks that the layer was present: for the optional tier a missing layer shows only as `validation-layer=SKIP` in the output. Moving the optional tests to the required tier is a separate decision.

## Where tests run

There is no hosted CI configuration in the repository. The quality gate is `nix develop --command just project::check` (format, lint, build, all CTests). The dev shell provides the Vulkan loader, the pinned validation layers and a Mesa software device, so headless GPU tests run there without hardware.

| Environment | Runs | SKIPs |
| --- | --- | --- |
| Dev shell, no display | CPU, headless GPU (including `graphics.surface` and `graphics.swapchain*`), package | `platform.native_events`, `platform.native_window`, `integration.native_platform_graphics`, `integration.two_window_swapchain`, `example.triangle` |
| Dev shell with Xvfb (`DISPLAY` set, X11) | everything above plus the window class and the example | none in the last gate run |
| Real Wayland session | window / presentation and the example | `platform.native_events` (an unmapped Wayland window gets no resize); capability tests |
| Real GPU (local only) | all of the above on hardware; the only place hardware-specific behaviour (present fences, multiple queues) is exercised | none by design |

To run the window class locally on a machine without a display: start an X server (for example `Xvfb :99 -screen 0 1280x720x24`), then `DISPLAY=:99 nix develop --command just project::check`. A run without a display is a valid run of the other classes, and its window tests show as *Skipped*; it is not evidence about a real window system or compositor.

## Known gaps

These are stated so a green gate is not read as more than it is:

- The window class does not exercise minimize or zero-size recovery, a guaranteed `out_of_date`, or a window-manager close request (see `example.triangle`).
- `example.triangle` requires the resize to happen (`--require-resize`) and reports per-path counts, but it cannot force the window system to answer: where the resize is refused it reports SKIP. It does not require `out_of_date` or zero-size paths, and shutdown always idles the Queue, so it never tests the retirement queue on its own.
- Retirement through the queue is exercised only on a device with present fences; without them every recreation reports `retired_via_queue_idle`.
- The CI/hosted-runner question is open: no hosted CI exists yet, so the matrix above is what a developer runs, not what is enforced on every push.

## Graphics foundation graduation

The Graphics foundation counts as validated when, on the gate environment with the layer and a display:

1. every CPU and headless GPU test passes, with none skipped other than for a capability the device really lacks, and the output has no `validation-layer=SKIP` line (only the required tier and `example.triangle` enforce the layer);
2. the window class passes under X11 (Xvfb) and, where available, a real Wayland session (on Wayland `platform.native_events` reports SKIP, as listed above);
3. `graphics.headless_triangle` and `example.triangle` pass, so the Terreate API alone covers offscreen and windowed rendering;
4. `cmake.package_smoke` passes, so an installed package works from an external consumer;
5. the known gaps above are either closed or recorded in an Issue.
