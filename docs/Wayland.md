# LayerC on Wayland (V2.7, in development)

This document owns the Wayland backend's architecture, contracts, and
invariants. [`Linux.md`](Linux.md) keeps owning user-facing Linux behavior,
commands, packaging, and evidence; it gains a Wayland section once the lanes
below land. Until then nothing here is a support claim: V2.6 remains X11-only.

## Target

| Desktop | Windows | Overlay | Input | Capture |
| --- | --- | --- | --- | --- |
| KDE Plasma 6 (KWin) | KWin script → D-Bus | `wlr-layer-shell` | evdev (opt-in) or XWayland XI2 | ScreenCast portal + PipeWire |
| wlroots family (sway, labwc, river, Wayfire, Hyprland) | `wlr-foreign-toplevel-management` | `wlr-layer-shell` | evdev (opt-in) or XWayland XI2 | ScreenCast portal + PipeWire |
| GNOME 45–50 (Mutter) | LayerC Shell extension → D-Bus | LayerC Shell extension | evdev (opt-in) or XWayland XI2 | ScreenCast portal + PipeWire |
| X11 sessions | unchanged V2.6 adapters | unchanged | unchanged | unchanged |

Gamescope stays unsupported. XWayland fallbacks keep LayerC useful when a
native provider is missing, and are always reported as `degraded`.

## Product boundaries (V2.7 deltas)

- **Input stays passive.** Wayland has no passive global input protocol, so the
  only global source is evdev and it is **opt-in**: `layerd --input=evdev` or
  `LAYERC_INPUT=evdev`. It needs the user's `input` group membership, a
  one-time step the user performs; `layerd` never elevates, changes groups, or
  prompts. Devices are opened `O_RDONLY | O_NONBLOCK | O_CLOEXEC`, never
  `EVIOCGRAB`bed, never written. uinput, like XTest, is test-only and must not
  link into `layerd`. Without the opt-in, Wayland input comes from XI2 through
  XWayland and is `degraded` with reason `xwayland_focus_only`.
- evdev sees every keystroke on the seat. Events are normalized and fed to the
  session registry exactly like XI2 raw events on X11 (which are also global);
  nothing beyond the existing `last_input_match` diagnostic is retained or
  logged.
- **No network.** The session D-Bus bus and PipeWire are local IPC. `layerd`
  talks only to `org.freedesktop.portal.Desktop` (ScreenCast),
  `org.kde.KWin` (Scripting), and the LayerC GNOME Shell extension.
- **Demand-driven, idle-blocking, owned, no threads.** The Wayland connection
  opens at start like the X11 connection. The session bus opens only when a
  provider needs it; the KWin script is loaded only while window tracking is
  needed; evdev devices open only while input is demanded; layer surfaces exist
  only while overlay is demanded; a portal session and PipeWire stream exist
  only while visual capture is demanded. The `wl_display`, sd-bus, `pw_loop`,
  evdev, and inotify descriptors all go through the one `EventLoop`; sd-bus
  timeouts and PipeWire timers feed `next_deadline`. No adapter starts a thread
  (`pw_thread_loop` is not used).
- Cooldowns, retries, and debounce use monotonic clocks.

## Seam

`DisplayAdapters` (`src/daemon/linux/display_adapters.{hpp,cpp}`) replaces
`X11Adapters` with the same orchestration surface (`start`, `refresh`,
`refresh_status`, `dispatch`, `has_pending_events`, `dispatch_pending`,
`render`, `stop_sessions`, `next_deadline`, `session_count`,
`session_diagnostics`). It owns each shared connection at most once:
`X11Display` (`$DISPLAY`, possibly XWayland), `WaylandConnection`
(`$WAYLAND_DISPLAY`), and `BusConnection` (session bus, lazy).

Display-server-neutral values live in
[`src/platform/linux/display_types.hpp`](../src/platform/linux/display_types.hpp):
`WindowSnapshot`/`WindowScan`, `Monitor`/`MonitorScan`, `OverlayFrame`,
`panel_rect`, and the `OverlayDisplayList` paint model that every renderer
(Xlib, cairo on a layer-shell buffer, the GNOME extension) consumes. No
backend header may reach it.

Session detection: `wayland` when `$WAYLAND_DISPLAY` connects, otherwise `x11`
when `$DISPLAY` connects, otherwise `none` (headless store/control bridge).
`LAYERC_DISPLAY_BACKEND=auto|x11|wayland` overrides detection for tests and
for users who prefer the XWayland path. Compositor detection: `kwin` (owner of
`org.kde.KWin` or `XDG_CURRENT_DESKTOP` containing `KDE`), `gnome` (owner of
`org.gnome.Shell` or `GNOME`), `wlroots` (both `zwlr_layer_shell_v1` and
`zwlr_foreign_toplevel_manager_v1` advertised), otherwise `unknown`.

