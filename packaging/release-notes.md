LayerC 2.7.0 — development notes (not published)

This local development version folds typed overlay items into the Wayland
preview. It does not announce a release, new supported desktop, physical
hardware qualification, signing or certification.

- Panels can mix cooldowns, timers, numeric counters and boolean indicators.
  Timer items can observe automation timers or a cooldown's optional active
  duration, which starts only when that cooldown accepts a trigger.
- Counters support numbers or up to eight pips. Indicators support per-item
  flashing: off, or 0.5–4 Hz. Missing or mistyped sources stay visibly
  unavailable rather than being guessed.
- Shared presentation drives Win32 GDI, X11 Shape, the in-development
  Wayland/GNOME display list and Game Bar's eight fixed panel slots.
  Game Bar snapshots remain bounded to 64 KiB and clear after two seconds
  without fresh data.
- Both native editors provide ordered items, item properties, a timer preset
  and cooldown active-duration editing, with a live preview/simulator.
- New item features use profile schema 5. Existing canonical legacy profiles
  keep their old serialization; unknown item kinds are retained but not
  rendered and remain visible in diagnostics.

The existing Windows and Linux X11 support boundaries are unchanged.
Wayland remains an in-development preview; Gamescope is unsupported.
Physical desktop and Game Bar manual/signing/certification evidence is still
separate from automated fixtures. See `docs/Linux.md`, `docs/Wayland.md` and
`docs/GameBarCompanion.md`.

Packaging still follows the version-bound CI/release gates; no 2.7.0 package
is published by this work. The Windows launcher remains omitted until a
release-signing key is provisioned, and its self-update is off by default.
