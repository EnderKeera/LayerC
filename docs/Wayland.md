# LayerC on Wayland (V2.7, in development)

This document owns the Wayland backend's architecture, contracts, and
invariants. [`Linux.md`](Linux.md) keeps owning user-facing Linux behavior,
commands, packaging, and evidence, including a V2.7 development section.
Nothing here is a support claim: V2.6 remains X11-only.

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

On wlroots compositors with XWayland, `x11_ewmh` keeps window tracking:
foreign toplevels carry no PID, so executable-matched profiles could never
attach. `wlr_foreign_toplevel` is used only without an X11 connection.
Monitors and overlay stay native in either case.

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
- A panel that would exceed these bounds is rejected on its own: accepted
  panels keep presenting, the overlay row is `degraded` with
  `gnome_frame_too_large`, and it recovers once the panel fits or is removed.
- The extension accepts `Present` and `Clear` only from the current unique
  owner of `io.github.EnderKeera.LayerC` (`AccessDenied` otherwise) and
  rejects an invalid frame whole with `InvalidArgs`, keeping the last good
  one. Reports go only to that unique owner, without bus activation, with one
  call in flight and no retry.

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
rect of the first entered output; both are published as degraded reasons
(`toplevel_no_pid`, `toplevel_output_geometry`). Compositors that send no
`output_enter` (sway 1.10) get the only monitor's rect when exactly one
exists; otherwise the geometry stays empty and no panel is anchored to that
window rather than guessing a monitor.

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
  (limited to the classes a profile needs), `evdev_device_unavailable`,
  `evdev_unavailable`, and `evdev_keymap`. Provider rows carry only the token;
  device paths and errno text stay in the capability diagnostics.
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
- PipeWire: demanded streams hold balanced `pw_init`/`pw_deinit` leases; the
  last stream releases library resources. A `pw_loop` is driven from the main
  loop (`pw_loop_get_fd` + `pw_loop_iterate(loop, 0)`), with
  `pw_context_connect_fd` on the portal fd and one `pw_stream` per session
  negotiating BGRx/BGRA/RGBx/RGBA with `MemPtr`/`MemFd` buffers (no DMA-BUF in
  V2.7), converted into `capture::CaptureFrame` honouring `CaptureQuality`.
  Native capture requires PipeWire 1.2 or later to disable every data-loop
  thread; older runtimes report `pipewire_version_requires_1_2`. The private
  context uses the null configuration and explicitly loaded local modules;
  custom `PIPEWIRE_CONFIG_NAME` overrides report `pipewire_config_override`
  rather than executing helpers or starting threads. Library diagnostics use
  stderr, never the default persistent journal/file logger.
- Monitor sources are cropped to the window rect (logical → buffer pixels) and
  reported degraded `monitor_crop`, because overlays may appear in the pixels.
  The frame keeps whole-window coordinates: a window partly outside its
  monitor is placed on a whole-window canvas whose missing part is excluded
  through `available_region`, so rule regions sample the same window pixels.
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

## Implementation evidence

The display seam and the wlroots-family native providers (outputs, foreign
toplevels, layer-shell overlay) and opt-in evdev input routing are implemented
on the local Linux-editor integration branch, as is the daemon side of the bus
lane (Shell1 window reports, KWin script loader, GNOME overlay bridge client,
ScreenCast portal, and PipeWire capture) and the GNOME Shell extension.
This is not a V2.7 support claim.

- `DisplayAdapters` selects a successfully connected Wayland transport in
  `auto`, then X11, then headless. A forced backend never selects the other
  session. Invalid overrides select headless with `invalid_display_backend`.
  A Wayland session may still use explicitly degraded XWayland providers.
- Connection establishment and the registry handshake share a monotonic
  250 ms deadline. Named sockets connect nonblocking; a full listening backlog
  fails without retry polling. Inherited `WAYLAND_SOCKET` ownership is unchanged.
  More than 1024 global instances or 128 distinct interfaces fail the
  connection with `wayland_too_many_globals` instead of publishing a truncated
  registry snapshot.
  Loss unregisters the transport fd, publishes `wayland_connection_lost`, and
  does not start a retry timer. A remaining XWayland queue is drained with
  sessions stopped when the forced Wayland backend is lost, so an unselected
  connection cannot busy-loop.
  The connection owns no worker thread.
