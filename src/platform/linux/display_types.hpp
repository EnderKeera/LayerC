#pragma once

// Display-server-neutral values shared by the X11, Wayland, evdev, portal and
// GNOME Shell adapters. No Xlib, wayland-client, D-Bus or PipeWire header may
// reach this file: every backend converts into these types at its boundary.

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {

// Panel geometry is authored at 96 dpi (Windows MulDiv parity), so every
// backend converts sizes before they reach the display server.
inline constexpr int baseline_dpi = 96;
// Physical monitor sizes are frequently 0 or absurd; keep the derived scale
// inside a range a UI can survive.
inline constexpr int min_dpi = 72;
inline constexpr int max_dpi = 288;

// One top-level window as a tracking provider reported it. `window` is an
// opaque, nonzero identity that stays stable while the window lives: an XID
// on X11, a provider-assigned id on Wayland. Identity fields are empty when
// the provider does not publish them, and executable resolution degrades
// silently. Geometry is in the provider's global logical coordinates.
struct WindowSnapshot {
  std::uint64_t window{};
  std::string title;
  std::string class_name;
  std::string instance_name;
  std::optional<std::uint32_t> pid;
  std::string executable_name;
  std::string executable_path;
  int x{};
  int y{};
  int width{};
  int height{};
  bool active{};
  bool fullscreen{};
  bool hidden{};
  // The XWayland XID a compositor bridge reported, so X11 capture can still
  // address the window; absent for native clients and other providers.
  std::optional<std::uint64_t> x11_window;

  bool operator==(const WindowSnapshot &) const = default;
};

struct WindowScan {
  // True when the list came from an authoritative source (EWMH client list,
  // compositor script/extension, foreign-toplevel protocol); false for a
  // best-effort fallback the caller must report as degraded.
  bool authoritative{};
  std::vector<WindowSnapshot> windows;
  std::optional<std::uint64_t> active;
  // Set when the list is missing, unreadable, or a query failed: the caller
  // surfaces it instead of silently tracking a partial list.
  std::string warning;
  std::uint32_t truncated{};

  bool operator==(const WindowScan &) const = default;
};

// One window for a profile under the shared core::resolve_game_match
// policy: every specified identity field must hold (byte-exact executable
// path), the active window wins, and a previous window breaks background
// ties so losing focus does not change an existing session's target.
[[nodiscard]] const WindowSnapshot *
match_window(const WindowScan &scan, const layerc::core::GameMatch &match,
             std::uint64_t previous_window = 0);

// Resolved /proc/<pid>/exe for PIDs owned by the effective UID (root may read
// any). Empty for every other case — the caller degrades to title/class.
[[nodiscard]] std::string process_executable_path(std::uint32_t pid);
[[nodiscard]] std::string path_basename(std::string_view path);

// Upper bound every provider applies before a scan reaches the registry.
inline constexpr std::uint32_t window_scan_limit = 64;

struct Monitor {
  std::string name;
  int x{};
  int y{};
  int width{};
  int height{};
  bool primary{};
  int dpi{baseline_dpi};
  // Integer buffer scale for backends that render client-side (Wayland
  // wl_output.scale); 1 on X11.
  int buffer_scale{1};

  bool operator==(const Monitor &) const = default;
};

struct MonitorScan {
  bool available{};
  std::vector<Monitor> monitors;
  // Set when the monitor source is missing or a query failed: the caller
  // publishes it and geometry falls back to the 96 dpi baseline.
  std::string warning;

  bool operator==(const MonitorScan &) const = default;
};

// Mean of the horizontal and vertical DPI, clamped; unknown physical size
// (0 mm) yields the baseline.
[[nodiscard]] int dpi_from_geometry(int width, int height, int mm_width,
                                   int mm_height) noexcept;

// Largest-overlap monitor wins; the primary, then the first monitor, break
// ties. An empty scan yields the baseline.
[[nodiscard]] int monitor_dpi_for_rect(const MonitorScan &scan, int x, int y,
                                      int width, int height) noexcept;

// The largest-overlap monitor itself (same tie rules); nullptr when empty.
[[nodiscard]] const Monitor *monitor_for_rect(const MonitorScan &scan, int x,
                                              int y, int width,
                                              int height) noexcept;

[[nodiscard]] int scaled_pixels(int value, int dpi) noexcept;

struct OverlayRect {
  int x{};
  int y{};
  int width{};
  int height{};

  bool operator==(const OverlayRect &) const = default;
};

// One cooldown bar inside a panel. `remaining` is the fraction of the
// cooldown still running: 1 right after a trigger, 0 once it is ready.
struct OverlayBar {
  std::string label;
  double remaining{};
  bool running{};

  bool operator==(const OverlayBar &) const = default;
};

// Everything one overlay needs for one paint: the game window it is anchored
// to (global coordinates), the panel, its bars and the monitor scale.
struct OverlayFrame {
  OverlayRect target;
  layerc::core::Panel panel;
  std::vector<OverlayBar> bars;
  int dpi{baseline_dpi};
  bool anchor_active{};
};

// Panel geometry in device pixels: authored at 96 dpi, anchored inside the
// target window. Pure, so the geometry rules are verifiable without a server.
[[nodiscard]] OverlayRect panel_rect(const OverlayFrame &frame);

// The one paint model every overlay renderer consumes (Xlib, cairo on a
// layer-shell buffer, the GNOME Shell extension). Ops are in panel-local
// device pixels and painted in order onto an opaque surface of bounds' size.
enum class OverlayOpKind : std::uint8_t {
  // Solid rectangle.
  fill_rect,
  // One-device-pixel outline drawn inside `rect` (Xlib XDrawRectangle with
  // width-1/height-1 covers exactly `rect`).
  stroke_rect,
  // UTF-8 `text` centred horizontally and vertically inside `rect`, at
  // `font_px` pixel size in the renderer's monospace face.
  text,
};

struct OverlayOp {
  OverlayOpKind kind{OverlayOpKind::fill_rect};
  OverlayRect rect;
  // 0xRRGGBB, opaque.
  std::uint32_t rgb{};
  std::string text;
  int font_px{};

  bool operator==(const OverlayOp &) const = default;
};

struct OverlayDisplayList {
  // panel_rect(frame): global position and device-pixel size.
  OverlayRect bounds;
  std::vector<OverlayOp> ops;

  bool operator==(const OverlayDisplayList &) const = default;
};

// Unscaled text size in DIPs; renderers receive scaled_pixels(.., dpi).
inline constexpr int overlay_font_dip = 13;

// Builds the paint model for one frame with the exact rules the X11 overlay
// has always used (background, border, bar fills or item outlines, centred
// labels; text only when the panel shows it). An empty or degenerate panel
// yields zero-sized bounds and no ops.
[[nodiscard]] OverlayDisplayList
build_overlay_display_list(const OverlayFrame &frame);

// Byte-stable JSON for one display list (core json::append_* only):
// {"x":..,"y":..,"width":..,"height":..,"ops":[{"op":"fill"|"stroke"|"text",
// "x":..,"y":..,"width":..,"height":..,"color":"#RRGGBB"[,"text":..,
// "fontPx":..]}]}
void append_overlay_display_list_json(std::string &out,
                                      const OverlayDisplayList &list);

} // namespace layerc::platform
