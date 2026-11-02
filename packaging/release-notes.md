# LayerC 2.7.0

## Typed overlay items

Panels can now mix **cooldowns, timers, counters, and indicators** rather than
showing only cooldowns.

- Show a cooldown's active-duration window or a declared automation timer.
- Bind counters to numeric automation variables, displayed as a number or up
  to eight pips.
- Bind indicators to Boolean or numeric variables, with optional flashing.
- Add, reorder, relabel, and configure items in both Windows and Linux editors.
- Apply item-scoped visibility, text, and color overrides through automation.
- Use the same presentation on Win32, X11, Wayland/GNOME preview frames, and
  Game Bar, including item effects on legacy cooldown panels.

Legacy cooldown panels retain their existing format when new presentation
features are not used. Unknown item kinds are preserved and reported through
diagnostics. Missing or incompatible sources remain visibly unavailable.

## Linux editor and packages

This release includes the native X11 `layere` editor: profiles and matching,
cooldowns and bindings, panels, automation, visual calibration, live drafts,
runtime diagnostics, appearance, and typed items.

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
- The Windows ZIP starts through `layerd.exe`; `layerc.exe` remains omitted
  until a release-signing key is provisioned. Launcher self-updates remain OFF
  by default.
- Profiles using typed items require schema-5 support. Move profiles between
  platforms through validated JSON or `.layercprofile` bundles, not by copying
  platform stores.
