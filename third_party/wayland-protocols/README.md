# Vendored Wayland protocols

Unmodified copies from the wlr-protocols repository
(<https://gitlab.freedesktop.org/wlroots/wlr-protocols>, `unstable/`), kept
with their license headers. They are compiled by `wayland-scanner` into
client glue for the optional Wayland lane (`cmake/LayerCLinuxWayland.cmake`).

| File | Interfaces used | License |
| --- | --- | --- |
| `wlr-layer-shell-unstable-v1.xml` | `zwlr_layer_shell_v1`, `zwlr_layer_surface_v1` | MIT-style (in file) |
| `wlr-foreign-toplevel-management-unstable-v1.xml` | `zwlr_foreign_toplevel_manager_v1`, `zwlr_foreign_toplevel_handle_v1` (read-only) | MIT-style (in file) |

`xdg-shell` and `xdg-output` come from the system `wayland-protocols`.