- The `x11` object's bytes remain unchanged. The optional typed `display`
  object roundtrips through shared diagnostics parsing; `layerctl status`
  prints a display/provider summary. Native providers that have not landed
  report `not_built` rather than active capabilities. Compositor detection
  uses desktop hints and advertised registry globals; once window tracking
  opens the session bus, the owner of `org.kde.KWin` or `org.gnome.Shell`
  overrides the desktop hint. The bus remains closed until demanded.
- Automated Debian WSL evidence: private transport/server tests, seam tests,
  Xvfb X11 compatibility/idle-loop tests, and a no-X11/no-Wayland build.
  MSVC runtime and editor diagnostics tests also pass. These are not physical
  desktop, KWin, GNOME, or native Wayland-rendering evidence.
- The X11 GUI and smoke harnesses clear inherited Wayland sockets and force
  their own backend; headless CTest clears both display socket environments.
- Native providers bind lazily from the registry on the daemon's one
  connection and are released by its hook before the display goes away. The
  session engine drives either X11 overlay windows or the native
  `OverlayBackend` (`src/daemon/linux/display_providers.hpp`); with no native
  source the X11 path is unchanged. Layer surfaces are created on first paint,
  painted by cairo into double-buffered `wl_shm` XRGB8888 at the integer
  buffer scale, and not repainted while their content is unchanged.
  `Capability::overlay` takes its platform availability from layer-shell when
  it is native. Native window ids are not XIDs, so X11 capture reports
  `visual: native windows have no X11 window id`. The native providers need
  the X11-enabled daemon build (the session engine); otherwise they report
  `not_built`. A registry without `wl_shm` keeps layer-shell `unavailable`.
- Automated headless-sway evidence (`wayland` label, two outputs at scale 1
  and 2): logical monitor layout and buffer scale; layer-shell panels mapped
  above a fullscreen window, read back with screencopy, crisp at scale 2,
  moved/resized after configure, unmapped on removal or client disconnect;
  a virtual-pointer click on a panel reaches the window beneath it; foreign
  toplevel title/app_id/active/fullscreen and close, without `output_enter`;
  and `DisplayAdapters` end to end (session, painted panel, provider status).
  Screencopy and the virtual pointer are test-only protocols. This is not
  KWin, GNOME, or physical-desktop evidence.
- evdev routing (`src/daemon/linux/evdev_route.*`): `layerd --input=auto|evdev`
  wins over `LAYERC_INPUT`; any other argument exits with status 2, and an
  invalid `LAYERC_INPUT` keeps evdev off with `invalid_input_backend`. The
  opted-in route serves `Capability::input` in a Wayland session, or in an X11
  session without XI2; otherwise XI2 stays first. Devices open when input is
  demanded and close on quiesce; each device and the inotify descriptor is
  registered on the one `EventLoop` and unregistered before it closes, and
  hotplug replaces the registrations. Events reach the same sessions and
  `last_input_match` as XI2. If evdev can read no device (every open denied),
  the XWayland XI2 fallback serves input as `x11_xi2` (degraded,
  `xwayland_focus_only` plus the evdev token). Without the opt-in the XWayland
  row also carries `evdev_not_enabled`; without an X11 connection the row is
  `evdev` (`unavailable`, `evdev_not_enabled`).
- The keymap is the first `wl_seat` keyboard's XKB v1 keymap (at most 1 MiB,
  mapped read-only, descriptor always closed), applied live when it changes.
  The daemon has no keyboard focus, so no key event reaches it. A keymap that
  does not compile falls back to the XKB defaults and reports `evdev_keymap`.
- Automated evdev evidence: FIFO-backed synthetic devices through the daemon
  route (opt-out opens nothing; press to cooldown trigger; hotplug add and
  remove; quiesce; permission and missing-class tokens), an in-process
  `wl_seat` server for keymap delivery and bounds, CLI opt-in status rows, and
  the lexical passive-boundary check over `src/platform/linux/evdev_*` and
  `src/daemon/linux/evdev_*`. No physical input device was used.
- Bus lane (`src/platform/linux/{dbus_connection,dbus_shell_service,kwin_script,gnome_bridge,portal_screencast,pipewire_capture}.*`,
  `pipewire_transport.c`, `src/daemon/linux/bus_providers.*`): one sd-bus
  connection, opened on demand and registered on the daemon `EventLoop`;
  sd-bus timeouts feed `next_deadline`. Each asynchronous call belongs to its
  client's guard: releasing the guard cancels the pending calls, their
  captured state, and their reply timeouts, while already queued messages
  (`ReleaseName`, `Clear`, `unloadScript`, portal `Close`) are still written.
  Slots are released after dispatch, never inside their own callback, and no
  exception crosses an sd-bus or PipeWire callback.
