# Release verification on real hardware

The gate runs in the pinned Nix shell, on a software Vulkan device and a virtual X display. It cannot
show how Terreate behaves on a real GPU and a real compositor. This procedure is run by hand during the
release-candidate period and its results are recorded on the release Issue. It also covers what no test
checks: minimized-window recovery, an `out_of_date` swapchain, and a window-manager close request.

Do not stop or restart the compositor session to run this; every step works inside the running session.

## 0. Prepare and record the environment

Open a terminal inside the Wayland session (Hyprland), in a clean checkout of the release-candidate tag.

```sh
git checkout v0.1.0-rc.1
nix develop                       # the pinned shell
vulkaninfo --summary              # GPU, driver and Vulkan version: record them
echo "$XDG_SESSION_TYPE $WAYLAND_DISPLAY $DISPLAY"
hyprctl version                   # the compositor version (command may differ by version)
```

Record: GPU model and driver, Vulkan version, compositor and its version, the commit hash.

Force Wayland for the steps below by making sure the X variable is not used: `unset DISPLAY` (in that shell
only). GLFW then connects to Wayland.

## 1. The gate on the real GPU and Wayland

```sh
unset DISPLAY
just project::check
```

Expected: all tests pass. `platform.native_events` is **skipped by design on Wayland** (an unmapped window
gets no resize event; the test says so). Any other skipped test, any failure, and any message from the
validation layer must be recorded. Copy the summary lines (the `tests passed` line and the list of skipped
tests) and the log path `.build/default/Testing/Temporary/LastTest.log`.

## 2. The scripted examples

```sh
unset DISPLAY
.build/default/examples/triangle/terreate_example_triangle --frames 90 --resize-at 30 --require-validation --require-resize
.build/default/examples/textured_quad/terreate_example_textured_quad --frames 90 --resize-at 30 --require-validation --require-resize
```

Expected: the last line is `frames=PASS`, exit status 0, no `validation` lines. On a tiling compositor the
window may refuse the resize: the run then prints `resize=SKIP` and no PASS line (that is a skip, not a
failure). To make the resize happen, run the example in a floating window (for example, start it, then
`hyprctl dispatch togglefloating` on it; or add a window rule for floating) and repeat. Record the printed
`device=`, `present-fences=`, `paths` and `retired generation=` lines: they say which retirement path ran.

## 3. Interactive checks (not covered by any test)

Run `terreate_example_triangle` and then `terreate_example_textured_quad` **without arguments** (they run
until the window is closed). For each, make the window floating first.

| Check | How | Expected |
| --- | --- | --- |
| Picture | Look at the window | The triangle / the two textured quads, correct colors, the nearer quad in front |
| Resize | Drag the window edges for about 20 s, large and small | No flicker artifacts that persist, no crash, no `validation` line in the terminal |
| `out_of_date` | The resizing above | At exit the `paths` line is printed: note `present_out_of_date` and `acquire_out_of_date` |
| Minimize / hide | Move the window to the special workspace (`hyprctl dispatch movetoworkspace special`), wait 10 s, bring it back | Rendering resumes; no crash; no validation message |
| Close request | Close with the window's close button or `hyprctl dispatch killactive` | The process exits with status 0 and prints `shutdown retirement=...` |
| Idle | Leave it running 2 minutes | No growth in CPU use that you can see, no messages |

If a compositor command differs on your version, do the equivalent action and note it.

## 4. Other devices (if available)

If the machine has more than one Vulkan device (a second GPU, the integrated GPU, or the software device),
repeat steps 1 and 2 for each. List the devices with `vulkaninfo --summary` and select one with the
loader's device-selection variable of your driver (for example `MESA_VK_DEVICE_SELECT` for Mesa). Record the
device each run used (the example prints `device=`).

## 5. Report

Post one comment on the release Issue:

```text
Release verification of v0.1.0-rc.N (commit <hash>)
Environment: <GPU, driver, Vulkan>, <compositor and version>, session type
1. Gate on Wayland: <pass / fail>; skipped: <list>; log: <where>
2. Scripted triangle: <frames=PASS | resize=SKIP | failure>; textured_quad: <...>
3. Interactive: picture <ok>, resize <ok>, out_of_date <counts>, minimize <ok>, close <exit 0>, idle <ok>
4. Other devices: <device -> result>
Problems: <none | description and the output lines>
```

For each problem open an Issue with the command, the output, the environment above and the log, and link it
from the release Issue. A problem that changes the code means a new release candidate.
