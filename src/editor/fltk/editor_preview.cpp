#include "editor_app.hpp"
#include "editor_style.hpp"
#include "accessible_widgets.hpp"
#include "layerc/core/panel_geometry.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "automation_simulator.hpp"
#include "platform/overlay_preview.hpp"

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
  if constexpr (windows_editor_keys)
    accessibility::describe_with_tooltip(*this,
        "Drag the selected bar. Arrow keys nudge it 1 px (Shift: 10 px).");
  else
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
      // The local overlay's own paint model: the shared display list on
      // Linux, the GDI overlay's rounded shapes on Windows.
      const auto paint = overlay_preview_paint(view.panel, view.presentation.items);
      const Fl_Font face = paint.face == OverlayPreviewFace::monospace
          ? FL_COURIER : FL_HELVETICA_BOLD;
      for (const auto &op : paint.ops) {
        const int left = placed.left + static_cast<int>(op.x * placed.scale);
        const int top = placed.top + static_cast<int>(op.y * placed.scale);
        const int width = std::max(1, static_cast<int>(op.width * placed.scale));
        const int height = std::max(1, static_cast<int>(op.height * placed.scale));
        const int radius = op.radius > 0
            ? std::min({std::max(1, static_cast<int>(std::lround(op.radius * placed.scale))),
                        width / 2, height / 2})
            : 0;
        fl_color(rgb(op.rgb));
        switch (op.kind) {
        case OverlayPreviewOp::Kind::fill:
          if (radius > 0) fl_rounded_rectf(left, top, width, height, radius);
          else fl_rectf(left, top, width, height);
          break;
        case OverlayPreviewOp::Kind::stroke:
          if (radius > 0) fl_rounded_rect(left, top, width, height, radius);
          else fl_rect(left, top, width, height);
          break;
        case OverlayPreviewOp::Kind::text:
          fl_font(face, std::max(8, static_cast<int>(op.font_px * placed.scale)));
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
  if constexpr (windows_editor_keys)
    if (Fl::focus() == this && Fl::visible_focus())
      draw_editor_focus(x(), y(), w(), h());
  fl_pop_clip();
  if (next && visible_r()) {
    const auto seconds = std::chrono::duration<double>(*next - core::MonotonicClock::now()).count();
    Fl::add_timeout(std::max(seconds, 0.001), wake, this);
  }
}

void PanelPreview::move_anchor(const std::string_view panel_id,
                               const std::function<void(core::Panel &)> &move) {
  const auto error = app_.session.edit([&](auto &profile) {
    for (auto &panel : profile.panels)
      if (panel.id == panel_id) move(panel);
  }, true);
  if (error) app_.report(StatusKind::failure, *error);
  else {
    app_.refresh_forms();
    app_.report(StatusKind::information, app_.placement_active()
        ? "Positioning on the window: the live draft moves LayerD's overlay."
        : "Unsaved preview placement.", StatusVoice::silent);
    redraw();
  }
}

void PanelPreview::finish_move() {
  dragging_.clear();
  app_.session.end_text_edit();
}

bool PanelPreview::nudge(const int key) {
  if (key != FL_Left && key != FL_Right && key != FL_Up && key != FL_Down)
    return false;
  if (!app_.apply_fields()) return true;
  const auto selected = app_.panel_index();
  if (!selected) return true;
  const auto &panel = app_.session.document().profile().panels[*selected];
  const auto view = present(panel);
  if (!view.presentation.visible || view.presentation.items.empty()) return true;
  const auto frame = frame_of(*this, app_);
  const int width = app_.placement_active() ? app_.placement()->width : frame.width;
  const int height = app_.placement_active() ? app_.placement()->height : frame.height;
  const double step = Fl::event_shift() ? 10.0 : 1.0;
  const auto panel_id = panel.id;
  app_.session.end_text_edit();
  move_anchor(panel_id, [&](core::Panel &item) {
    if (key == FL_Left || key == FL_Right)
      item.anchor_x = std::clamp(item.anchor_x +
          (key == FL_Left ? -step : step) / std::max(width, 1), 0.0, 1.0);
    else
      item.anchor_y = std::clamp(item.anchor_y +
          (key == FL_Up ? -step : step) / std::max(height, 1), 0.0, 1.0);
  });
  finish_move();
  return true;
}

int PanelPreview::handle(int event) {
  if constexpr (windows_editor_keys) {
    if (event == FL_FOCUS || event == FL_UNFOCUS) {
      redraw();
      return event == FL_UNFOCUS || app_.session.has_document();
    }
    if (event == FL_KEYDOWN && Fl::focus() == this && app_.session.has_document() &&
        nudge(Fl::event_key()))
      return 1;
  }
  if (!app_.session.has_document()) return Fl_Widget::handle(event);
  if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
    if constexpr (windows_editor_keys) take_focus();
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
    move_anchor(dragging_, [&](core::Panel &panel) {
      const auto view = present(panel);
      const auto placed = placement(view.panel, frame, view.presentation.items.size());
      const auto width = static_cast<int>(placed.geometry.size.width * placed.scale);
      const auto height = static_cast<int>(placed.geometry.size.height * placed.scale);
      if (frame.scale > 0.0) { // positioning: the cursor is the overlay's centre
        const auto anchor = anchor_at(frame, Fl::event_x(), Fl::event_y());
        panel.anchor_x = anchor.x;
        panel.anchor_y = anchor.y;
        return;
      }
      panel.anchor_x = std::clamp(static_cast<double>(Fl::event_x() - frame.x - width / 2) /
                                    std::max(frame.width - width, 1), 0.0, 1.0);
      panel.anchor_y = std::clamp(static_cast<double>(Fl::event_y() - frame.y - height / 2) /
                                    std::max(frame.height - height, 1), 0.0, 1.0);
    });
    return 1;
  } else if (event == FL_RELEASE && !dragging_.empty()) {
    finish_move();
    return 1;
  }
  return Fl_Widget::handle(event);
}
} // namespace layerc::editor
