# Third-party notices

This file records vendored-code notices only.

## miniz 3.1.2

See `third_party/miniz/UPSTREAM.md` for its pinned tag, commit and SHA-256
values and `third_party/miniz/LICENSE` for the MIT text. Installs ship both
files under `licenses/miniz/` in the Windows ZIP and under
`share/licenses/LayerC/miniz/` on Linux and in the Flatpak.

## FLTK 1.4.5

See `third_party/fltk/UPSTREAM.md` for the pinned release archive, commit and
per-file SHA-256 values. `third_party/fltk/COPYING` contains the GNU Library
General Public License, version 2, with the FLTK exception permitting static
linking. The unmodified snapshot also retains upstream bundled image
libraries and their notices in `jpeg/`, `png/` and `zlib/`.

The Windows FLTK trial statically links only the base FLTK library, without
OpenGL or the image libraries; it is not yet installed or shipped. The
existing Win32 editor is unchanged. Native Linux uses the distribution's
FLTK (1.4 or later); the Flatpak builds the checksum-pinned 1.4.5 archive.
FLTK licensing and provenance ship under `licenses/fltk/` on Windows and
`share/licenses/LayerC/fltk/` on Linux and in the Flatpak.