- `ReportWindows` is authenticated against the current owner of
  `org.kde.KWin` or `org.gnome.Shell`, and a report is parsed completely
  before it replaces the previous one. The GNOME bridge calls only a verified
  owner of both `org.gnome.Shell` and the extension name, with one call in
  flight and only the newest replacement kept. Portal responses are bound to
  the request path and the authenticated unique sender.
- The visual service holds one capture session per target behind a shared
  contract: explicit XWayland windows use X11 Composite capture, opaque native
  windows use the portal stream, and both feed the same evaluator.
- Automated bus evidence (`dbus` and `gjs` labels; `dbus-run-session` with fake
  KWin, GNOME Shell, and portal services): name ownership and conflict, sender
  authentication, KWin script load/run/unload on KWin 6 and 5.27 object
  paths, GNOME owner verification, bounded frame replacement and bounds
  recovery, the portal request sequence, restore-token persistence, denial,
  spoofed responses, owner loss, cancellation of 128 pending calls, PipeWire
  stream lifecycle against the local PipeWire runtime, partially visible
  window sampling, and builds without X11, sd-bus, or PipeWire. No KWin, GNOME
  Shell, or desktop portal was used; this is not physical-desktop evidence.
- GNOME Shell extension (`packaging/gnome-shell/layerc@enderkeera.github.io/`):
  pure `lib/` modules (report building and coalescing, frame validation,
  panel reconciliation, Overlay1 authorization) plus thin `extension.js` glue.
  `cmake/LayerCGnomeShell.cmake` installs it as the `GnomeShellExtension`
  component (`LAYERC_INSTALL_GNOME_EXTENSION`, default on); installing never
  enables it.
- Automated extension evidence (`gjs` label): bounded UTF-8 rows and 64 KiB
  snapshots, at most one report per 100 ms with one call in flight, frame
  bounds matching layerd's, reconciliation by panel id, sender authorization,
  the unmodified `extension.js` glue on a private `dbus-run-session` bus with
  fake Shell/Mutter objects (reports to a fake daemon, rogue `Present`
  denied, non-reactive actors, daemon loss, `disable()` releasing every
  signal and name), a lexical scan for network, system-bus, and process APIs,
  and a component install into a disposable prefix. No GNOME Shell was
  running; actor stacking above fullscreen windows is unverified.
