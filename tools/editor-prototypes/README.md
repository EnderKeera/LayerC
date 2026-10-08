# Linux editor toolkit experiments

This standalone CMake project is not part of the shipped build. It compares
FLTK 1.4 and GTK 4 against the same representative document/store operations.
[LinuxEditorPlan.md](../../docs/LinuxEditorPlan.md) owns the gates and results.

On a Debian development machine, use CMake/Ninja and the FLTK 1.4, GTK 4 and
ATK/AT-SPI development packages. The accessibility extension requires
ATK/AT-SPI ≥2.62 (`libatk1.0-dev`, `libatk-bridge2.0-dev` and their development
dependencies, including `libxres-dev`). Older adaptors have upstream cache
weak-reference cleanup defects (GNOME #213/#231). The verified experiment used
Debian trixie-backports packages extracted into a private prefix, without a
system installation. Its probe also needs
`python3-pyatspi`, Xvfb, Openbox, xdotool, x11-utils and D-Bus.

```sh
cmake -S tools/editor-prototypes -B out/editor-prototypes -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/editor-prototypes --parallel
bash tools/editor-prototypes/bridge-test.sh \
  "$PWD/out/editor-prototypes/layere_proto_fltk_a11y" \
  "$PWD/out/linux-gui/bridge-probe"
LAYERD="$PWD/build-x11/layerd" bash tools/editor-prototypes/bridge-measure.sh \
  "$PWD/out/editor-prototypes/layere_proto_fltk_a11y" \
  "$PWD/out/linux-gui/bridge-resource"
python3 tools/editor-prototypes/comparison_bundle.py \
  "$PWD/out/editor-prototypes/layere_proto_fltk_a11y" \
  "$PWD/out/linux-gui/prototype-packages"
```

Each probe uses an authenticated private Xvfb display, a private D-Bus session
and disposable XDG profile data. Never point these scripts at a production
display/store. The executables refuse to scan or save unless
`private_session.sh` created a private, owned fixture root with a matching
random marker, every HOME/XDG root resolves inside it, it contains no
symlink, and `LAYERC_STORE_ROOT` equals the platform-resolved store. They
report `fixture={store, profiles, cooldowns, panels}`, which `measure.sh`
checks for each scenario. After that check they pin the fixture root and
confine themselves with Landlock ABI >= 3 (required; they refuse without it):
writes only beneath the fixture (and to `/dev/null`); reads only beneath the
fixture, system trees, the canonical directories of loaded libraries and four
device nodes; directory listing for the durable store's fsync barrier. No grant
may include the real home or overlap the production configuration/data roots
that `private_session.sh` records in `LAYERC_PROTO_PRODUCTION_ROOTS`. Documents
are read only through single-link inodes. The guard prevents accidental
production access; it does not defend against a concurrent same-user adversary.
`layere_proto_confinement` demonstrates the denials and a confined durable Save.
Every long-lived harness process runs under `owned.py` (via `owned.sh`), which
keeps it unreaped, always stops and reaps it (also on internal failure, and
including detached descendants in `--group` mode), and stops when its control
FIFO (its stdin) closes; no script signals a numeric PID. The measurement sampler alone owns the application, publishes its
PID only after exec, and binds its per-run report to a nonce plus the
application's PID/start time/executable; the shell verifies that identity before
any `/proc` read. `measure.sh` requires a new or empty output directory.
`layout-test.sh` checks native editing/Save, key-capture cancellation and
Tab/Shift+Tab traversal, requested-size achievement and non-overlap at 1x/2x.
`bridge-test.json` reports failures as failures and records semantic actions
and events. The probe also checks successful window close, with GLib warnings
made fatal, and an explicitly disabled bridge with visible fault diagnostics.
`bridge-measure.sh` retains a subscribed accessibility test client
while running the empty/representative/100-cycle scenarios; that client's
process and desktop/session services are harness overhead, not editor-owned
helpers. The bridge uses ATK's AT-SPI adaptor without GTK widgets and integrates
GLib readiness descriptors/deadlines into FLTK's blocking UI loop.

**Prototype limitations:** main-window controls only; no exported popup/modal
surfaces, dynamic list-child reconciliation, exact row/tab hit testing,
complete editable-text/selection interfaces or verified Orca experience.
The cycle driver is internal (not a user-driven modal walkthrough); each
cycle opens the real validation dialog and dismisses it after it renders.
`measure.sh` establishes its sampler before exec and samples every 5 ms through
readiness/settling. It waits for a synchronized pre-cycle baseline before
releasing the 100 cycles. Sampled peaks do not prove sub-interval allocation
peaks. One-second smoke runs are not idle-CPU acceptance evidence. Measure
candidates separately for acceptance: a concurrent peer exiting can change
shared/private page accounting. Dependency closure bytes and the deterministic
same-host comparison archives are not release-package sizes. The archives omit
host interpreters, fonts, assets/plugins and session services, record checksums
and verify executable readback; they do not establish native/Flatpak installation
or cross-distribution compatibility.
These experiments support the recorded toolkit selection only. They do not
establish full editor accessibility, production feature parity or release-gate
acceptance; those require the later implementation and packaging evidence.
