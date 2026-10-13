#include "editor_app.hpp"
#include "layerc/core/panel_geometry.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace layerc::editor {
namespace {
struct PlacedPanel {
  layerc::core::PanelGeometry geometry;
  int left{}, top{};
  double scale{};
};
// The area anchors span: the whole preview, or the target window's frame
// (game pixels scaled to the preview) while positioning on a window.
PlacementFrame frame_of(const Fl_Widget &view, const EditorApp &app) {
  if (app.placement_active())
    return placement_frame(view.x() + 5, view.y() + 5, view.w() - 10, view.h() - 10,
                           app.placement()->width, app.placement()->height);
  return {view.x(), view.y(), view.w(), view.h(), 0.0};
}
PlacedPanel placement(const layerc::core::Panel &panel, const PlacementFrame &frame) {
  auto geometry = layerc::core::panel_geometry(panel, panel.cooldown_ids.size());
  // Positioning shows bars at the game's scale; otherwise they fit the preview.
  const auto scale = frame.scale > 0.0
      ? frame.scale
      : std::min({1.0, static_cast<double>(frame.width - 10) / std::max(geometry.size.width, 1),
                  static_cast<double>(frame.height - 10) / std::max(geometry.size.height, 1)});
  const auto width = static_cast<int>(geometry.size.width * scale);
  const auto height = static_cast<int>(geometry.size.height * scale);
  if (frame.scale > 0.0) {
    const auto origin = placed_origin(frame, panel.anchor_x, panel.anchor_y, width, height);
    return {std::move(geometry), origin.x, origin.y, scale};
  }
  return {std::move(geometry),
          frame.x + static_cast<int>(panel.anchor_x * std::max(frame.width - width, 0)),
          frame.y + static_cast<int>(panel.anchor_y * std::max(frame.height - height, 0)), scale};
}
Fl_Color rgb(std::uint32_t color) {
  return fl_rgb_color(static_cast<unsigned char>(color >> 16),
                      static_cast<unsigned char>(color >> 8), static_cast<unsigned char>(color));
}
}
PanelPreview::PanelPreview(int x, int y, int width, int height, EditorApp &app)
    : Fl_Widget(x, y, width, height, "Local panel preview"), app_(app) {
  color(fl_rgb_color(31, 36, 41)); // the Preview role's default; Appearance may override it
}
void PanelPreview::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_rectf(x(), y(), w(), h(), color());
  const auto frame = frame_of(*this, app_);
  if (app_.placement_active()) {
    // The target window's outline, named so the mapping is explicit.
    fl_color(fl_rgb_color(120, 130, 140));
    fl_rect(frame.x, frame.y, frame.width, frame.height);
    fl_font(FL_HELVETICA, 11);
    const auto &target = *app_.placement();
    const auto caption = "Positioning on " + (target.title.empty() ? std::string("window") : target.title) +
                         " (" + std::to_string(target.width) + "x" + std::to_string(target.height) + ")";
    fl_draw(caption.c_str(), frame.x + 4, frame.y + 14);
  }
  if (app_.session.has_document()) {
    const auto &profile = app_.session.document().profile();
    for (const auto &panel : profile.panels) {
      const auto placed = placement(panel, frame);
      for (std::size_t index = 0; index < placed.geometry.items.size(); ++index) {
        const auto &item = placed.geometry.items[index];
        const int left = placed.left + static_cast<int>(item.left * placed.scale);
        const int top = placed.top + static_cast<int>(item.top * placed.scale);
        const int width = std::max(1, static_cast<int>(item.width() * placed.scale));
        const int height = std::max(1, static_cast<int>(item.height() * placed.scale));
        fl_rectf(left, top, width, height, rgb(panel.bar.background_color));
        fl_rectf(left + 2, top + 2, std::max(width - 4, 1), std::max(height - 4, 1), rgb(panel.bar.fill_color));
        fl_color(rgb(panel.bar.border_color)); fl_rect(left, top, width, height);
        if (panel.style != layerc::core::PanelStyle::bar || panel.bar.show_text) {
          const auto &id = panel.cooldown_ids[index];
          const auto found = std::ranges::find_if(profile.cooldowns, [&](const auto &cd) { return cd.id == id; });
          const auto &name = found == profile.cooldowns.end() ? id : found->name;
          fl_font(FL_HELVETICA, std::max(8, static_cast<int>(12 * placed.scale)));
          fl_color(rgb(panel.bar.text_color));
          fl_draw(name.c_str(), left + 3, top + 3, std::max(width - 6, 1),
                  std::max(height - 6, 1), FL_ALIGN_INSIDE | FL_ALIGN_CENTER);
        }
      }
      if (panel.id == app_.selected_panel()) {
        fl_color(FL_WHITE);
        fl_rect(placed.left, placed.top,
                static_cast<int>(placed.geometry.size.width * placed.scale),
                static_cast<int>(placed.geometry.size.height * placed.scale));
      }
    }
  }
  fl_pop_clip();
}
int PanelPreview::handle(int event) {
  if (!app_.session.has_document()) return Fl_Widget::handle(event);
  if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
    if (!app_.apply_fields()) return 1;
    const auto frame = frame_of(*this, app_);
    for (const auto &panel : app_.session.document().profile().panels) {
      const auto placed = placement(panel, frame);
      if (Fl::event_x() >= placed.left && Fl::event_y() >= placed.top &&
          Fl::event_x() < placed.left + placed.geometry.size.width * placed.scale &&
          Fl::event_y() < placed.top + placed.geometry.size.height * placed.scale) {
        dragging_ = panel.id; app_.panel_id_ = panel.id; app_.session.end_text_edit();
        app_.refresh(); return 1;
      }
    }
  } else if (event == FL_DRAG && !dragging_.empty()) {
    const auto frame = frame_of(*this, app_);
    const auto error = app_.session.edit([&](auto &profile) {
      for (auto &panel : profile.panels) if (panel.id == dragging_) {
        const auto placed = placement(panel, frame);
        const auto width = static_cast<int>(placed.geometry.size.width * placed.scale);
        const auto height = static_cast<int>(placed.geometry.size.height * placed.scale);
        if (frame.scale > 0.0) { // positioning: the cursor is the overlay's centre
          const auto anchor = anchor_at(frame, Fl::event_x(), Fl::event_y());
          panel.anchor_x = anchor.x;
          panel.anchor_y = anchor.y;
          continue;
        }
        panel.anchor_x = std::clamp(static_cast<double>(Fl::event_x() - frame.x - width / 2) /
                                      std::max(frame.width - width, 1), 0.0, 1.0);
        panel.anchor_y = std::clamp(static_cast<double>(Fl::event_y() - frame.y - height / 2) /
                                      std::max(frame.height - height, 1), 0.0, 1.0);
      }
    }, true);
    if (error) app_.report(*error);
    else {
      app_.refresh_forms();
      app_.report(app_.placement_active() ? "Positioning on the window: the live draft moves LayerD's overlay."
                                          : "Unsaved preview placement.");
      redraw();
    }
    return 1;
  } else if (event == FL_RELEASE && !dragging_.empty()) {
    dragging_.clear(); app_.session.end_text_edit(); return 1;
  }
  return Fl_Widget::handle(event);
}
} // namespace layerc::editor