### Providers

Each capability picks the first available provider; the choice, its state, and
stable reason tokens are published in status.

| Capability | Wayland session order | X11 session |
| --- | --- | --- |
| windows | `kwin_script`, `gnome_shell`, `wlr_foreign_toplevel`, `x11_ewmh` (XWayland windows only, degraded) | `x11_ewmh` |
| monitors | `wayland_output`, `x11_randr` (degraded) | `x11_randr` |
| input | `evdev` (opt-in), `x11_xi2` (degraded `xwayland_focus_only`) | `x11_xi2`, `evdev` (opt-in) |
| overlay | `wlr_layer_shell`, `gnome_shell`, `x11_shape` (degraded `xwayland_overlay`) | `x11_shape` |
| capture | `portal_screencast`, `x11_composite` (XWayland windows only, degraded) | `x11_composite` |

Window ids are opaque per provider. Overlay and input providers need only
geometry, so any combination works. `x11_composite` capture needs an XID: it is
usable with `x11_ewmh` windows, or with native reports that carry `x11Window`.

Coordinates on Wayland are global **logical** pixels. Wayland monitors report
`dpi = 96` and `buffer_scale = ceil(output scale)`; display lists are built at
96 dpi in logical pixels and renderers scale by `buffer_scale` for crisp
output, which matches the Windows MulDiv result at the same desktop scale.

## Window report contract (KWin script and GNOME extension → layerd)

`layerd` owns `io.github.EnderKeera.LayerC` on the session bus
(`DO_NOT_QUEUE`; a second daemon reports `bus_name_taken`) and exports
`/io/github/EnderKeera/LayerC` with interface
`io.github.EnderKeera.LayerC.Shell1`:

- method `ReportWindows(s report)`; property `Version` (u) = 1.
- Only the unique name that currently owns `org.kde.KWin` or `org.gnome.Shell`
  may call it; anyone else receives `org.freedesktop.DBus.Error.AccessDenied`.

`report` is a full snapshot (never a delta):

```json
{"version":1,"source":"kwin","windows":[{"id":"{uuid-or-id}","title":"Game",
 "class":"game.exe","instance":"game.exe","pid":4242,"x":0,"y":0,
 "width":2560,"height":1440,"active":true,"fullscreen":true,"hidden":false,
 "x11Window":79691780}]}
```

Bounds: payload ≤ 64 KiB, ≤ 64 windows (excess truncated and counted in
`WindowScan::truncated`), strings ≤ 512 bytes of valid UTF-8, `id` ≤ 128
bytes; unknown fields are ignored; `pid` and `x11Window` are optional. A
malformed report is rejected as a whole and surfaces a window-provider reason.
`id` maps to a stable nonzero `std::uint64_t` through core hashing of
`source + id`. `executable_name`/`executable_path` come from `/proc/<pid>/exe`
when the PID belongs to the same UID, exactly like X11.

### KWin script

`layerd` writes its embedded script to
`$XDG_RUNTIME_DIR/layerc/kwin/layerc-windows.js` (directory 0700, file 0600),
unloads a stale `layerc-windows` plugin, then calls
`org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript(path,
"layerc-windows")` and `run` on the returned script object (KWin 6 path
`/Scripting/Script<id>`; KWin 5.27 path `/<id>`). It unloads the script when
window tracking is no longer needed and on shutdown. The script uses the KWin 6
API (`workspace.windowList()`, `workspace.activeWindow`, `windowActivated`,
`windowAdded`, `windowRemoved`, per-window caption/geometry/fullscreen/minimized
signals) with a KWin 5.27 fallback (`clientList`, `activeClient`,
`clientActivated`, …), reports only normal windows, and calls
`callDBus("io.github.EnderKeera.LayerC", "/io/github/EnderKeera/LayerC",
"io.github.EnderKeera.LayerC.Shell1", "ReportWindows", json)` with an initial
snapshot on load and after every relevant change.

### GNOME Shell extension

UUID `layerc@enderkeera.github.io`, GNOME Shell 45–50 (ESM), shipped under
`packaging/gnome-shell/` and installed to
`${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions/<uuid>/`. The user enables it
with `gnome-extensions enable`; it never accesses the network.

