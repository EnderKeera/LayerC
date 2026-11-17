#include "editor_app.hpp"
#include "editor_style.hpp"
#include "accessible_widgets.hpp"
#include "layerc/core/panel_geometry.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "automation_simulator.hpp"
#include "display_types.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace layerc::editor {
namespace {
Fl_Color token_color(const std::uint32_t value) {
  return fl_rgb_color(static_cast<uchar>((value >> 16U) & 0xFFU),
                      static_cast<uchar>((value >> 8U) & 0xFFU),
                      static_cast<uchar>(value & 0xFFU));
}
} // namespace

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
PlacedPanel placement(const layerc::core::Panel &panel, const PlacementFrame &frame,
                      const std::size_t count) {
  auto geometry = layerc::core::panel_geometry(panel, count);
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
  accessibility::set_accessible_role(*this, a11y::Role::canvas);
  accessibility::set_accessible_description(*this,
      "Drag a panel to change its anchor. The Panel fields edit the same values.");
  // The Preview role's default; Appearance may override it.
  color(token_color(current_editor_tokens().input));
}
PanelPreview::~PanelPreview() { Fl::remove_timeout(wake, this); }
void PanelPreview::wake(void *data) {
  static_cast<PanelPreview *>(data)->redraw();
}

SimulatorPanelPreview PanelPreview::present(const core::Panel &panel,
                                           bool *simulated) const {
  if (simulated) *simulated = false;
  if (app_.automation) {
    if (auto value = app_.automation->panel_preview(panel.id)) {
      if (simulated) *simulated = true;
      return *value;
    }
  }
  const auto &document = app_.session.document();
  const auto &profile = document.profile();
  const auto now = core::MonotonicClock::now();
  core::CooldownEngine engine(profile.cooldowns, profile.cooldown_groups);
  const auto states = engine.snapshot(now);
  std::optional<core::AutomationRuntimeSnapshot> automation;
  if (document.automation())
    automation = core::automation_default_snapshot(*document.automation());
  core::PanelPresentationOverrides overrides;
  overrides.edit_mode = true;
  overrides.presentation_epoch = epoch_;
  return {panel, core::present_panel(panel, profile.cooldowns, states,
      automation ? &*automation : nullptr, overrides, now)};
}

void PanelPreview::draw() {
  // The only preview wakeup is a real core flash edge. Deterministic replay
  // redraws on a simulator step, never against the wall clock.
  Fl::remove_timeout(wake, this);
  std::optional<core::TimePoint> next;
  fl_push_clip(x(), y(), w(), h());
  fl_rectf(x(), y(), w(), h(), color());
  const auto frame = frame_of(*this, app_);
  if (app_.placement_active()) {
    fl_color(token_color(current_editor_tokens().border));
    fl_rect(frame.x, frame.y, frame.width, frame.height);
    fl_font(FL_HELVETICA, static_cast<int>(FL_NORMAL_SIZE));
    const auto &target = *app_.placement();
    const auto caption = "Positioning on " +
        (target.title.empty() ? std::string("window") : target.title) +
        " (" + std::to_string(target.width) + "x" + std::to_string(target.height) + ")";
    fl_draw(caption.c_str(), frame.x + 4, frame.y + 14);
  }
  if (app_.session.has_document()) {
    const auto &profile = app_.session.document().profile();
    for (const auto &panel : profile.panels) {
      bool simulated = false;
      const auto view = present(panel, &simulated);
      if (!view.presentation.visible) continue;
      const auto placed = placement(view.panel, frame, view.presentation.items.size());
      platform::OverlayFrame overlay;
      overlay.panel = view.panel;
      for (const auto &item : view.presentation.items)
        overlay.bars.push_back(platform::overlay_bar_from_presentation(item));
      const auto display = platform::build_overlay_display_list(overlay);
      // This is the exact same typed paint model as X11, cairo and GNOME.
      for (const auto &op : display.ops) {
        const int left = placed.left + static_cast<int>(op.rect.x * placed.scale);
        const int top = placed.top + static_cast<int>(op.rect.y * placed.scale);
        const int width = std::max(1, static_cast<int>(op.rect.width * placed.scale));
        const int height = std::max(1, static_cast<int>(op.rect.height * placed.scale));
        fl_color(rgb(op.rgb));
        switch (op.kind) {
        case platform::OverlayOpKind::fill_rect:
          fl_rectf(left, top, width, height);
          break;
        case platform::OverlayOpKind::stroke_rect:
          fl_rect(left, top, width, height);
          break;
        case platform::OverlayOpKind::text:
          fl_font(FL_COURIER, std::max(8, static_cast<int>(op.font_px * placed.scale)));
          fl_draw(op.text.c_str(), left + 2, top + 1, std::max(width - 4, 1),
                  std::max(height - 2, 1), FL_ALIGN_INSIDE | FL_ALIGN_CENTER);
          break;
        }
      }
      if (!simulated && view.presentation.next_wakeup &&
          (!next || *view.presentation.next_wakeup < *next))
        next = view.presentation.next_wakeup;
      if (panel.id == app_.selected_panel()) {
        fl_color(token_color(current_editor_tokens().accent));
        fl_rect(placed.left, placed.top,
                static_cast<int>(placed.geometry.size.width * placed.scale),
                static_cast<int>(placed.geometry.size.height * placed.scale));
      }
    }
  }
  fl_pop_clip();
  if (next && visible_r()) {
    const auto seconds = std::chrono::duration<double>(*next - core::MonotonicClock::now()).count();
    Fl::add_timeout(std::max(seconds, 0.001), wake, this);
  }
}
int PanelPreview::handle(int event) {
  if (!app_.session.has_document()) return Fl_Widget::handle(event);
  if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
    if (!app_.apply_fields()) return 1;
    const auto frame = frame_of(*this, app_);
    for (const auto &panel : app_.session.document().profile().panels) {
      const auto view = present(panel);
      if (!view.presentation.visible) continue;
      const auto placed = placement(view.panel, frame, view.presentation.items.size());
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
        const auto view = present(panel);
        const auto placed = placement(view.panel, frame, view.presentation.items.size());
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
    if (error) app_.report(StatusKind::failure, *error);
    else {
      app_.refresh_forms();
      // Every drag of the bar says this again; the reader hears it once.
      app_.report(StatusKind::information, app_.placement_active() ? "Positioning on the window: the live draft moves LayerD's overlay."
                                          : "Unsaved preview placement.",
                  StatusVoice::silent);
      redraw();
    }
    return 1;
  } else if (event == FL_RELEASE && !dragging_.empty()) {
    dragging_.clear(); app_.session.end_text_edit(); return 1;
  }
  return Fl_Widget::handle(event);
}
} // namespace layerc::editor
