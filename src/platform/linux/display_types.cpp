#include "display_types.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/panel_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace layerc::platform {

int dpi_from_geometry(const int width, const int height, const int mm_width,
                      const int mm_height) noexcept {
  if (width <= 0 || height <= 0 || mm_width <= 0 || mm_height <= 0)
    return baseline_dpi;
  const double horizontal =
      static_cast<double>(width) * 25.4 / static_cast<double>(mm_width);
  const double vertical =
      static_cast<double>(height) * 25.4 / static_cast<double>(mm_height);
  const int dpi = static_cast<int>(std::lround((horizontal + vertical) / 2.0));
  return std::clamp(dpi, min_dpi, max_dpi);
}

const Monitor *monitor_for_rect(const MonitorScan &scan, const int x,
                                const int y, const int width,
                                const int height) noexcept {
  if (scan.monitors.empty()) return nullptr;
  const Monitor *best = nullptr;
  std::int64_t best_overlap = 0;
  for (const auto &monitor : scan.monitors) {
    const int left = std::max(x, monitor.x);
    const int right = std::min(x + width, monitor.x + monitor.width);
    const int top = std::max(y, monitor.y);
    const int bottom = std::min(y + height, monitor.y + monitor.height);
    const auto overlap = static_cast<std::int64_t>(std::max(0, right - left)) *
                         static_cast<std::int64_t>(std::max(0, bottom - top));
    if (best == nullptr || overlap > best_overlap) {
      best = &monitor;
      best_overlap = overlap;
    }
  }
  if (best_overlap > 0 && best != nullptr) return best;
  for (const auto &monitor : scan.monitors)
    if (monitor.primary) return &monitor;
  return &scan.monitors.front();
}

int monitor_dpi_for_rect(const MonitorScan &scan, const int x, const int y,
                         const int width, const int height) noexcept {
  const auto *monitor = monitor_for_rect(scan, x, y, width, height);
  return monitor == nullptr ? baseline_dpi : monitor->dpi;
}

int scaled_pixels(const int value, const int dpi) noexcept {
  return (value * dpi + baseline_dpi / 2) / baseline_dpi;
}

OverlayRect panel_rect(const OverlayFrame &frame) {
  const int dpi = frame.dpi;
  const auto geometry =
      layerc::core::panel_geometry(frame.panel, frame.bars.size());
  const int width = std::max(scaled_pixels(geometry.size.width, dpi), 1);
  const int height = std::max(scaled_pixels(geometry.size.height, dpi), 1);
  OverlayRect rect;
  rect.width = width;
  rect.height = height;
  rect.x = frame.target.x +
           static_cast<int>(
               std::lround(frame.panel.anchor_x *
                           static_cast<double>(frame.target.width))) -
           width / 2;
  rect.y = frame.target.y +
           static_cast<int>(
               std::lround(frame.panel.anchor_y *
                           static_cast<double>(frame.target.height))) -
           height / 2;
  return rect;
}

namespace {

[[nodiscard]] OverlayRect scaled_rect(const layerc::core::PanelRect &rect,
                                      const int dpi) noexcept {
  return {scaled_pixels(rect.left, dpi), scaled_pixels(rect.top, dpi),
          scaled_pixels(rect.width(), dpi), scaled_pixels(rect.height(), dpi)};
}

void append_int(std::string &out, const int value) {
  layerc::core::json::append_number(out, static_cast<double>(value));
}

[[nodiscard]] const char *op_name(const OverlayOpKind kind) noexcept {
  switch (kind) {
  case OverlayOpKind::fill_rect: return "fill";
  case OverlayOpKind::stroke_rect: return "stroke";
  case OverlayOpKind::text: return "text";
  }
  return "fill";
}

} // namespace

