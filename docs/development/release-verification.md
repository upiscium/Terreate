# Release verification on real hardware

The gate runs in the pinned Nix shell, by default without a display (the window tests are skipped) or on
a virtual X display that the developer provides, and on a software Vulkan device. It cannot show how Terreate
behaves on a real GPU and a real compositor. This procedure is run by hand during the release-candidate period
and its results are recorded on the release Issue. It also covers what no test checks: an `out_of_date`
swapchain, how a hidden window behaves, and a window-manager close request. It does **not** verify
minimized-window recovery on a compositor that ignores minimize requests (see step 3).

Do not stop or restart the compositor session to run this; every step works inside the running session.

## 0. Prepare and record the environment

Open a terminal inside the Wayland session (Hyprland), in a clean checkout of the release-candidate tag
(or, before the tag exists, of the commit that the release Issue names: the tag will be put on a commit with the
same tree, so the results hold for it). Run the
commands below either inside `nix develop` (an interactive shell) or as `nix develop --command <command>`.
`unset DISPLAY` in that shell only, so that GLFW uses Wayland and not XWayland.

```sh
git checkout v0.1.0-rc.1          # or the commit named on the release Issue, before the tag exists
nix develop                       # the pinned shell; then, inside it:
unset DISPLAY
vulkaninfo --summary              # GPU, driver and Vulkan version: record them
echo "$XDG_SESSION_TYPE $WAYLAND_DISPLAY"
hyprctl version                   # the compositor version (the command may differ by version)
```

Record: GPU model and driver, Vulkan version, compositor and its version, the commit hash.

