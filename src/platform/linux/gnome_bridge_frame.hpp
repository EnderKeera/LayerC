#pragma once

// The full-replacement frame layerd hands the LayerC GNOME Shell extension
// through io.github.EnderKeera.LayerC.Overlay1.Present (docs/Wayland.md,
// "GNOME Shell extension"). Pure: the bus client only transports the bytes.

#include "display_types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace layerc::platform {

inline constexpr std::size_t gnome_frame_max_panels = 64;
// The largest legal typed panel, 32 rows of progress fill, outline, eight pips
// and label (2 + 32 * 11 = 354 ops), fits; the JS validator uses the same bound.
inline constexpr std::size_t gnome_frame_max_ops = 512;
inline constexpr std::size_t gnome_frame_max_bytes = 64 * 1024;

struct GnomePanel {
  std::string id;
  OverlayDisplayList list;
};

// {"version":1,"panels":[{"id":"…","x":..,…,"ops":[…]}]} through core
// json::append_*. nullopt when a bound is exceeded; the caller reports the
// overlay provider degraded instead of sending a truncated frame.
[[nodiscard]] std::optional<std::string>
build_gnome_overlay_frame(std::span<const GnomePanel> panels);

} // namespace layerc::platform