OverlayDisplayList build_overlay_display_list(const OverlayFrame &frame) {
  OverlayDisplayList list;
  list.bounds = panel_rect(frame);
  if (list.bounds.width <= 0 || list.bounds.height <= 0) return list;

  const auto &panel = frame.panel;
  const bool bar_style = panel.style == layerc::core::PanelStyle::bar;
  const std::uint32_t background =
      bar_style ? panel.bar.background_color : 0x272D35U;
  const std::uint32_t border = bar_style ? panel.bar.border_color : 0x50BAC8U;
  const std::uint32_t fill = bar_style ? panel.bar.fill_color : 0x50BAC8U;
  const std::uint32_t text_color =
      bar_style ? panel.bar.text_color : 0xE4E8ECU;
  const bool show_text = !bar_style || panel.bar.show_text;
  const int font_px = scaled_pixels(overlay_font_dip, frame.dpi);

  list.ops.push_back({OverlayOpKind::fill_rect,
                      {0, 0, list.bounds.width, list.bounds.height},
                      background, {}, 0});
  list.ops.push_back({OverlayOpKind::stroke_rect,
                      {0, 0, list.bounds.width, list.bounds.height},
                      border, {}, 0});

  const auto geometry =
      layerc::core::panel_geometry(panel, frame.bars.size());
  for (std::size_t index = 0; index < frame.bars.size(); ++index) {
    const auto &bar = frame.bars[index];
    auto item = geometry.items[index];
    if (bar_style) {
      const auto shape = layerc::core::bar_geometry(
          panel, index, frame.bars.size(), bar.remaining);
      item = shape.outline;
      if (shape.fill.right > shape.fill.left &&
          shape.fill.bottom > shape.fill.top)
        list.ops.push_back(
            {OverlayOpKind::fill_rect,
             {scaled_pixels(shape.fill.left, frame.dpi),
              scaled_pixels(shape.fill.top, frame.dpi),
              std::max(1, scaled_pixels(shape.fill.width(), frame.dpi)),
              std::max(1, scaled_pixels(shape.fill.height(), frame.dpi))},
             fill, {}, 0});
    } else {
      // Xlib draws (w-1)x(h-1) outlines that cover w x h pixels; keep that
      // covered area as the op rect so every renderer agrees.
      const auto scaled = scaled_rect(item, frame.dpi);
      list.ops.push_back({OverlayOpKind::stroke_rect,
                          {scaled.x, scaled.y,
                           std::max(1, scaled.width - 1) + 1,
                           std::max(1, scaled.height - 1) + 1},
                          border, {}, 0});
    }
    if (!show_text || bar.label.empty()) continue;
    list.ops.push_back({OverlayOpKind::text, scaled_rect(item, frame.dpi),
                        text_color, bar.label, font_px});
  }
  return list;
}

void append_overlay_display_list_json(std::string &out,
                                      const OverlayDisplayList &list) {
  namespace json = layerc::core::json;
  out += "{\"x\":";
  append_int(out, list.bounds.x);
  out += ",\"y\":";
  append_int(out, list.bounds.y);
  out += ",\"width\":";
  append_int(out, list.bounds.width);
  out += ",\"height\":";
  append_int(out, list.bounds.height);
  out += ",\"ops\":[";
  bool first = true;
  for (const auto &op : list.ops) {
    if (!first) out += ',';
    first = false;
    out += "{\"op\":";
    json::append_quoted(out, op_name(op.kind));
    out += ",\"x\":";
    append_int(out, op.rect.x);
    out += ",\"y\":";
    append_int(out, op.rect.y);
    out += ",\"width\":";
    append_int(out, op.rect.width);
    out += ",\"height\":";
    append_int(out, op.rect.height);
    out += ",\"color\":";
    json::append_color_quoted(out, op.rgb);
    if (op.kind == OverlayOpKind::text) {
      out += ",\"text\":";
      json::append_quoted(out, op.text);
      out += ",\"fontPx\":";
      append_int(out, op.font_px);
    }
    out += '}';
  }
  out += "]}";
}

} // namespace layerc::platform