- **Windows:** while `io.github.EnderKeera.LayerC` has an owner, the extension
  sends `ReportWindows` (`"source":"gnome"`) on focus, title, geometry,
  minimize, fullscreen, create, and unmanage changes, coalesced to at most
  10 Hz with a monotonic GLib timeout, plus an initial snapshot when the name
  appears. It reports nothing while the daemon is absent.
- **Overlay:** the extension owns `io.github.EnderKeera.LayerC.GnomeShell`,
  object `/io/github/EnderKeera/LayerC/GnomeShell`, interface
  `io.github.EnderKeera.LayerC.Overlay1`: `Present(s frame)`, `Clear()`,
  property `Version` (u) = 1. Before calling, `layerd` checks that the owner of
  that name equals the owner of `org.gnome.Shell`.
- `frame` is a full replacement:
  `{"version":1,"panels":[{"id":"…", <append_overlay_display_list_json fields>}]}`,
  ≤ 64 panels, ≤ 256 ops per panel, ≤ 64 KiB. Panels are drawn in logical
  stage coordinates by non-reactive actors that never take input and stay above
  fullscreen windows. Every panel is removed on `Clear()`, when the daemon's
  name vanishes, and on `disable()`.

## Native Wayland adapters

### Layer-shell overlay (`wlr_layer_shell`)

One `zwlr_layer_surface_v1` per panel: layer `overlay`, namespace
`layerc-overlay`, anchored top|left on the output that contains the panel
(`monitor_for_rect`), margins = panel position − output origin,
`exclusive_zone = -1`, keyboard interactivity none. An **empty** `wl_region`
is set as the input region before the first commit and kept on every commit
(a null region would take input); an overlay whose empty input region was not
set is never mapped. The first buffer waits for `configure`/`ack_configure`;
`closed` drops the surface and the next update recreates it. Buffers are
double-buffered `wl_shm` XRGB8888 painted by cairo from the display list at
`buffer_scale`.

### Foreign toplevel tracking (`wlr_foreign_toplevel`)

`zwlr_foreign_toplevel_manager_v1` (read-only: no activate/close/set requests).
`title`, `app_id` (→ `class_name` and `instance_name`), activated/fullscreen/
minimized state, and output enter/leave are applied on `done`. There is no PID
or window geometry, so executable matching is unavailable and geometry is the
rect of the first entered output; both are published as degraded reasons.

### Monitors (`wayland_output`)

`wl_output` (v2+, `name` from v4) plus `zxdg_output_manager_v1` for logical
position and size. `dpi = 96`, `buffer_scale = wl_output.scale`.

### evdev input

- Devices: `/dev/input/event*` (directory injectable for tests); an inotify
  watch reports hotplug and permission changes as `devices_changed`.
- Classification by `EVIOCGBIT`: keyboard (letter, space, and enter keys),
  mouse (`REL_X`/`REL_Y` plus `BTN_LEFT`), gamepad (`BTN_GAMEPAD` or
  `BTN_JOYSTICK` plus `ABS_X`/`ABS_Y`); everything else is ignored.
- Keys: `keycode + 8` → xkbcommon keysym at layout 0, level 0 →
  `keysym_to_vk`, the same table the X11 adapter uses. The keymap comes from
  the Wayland `wl_keyboard` keymap when available, else
  `xkb_keymap_new_from_names` defaults, else a built-in US table. Autorepeat
  (`value == 2`) is ignored; `SYN_DROPPED` resynchronizes with `EVIOCGKEY` and
  releases keys that are no longer down.
- Mouse buttons use the X11 path's profile codes; vertical wheel steps become
  press+release pairs on the wheel codes.
- Gamepad buttons use the controller code space the Windows daemon publishes,
  so profiles stay portable.
- Reasons: `evdev_not_enabled`, `evdev_permission_denied` (with the `input`
  group hint), `evdev_no_keyboard`, `evdev_no_mouse`, `evdev_no_gamepad`
  (limited to the classes a profile needs).
- Flatpak grants no device access; evdev there needs the user's own
  `flatpak override --user --device=all`.

### Portal capture (`portal_screencast`)

- `org.freedesktop.portal.ScreenCast` version 4 or later (persistence):
  `CreateSession` → `SelectSources` (`types` = window when
  `AvailableSourceTypes` has it, otherwise monitor; `multiple = false`;
  hidden cursor when available; `persist_mode = 2`; stored `restore_token`) →
  `Start` → `OpenPipeWireRemote`. Every request subscribes to its `Response`
  before calling, using the handle path derived from the unique name and a
  per-request token.
- The single-use `restore_token` is stored per profile id in
  `$XDG_DATA_HOME/layerc/state/screencast.json` through the durable write
  helper and replaced with every new token.
