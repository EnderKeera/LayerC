---
name: verify
description: Build LayerC's Linux programs and drive layerd/layerctl/layercal at runtime (headless and under Xvfb) to verify a change.
---

# Verify LayerC on Linux at runtime

Linux only reaches `layerd`, `layerctl`, and `layercal`; the editor, capture
worker, and Game Bar host are Windows-only and cannot be driven here.

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