- Editor on Wayland: `layere` stays an FLTK X11 client, so on Wayland it is an
  XWayland client. Daemon status names the session ("Wayland (wlroots),
  XWayland connected"); Runtime Diagnostics adds a `Display:` line and one row
  per provider exactly as LayerD reports it. Detected Tasks and Position on
  window scan X11 windows only: on Wayland they list XWayland clients and say
  so (`only XWayland windows are listed (degraded)`), naming LayerD's window
  provider. Native Wayland windows are not listed by the editor.
- Automated editor evidence: `tools/linux-editor-gui-tests.sh --mode wayland`
  runs the editor, LayerD, an XWayland game fixture and a native foot window in
  a private headless sway with XWayland (`tools/linux-gui/wayland_walkthrough.py`,
  5 required cases: XWayland/native split, status, diagnostics rows matching
  LayerD, degraded Detected Tasks, degraded placement), with the editor's
  resource targets and cleanup. Rootless XWayland has no root image, so
  screenshots compose the X windows; native windows are absent from them. The
  X11 walkthrough is unchanged. sway only; no KWin, GNOME, or physical desktop.

## Tests

| Label | Harness | Skips (77) when |
| --- | --- | --- |
| `wayland` | headless sway (`WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1`, private `XDG_RUNTIME_DIR`) via `tools/wayland-test-session.sh` | sway is missing |
| `dbus` | `dbus-run-session` with fake `org.kde.KWin`, `org.gnome.Shell`, and portal services | `dbus-run-session` is missing |
| `evdev` | synthetic event streams and capability bitmaps; optional uinput probe | — (uinput probe only) |
| `gjs` | `gjs -m` tests of the KWin script and the GNOME extension (glue under `dbus-run-session`), plus its datadir install | gjs (or `dbus-run-session` for the glue) is missing |

The reusable gate is `tools/run-linux-tests.sh --wayland required` (combine
with `--editor required --flatpak required` for editor/Flatpak coverage).
`required` fails on missing registrations, labels, harness tools, transports or
any skip; `auto` records unavailable coverage rather than claiming a pass.
`ci.yml` passes `--wayland required` in both Linux container jobs. The `evdev`
unit/boundary suites run in the headless gate; uinput stays optional/test-only.
Root containers drop only the sway fixture to their existing unprivileged
`nobody` account; a root host is refused. No host compositor is used.

## Compositor evidence and gaps

| Environment | Observed automated evidence | Not established |
| --- | --- | --- |
| Headless sway/pixman | Native output, foreign-toplevel and click-through layer-shell tests with scale 1/2; XWayland editor walkthrough (5/5) | Physical outputs/GPU, all wlroots-family compositors, real games |
| X11/Xvfb | Existing daemon suites, installed smokes, editor walkthrough (30/30) | Physical GNOME Xorg/KDE X11, real input/gamepad/HiDPI desktops |
| KWin | Private-bus script load/run/unload/authentication and GJS script fixtures | Real Plasma/KWin 5/6, fullscreen stacking, native portal consent |
| GNOME | GJS pure modules and unmodified glue with fake Shell/Mutter objects on a private bus | Real GNOME 45–50, host extension enablement, fullscreen stacking, accessibility user study |
| ScreenCast/PipeWire | Fake portal sequencing/denial/owner loss and private local PipeWire stream lifecycle | Real portal source picker, physical capture/GPU performance |
| evdev | Synthetic event/capability/keymap, idle/recovery and boundary tests | Physical devices, permissions/hotplug and real game input; sandbox device access |

Local complete gate `out/linux-automation/c7h-20261015T100347Z` passed on
`8183a7b` plus the recorded uncommitted c7h integration changes
(`sourceTreeClean=false`), including the Flatpak permissions and strict
base-gate fixture checks: 36 headless, 7 X11, 2 editor, 1 Wayland, 5 D-Bus
and 6 GJS suites, none skipped, plus 88 tooling tests and installed native
and Flatpak smokes. Native/Flatpak editor idle USS was 13.13/12.93 MiB,
both at 0.0% CPU over 20 s (targets 20 MiB / 0.2%). The package stage was off;
Docker Desktop was unavailable for the attempted native-container reruns.
The integration review found no High; its missing-protocol-fixture Medium
was fixed and independently confirmed. Workflow lint passed with the same
checksum-verified actionlint 1.7.12 version pinned by CI (not a CI run).
The latest editor GUI bundles are
`out/linux-gui/c7g-wayland-20261015T065435Z` (5/5) and
`out/linux-gui/c7b-20261015T065624Z` (30/30). CI was not run (nothing pushed).
No VM or physical-compositor evidence, release, signing or publication is claimed.

## Development lanes

| Lane | Branch | Owns |
| --- | --- | --- |
| S – seam | `v27/wl-seam` | `src/daemon/linux/**`, `src/platform/linux/x11_*`, `src/platform/linux/display_types.*`, `src/runtime/**` status, `src/ctl/layerctl.cpp`, existing Linux tests |
| A – wayland | `v27/wl-wayland` | `src/platform/linux/wayland_*`, `third_party/wayland-protocols/`, `cmake/LayerCLinuxWayland.cmake`, `tests/linux_wayland_*`, `tools/wayland-test-session.sh`, Wayland fixtures |
| B – evdev | `v27/wl-evdev` | `src/platform/linux/evdev_*`, `src/platform/linux/keysym_vk.*`, `cmake/LayerCLinuxEvdev.cmake`, `tests/linux_evdev_*` |
| C – bus | `v27/wl-bus` | `src/platform/linux/{dbus,kwin,gnome_bridge,portal,pipewire}_*`, `cmake/LayerCLinuxBus.cmake`, `tests/linux_{dbus,kwin,gnome_bridge,portal,pipewire}_*`, D-Bus fakes |
| D – gnome | `v27/wl-gnome` | `packaging/gnome-shell/**`, `cmake/LayerCGnomeShell.cmake`, `tests/gjs/**` |
| I – integration | `v27/wayland` | provider wiring in `DisplayAdapters`, CI, `docs/Linux.md`, `README.md`, `tools/linux-deps.txt`, Flatpak |