- A user cancel/deny marks capture unavailable (`portal_denied`) until the
  next configuration or demand change; there is no retry loop.
- PipeWire: `pw_init` once, a `pw_loop` driven from the main loop
  (`pw_loop_get_fd` + `pw_loop_iterate(loop, 0)`), `pw_context_connect_fd`
  on the portal fd, one `pw_stream` per session negotiating
  BGRx/BGRA/RGBx/RGBA with `MemPtr`/`MemFd` buffers (no DMA-BUF in V2.7),
  converted into `capture::CaptureFrame` honouring `CaptureQuality`.
- Monitor sources are cropped to the window rect (logical → buffer pixels) and
  reported degraded `monitor_crop`, because overlays may appear in the pixels.
- The portal session is closed as soon as visual demand drops.

## Status schema

The `x11` object stays byte-identical. A `display` object follows it:

```json
"display":{"session":"wayland","override":"auto","desktop":"KDE",
 "compositor":"kwin","wayland":{"connected":true,"globals":{"wl_compositor":6,
 "zwlr_layer_shell_v1":4}},"bus":{"connected":true},
 "providers":{"windows":{"name":"kwin_script","state":"active","reasons":[]},
 "monitors":{…},"input":{…},"overlay":{…},"capture":{…}}}
```

Providers that are not compiled in report `not_built`. `layerctl status`
prints one `display:` line with the session, compositor, and providers.

## Build

Each dependency group is detected through pkg-config and degrades to
`not_built` providers without warnings:

| Define | Packages | Enables |
| --- | --- | --- |
| `LAYERC_HAVE_WAYLAND` | `wayland-client`, `wayland-scanner`, `wayland-protocols`, `xkbcommon`, `cairo` | wayland connection, layer-shell, foreign toplevel, outputs |
| `LAYERC_HAVE_XKBCOMMON` | `xkbcommon` | evdev keymaps (evdev itself is always built on Linux) |
| `LAYERC_HAVE_SDBUS` | `libsystemd` or `libelogind` | session bus, KWin script, GNOME bridge, portal |
| `LAYERC_HAVE_PIPEWIRE` | `libpipewire-0.3` | portal capture |

`wlr-layer-shell-unstable-v1.xml` and
`wlr-foreign-toplevel-management-unstable-v1.xml` are vendored with their
license headers under `third_party/wayland-protocols/`; `xdg-shell` and
`xdg-output` come from the system `wayland-protocols`. Each lane owns one
`cmake/LayerCLinux*.cmake` file included from `CMakeLists.txt`.

## Tests

| Label | Harness | Skips (77) when |
| --- | --- | --- |
| `wayland` | headless sway (`WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1`, private `XDG_RUNTIME_DIR`) via `tools/wayland-test-session.sh` | sway is missing |
| `dbus` | `dbus-run-session` with fake `org.kde.KWin`, `org.gnome.Shell`, and portal services | `dbus-run-session` is missing |
| `evdev` | synthetic event streams and capability bitmaps; optional uinput probe | — (uinput probe only) |
| `gjs` | `gjs -m` unit tests of the extension's pure modules | gjs is missing |

## Development lanes

| Lane | Branch | Owns |
| --- | --- | --- |
| S – seam | `v27/wl-seam` | `src/daemon/linux/**`, `src/platform/linux/x11_*`, `src/platform/linux/display_types.*`, `src/runtime/**` status, `src/ctl/layerctl.cpp`, existing Linux tests |
| A – wayland | `v27/wl-wayland` | `src/platform/linux/wayland_*`, `third_party/wayland-protocols/`, `cmake/LayerCLinuxWayland.cmake`, `tests/linux_wayland_*`, `tools/wayland-test-session.sh`, Wayland fixtures |
| B – evdev | `v27/wl-evdev` | `src/platform/linux/evdev_*`, `src/platform/linux/keysym_vk.*`, `cmake/LayerCLinuxEvdev.cmake`, `tests/linux_evdev_*` |
| C – bus | `v27/wl-bus` | `src/platform/linux/{dbus,kwin,gnome_bridge,portal,pipewire}_*`, `cmake/LayerCLinuxBus.cmake`, `tests/linux_{dbus,kwin,gnome_bridge,portal,pipewire}_*`, D-Bus fakes |
| D – gnome | `v27/wl-gnome` | `packaging/gnome-shell/**`, `cmake/LayerCGnomeShell.cmake`, `tests/gjs/**` |
| I – integration | `v27/wayland` | provider wiring in `DisplayAdapters`, CI, `docs/Linux.md`, `README.md`, `AGENTS.md`, `tools/linux-deps.txt`, Flatpak |
