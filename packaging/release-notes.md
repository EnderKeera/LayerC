# LayerC 2.7.2

LayerC 2.7.2 is a patch release on 2.7.1. It adds the launcher update setting
to the Windows editor. Linux packages carry the new version number and are
otherwise unchanged. The 2.7.1 notes follow below; for typed overlay items and
the Wayland preview, see the 2.7.0 release notes.

## Update setting in the Windows editor

- Tools > Options has a new **Install updates when LayerC starts** check box.
  It turns the `layerc.exe` launcher's signed self-updates on or off, so you no
  longer need to edit the registry.
- The check box sets the same `HKCU\Software\LayerC\Updates` value as before.
  If that value is unset or unreadable, it shows as off, matching the
  launcher's default.
- Updates remain **OFF by default**. The change applies the next time the
  launcher starts LayerC. `layere` itself still never accesses the network.

## From 2.7.1: Signed launcher updates

- The Windows ZIP now also contains the `layerc.exe` launcher. Starting LayerC
  directly through `layerd.exe` still works as before.
- Launcher self-updates remain **OFF by default**. With updates OFF, the
  launcher makes no network requests and cannot download a first build.
- After opting in, the launcher follows tagged releases. Each release
  publishes an update feed signed with the release ECDSA P-256 key. The
  launcher verifies the signature before it uses any file and never moves to
  an older release.
- `layerd`, `layere`, `layercal`, and `layerw` still never access the network.

## From 2.7.1: Accessible editor design system

Both LayerE editors now follow design system v2:

- Status marks are drawn shapes whose meaning comes from the operation's
  outcome, not its wording. Status colors and a secondary accent meet contrast
  floors on every theme preset.
- Visual flashing is capped at 3 Hz in every renderer. Profiles keep their
  stored rate.
- **Windows:** consistent dialog layout and OK, Cancel, Apply button order; a
  live contrast warning in Appearance; a Keyboard shortcuts dialog; full accent
  focus outlines; and UI Automation names and descriptions on controls and
  color swatches.
- **Linux:** tokenized controls, accent focus rings, desktop text metrics with
  app-local scaling, a Keyboard shortcuts dialog, and AT-SPI names, hints and
  announcements.

## Packages

The release workflow publishes the Windows x64 ZIP, native Debian package,
and editor Flatpak bundle that its package gates built and tested, plus
`SHA256SUMS`. It does not substitute unchecked local packages.

## Scope and known limitations

- Windows and Linux X11/headless support boundaries are unchanged. Dated
  automated and virtual-desktop evidence is not physical-desktop qualification.
- Native Wayland is an **unqualified preview**. Global passive evdev input is
  opt-in and requires the user's existing `input` group membership. Gamescope
  remains unsupported.
- Game Bar's real-host manual matrix, production signing, and certification
  remain pending. Package generation does not close those requirements.
- 2.7.2 is the first release that an opted-in 2.7.1 launcher can update to.
  No opted-in update on a real machine has been recorded yet.
- Profiles using typed items require schema-5 support. Move profiles between
  platforms through validated JSON or `.layercprofile` bundles, not by copying
  platform stores.
