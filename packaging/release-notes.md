LayerC 2.6.2 (v2.6c) adds the Linux `layere` profile editor for X11 and
publishes the first Linux packages.

**Windows x64** (`layerc-windows-x64.zip`): start LayerC with `layerd.exe`.
`layerc.exe` is omitted until a release-signing key is provisioned, and the
launcher's self-update is OFF by default.

**Linux X11** (`layerc_2.6.2-1_amd64.deb`, `layerc-2.6.2-x86_64.flatpak`):
the `.deb` targets Debian 13 with AT-SPI 2.62 from trixie-backports; the
Flatpak bundle uses the Flathub `org.freedesktop.Platform//25.08` runtime.
Both are the exact packages the release gates built, installed and smoke
tested in containers and under Xvfb. Physical desktop evidence (GNOME Xorg,
KDE Plasma X11) is still pending; see `docs/Linux.md`.

Wayland and Gamescope are not supported in this release. `SHA256SUMS` lists
every asset.
