---
name: verify
description: Build LayerC's Linux programs and drive layerd/layerctl/layercal at runtime (headless and under Xvfb) to verify a change.
---

# Verify LayerC on Linux at runtime

Linux supports `layerd`, `layerctl`, the `layercal` CLI, and the X11 `layere` editor (v2.6c). The standalone graphical `layercal` UI, isolated capture worker and Game Bar host remain Windows-only. Drive editor cases with `tools/linux-editor-gui-tests.sh` in disposable Xvfb sessions; see `docs/Linux.md` for required dependencies and support boundaries.

## Editor evidence discipline

Use the [current source-bound editor qualification](../../../docs/LinuxEditorPlan.md#rendered-audit-repairs-and-current-local-qualification-2026-10-21) and preserve the source freeze throughout resource acquisition and broad gates. Check actual rendered calibration and appearance states (including explicit black, scaling and scrolled edges), not only strings, widget bounds or a producer's pass flag; also verify semantic/durable outcomes and fault recovery. Retain failed attempts and bind final documentation-only changes separately. Private Xvfb/WSL and container results do not establish VM, physical desktop or screen-reader-user acceptance.

The full X11 walkthrough must execute the four required controlled-target overlay cases: focus changes, corner-drag resize, actual click-through and cleanup (currently 36 required cases total). Do not defer that group or substitute runtime CTest results for process/desktop evidence. Require the real `xev` ButtonPress/ButtonRelease observation for clicks beside and on painted overlay pixels, fresh screenshot IDs for input, and complete owned-process cleanup. Preserve failing bundles and isolate conflicting disposable fixture copies through the real editor without deleting profiles or bypassing the failed-save test. See the [fresh required-overlay record](../../../docs/LinuxEditorPlan.md#required-overlay-walkthrough-audit-repair-2026-10-21).

## Build

```sh
xargs -a tools/linux-deps.txt apt-get install -y --no-install-recommends
apt-get install -y --no-install-recommends openbox xdotool x11-apps imagemagick  # driving/screenshot only
cmake -S . -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build $B --target layerd layerctl layercal layerc_linux_x11_smoke_fixture
```

Keep `$B` and all state in the scratchpad. Always export private
`XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_RUNTIME_DIR` (chmod 700) before
running any program.

## Headless

`unset DISPLAY; layerd &` then `layerctl status --json | reload | stop`.
Plain `layerctl status` only prints "status acknowledged"; use `--json`.

## End-to-end overlay under Xvfb

1. `Xvfb :77 -screen 0 1280x800x24 &` and `openbox &` (EWMH is needed for
   window tracking; bare Xvfb has no `_NET_ACTIVE_WINDOW`).
2. Copy the fixture to a file named `lctest.exe` and run it; it prints its XID.
   The fixture does not set `_NET_WM_PID`, so set it:
   `xprop -id $W -f _NET_WM_PID 32c -set _NET_WM_PID $PID`.
3. Import a profile with `"executableName": "lctest.exe"` and a keyboard
   binding using the Win32 VK code (E = 69), then start `layerd`.
4. `xdotool key e`, then `xwd -root -silent | convert xwd:- shot.png`.
   Counting `#50BAC8` pixels in the panel crop tracks the bar draining.

Gotchas: keep `XDG_RUNTIME_DIR` short; the control socket path must fit the
108-byte Unix socket limit, and a deep scratchpad path overflows it
("control socket path exceeds the unix name limit"). `xdotool windowactivate` does not move focus under openbox here; use
`xdotool windowfocus`. `examples/lctest.json` binds VK 0x2A/0x12, which is
awkward to press; author a test profile instead.

## Worth probing

Retrigger during cooldown, key while target unfocused, focus away/back
mid-cooldown, killing the target, SIGTERM, a stale socket file, a second
daemon, `layercal snapshot` with `--region` (normalized 0..1, metadata only),
bundle `--dry-run`/`--keep-existing`, malformed import JSON.