**Check that a real GPU is used.** The Nix shell ships a software Vulkan device (Mesa's lavapipe). On a machine
that is not NixOS, the host's GPU driver may be invisible from the Nix shell, and then `vulkaninfo --summary`
lists only `llvmpipe`/lavapipe. If the examples print `device=llvmpipe` (or any CPU device), the run does not
verify a real GPU: record that, and do not count the run as a hardware verification.

**Check that Wayland is used.** While an example runs, `hyprctl clients` lists its window (title `terreate`);
its entry must say `xwayland: 0`.

## 1. Float the example windows

The examples create a window titled `terreate`. A tiling compositor sizes it itself and may refuse the scripted
resize. Add a window rule that makes windows titled `terreate` floating before steps 2 and 3. The rule syntax
(`windowrule`/`windowrulev2`) depends on the Hyprland version: use the one of your version (see the Hyprland
wiki, "Window Rules"), and note which you used. Hyprland 0.57 and later is configured
with Lua: there is no `hyprctl keyword`, `windowrule` or `windowrulev2`, and the legacy `hyprctl dispatch` syntax is
gone. A rule can then be added to the running session through `hyprctl repl` with
`hl.window_rule({ name = "terreate-release-verification", match = { title = "^(terreate)$" }, float = true })`
(it lasts until the configuration is reloaded: reload it afterwards to remove the rule). A rule is needed because the scripted runs end in about two
seconds: there is no time to float the window by hand.

## 2. The gate on the real GPU and Wayland

```sh
just project::check               # inside the shell of step 0 (DISPLAY unset)
```

Expected: every test passes or is skipped for a reason named here. `platform.native_events` is **skipped by
design on Wayland**; `graphics.retirement.multiqueue` is skipped on a device that has no two distinct native queues
("no valid distinct native queues": the Intel and the software devices); the timeline tests are skipped on a device
without the feature (an unmapped Wayland window gets no resize event; the test says so). `example.triangle` and
`example.textured_quad` are reported as *Skipped* when the compositor refuses the resize (`resize=SKIP`): with
the rule of step 1 they should run. Any other skipped test, any failure, and any message from the validation
layer must be recorded. Copy the summary lines (the `tests passed` line and the list of skipped tests) and the
log path `.build/default/Testing/Temporary/LastTest.log`.

## 3. The scripted examples

```sh
.build/default/examples/triangle/terreate_example_triangle --frames 90 --resize-at 30 --require-validation --require-resize
.build/default/examples/textured_quad/terreate_example_textured_quad --frames 90 --resize-at 30 --require-validation --require-resize
```

Expected: the last line is `example.<name>: frames=PASS presented=90`, exit status 0, no `validation` lines.
`resize=SKIP` with no PASS line means the compositor refused the resize (step 1 not in effect), a skip and not a
failure. Record the printed `device=`, `present-fences=`, `paths` and `retired generation=` lines: they say
which retirement path ran.

## 4. Interactive checks (not covered by any test)

Run `terreate_example_triangle` and then `terreate_example_textured_quad` **without arguments** (they run until
the window is closed). The window is floating by the rule of step 1.

| Check | How | Expected |
| --- | --- | --- |
| Picture | Look at the window | The triangle / the two textured quads, correct colors, the nearer quad in front |
| Resize | Drag the window edges for about 20 s, large and small | No persisting artifacts, no crash, no `validation` line in the terminal |
| `out_of_date` | The resizing above | At exit the `paths` line is printed: note `present_out_of_date` and `acquire_out_of_date` |
| Hidden window | Move it to the special workspace without focusing it (`hyprctl dispatch movetoworkspacesilent special`; on Hyprland 0.57 and later `hl.dsp.window.move({ workspace = "special", window = "address:0x...", follow = false })`), wait 10 s, bring it back (`hyprctl dispatch togglespecialworkspace`, then move it to your workspace; on 0.57 `hl.dsp.workspace.toggle_special("special")` and `hl.dsp.window.move({ workspace = 1, ... })`) | Rendering resumes; no crash; no validation message. Hyprland does **not** minimize the window, so the framebuffer keeps its size: `zero_size_waits` and `deferred_zero_extent` in the `paths` line stay 0, and the zero-size recovery path is **not** exercised by this check. Record the two counters |
| Close request | Close with the window's own close button, or `hyprctl clients` for its `address` then `hyprctl dispatch closewindow address:<address>` (do not use `killactive`: it closes the focused window, which may be your terminal; on 0.57 and later focus the window by address, check that it is the active one, then `hl.dsp.window.close({ window = "address:0x..." })`) | The process exits with status 0 and prints `shutdown retirement=...` |
| Idle | Leave it running 2 minutes | No messages, no visible CPU growth |

If a compositor command differs on your version, do the equivalent action and note it.

## 5. Other devices (if available)

If the machine has more than one Vulkan device (a second GPU, the integrated GPU, or the software device),
repeat steps 2 and 3 for each. List the devices with `vulkaninfo --summary`. The examples pick their own device
(discrete first), so changing the device order is not enough to make them use another one. The most reliable way is to
restrict the loader to one driver: `VK_DRIVER_FILES=<one ICD manifest>` (the manifests are under
`/run/opengl-driver/share/vulkan/icd.d` on NixOS and in the Nix shell's Mesa). `MESA_VK_DEVICE_SELECT` selects by
vendor and device id and cannot tell two Mesa drivers of the same device apart. Note that one physical device can be
listed twice when the host driver and the Nix shell's Mesa both expose it (the same UUID): that is handled by the
tests (the library refuses an explicit UUID that names two candidates as ambiguous) but it makes device lists longer.
A host driver built against a newer glibc than the Nix shell's can fail to load inside the shell: record it and use
the Nix shell's own driver for that device. Record the device
each run used (the example prints `device=`).

## 6. Report

Post one comment on the release Issue:

```text
Release verification of v0.1.0-rc.N (commit <hash>)
Environment: <GPU, driver, Vulkan>, <compositor and version>, session type, xwayland: <0|1>, window rule: <syntax used>
Real GPU confirmed (device= is not llvmpipe): <yes/no>
2. Gate on Wayland: <pass / fail>; skipped: <list>; log: <where>
3. Scripted triangle: <frames=PASS | resize=SKIP | failure>; textured_quad: <...>
4. Interactive: picture <ok>, resize <ok>, out_of_date <counts>, hidden window <ok; zero_size_waits=<n> deferred_zero_extent=<n>>, close <exit 0>, idle <ok>
5. Other devices: <device -> result>
Problems: <none | description and the output lines>
```

For each problem open an Issue with the command, the output, the environment above and the log, and link it from
the release Issue. A problem that changes the code means a new release candidate.
