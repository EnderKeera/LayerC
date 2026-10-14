#include "gnome_bridge_frame.hpp"

#include "layerc/core/json.hpp"

namespace layerc::platform {

std::optional<std::string> build_gnome_overlay_frame(const std::span<const GnomePanel> panels) {
  if (panels.size() > gnome_frame_max_panels) return std::nullopt;
  std::string out = "{\"version\":1,\"panels\":[";
  std::string list;
  bool first = true;
  for (const auto &panel : panels) {
    if (panel.list.ops.size() > gnome_frame_max_ops) return std::nullopt;
    if (!first) out += ',';
    first = false;
    out += "{\"id\":";
    layerc::core::json::append_quoted(out, panel.id);
    list.clear();
    append_overlay_display_list_json(list, panel.list);
    // The display list is an object; splice its members after the id.
    out += ',';
    out.append(list, 1);
    if (out.size() > gnome_frame_max_bytes) return std::nullopt;
  }
  out += "]}";
  if (out.size() > gnome_frame_max_bytes) return std::nullopt;
  return out;
}

} // namespace layerc::platform
