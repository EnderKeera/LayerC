# Test-only Wayland protocols

Unmodified copies from wlr-protocols (<https://gitlab.freedesktop.org/wlroots/wlr-protocols>,
`unstable/`), with their license headers. They are compiled only into the
`wayland`-labelled test fixtures, never into `layerd`:

- `wlr-screencopy-unstable-v1.xml`: reads output pixels to prove an overlay is mapped.
- `wlr-virtual-pointer-unstable-v1.xml`: synthesizes pointer input inside the
  disposable headless compositor to prove overlays are click-through. Like
  XTest and uinput, it must never link into `layerd`.
