#include "panel_items_controls.hpp"
#include "accessible_widgets.hpp"
#include "editor_pages.hpp"
#include "panel_item_dialog.hpp"
#include "panel_items.hpp"
#include "editor_app.hpp"
#include "layerc/core/panel_presentation.hpp"
#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>


namespace layerc::editor {
namespace {
core::OverlayItem row_item(const core::Panel &panel, const std::size_t row) {
  if (panel.items) return (*panel.items)[row];
  return {.id = core::legacy_item_id(row), .label = std::nullopt,
          .content = core::CooldownItem{panel.cooldown_ids[row]}, .extra_fields = {}};
}
std::size_t row_count(const core::Panel &panel) {
  return panel.items ? panel.items->size() : panel.cooldown_ids.size();
}

constexpr std::string_view opaque_reason =
    "This item kind is kept as it is and can't be edited here.";
constexpr std::string_view last_reason = "A panel needs at least one item.";

// The line under the list: each disabled action's reason, in action order.
std::string reason_line(const bool full, const bool opaque, const bool last) {
  std::string line;
  const auto add = [&](const bool shown, const std::string_view reason) {
    if (!shown) return;
    if (!line.empty()) line += ' ';
    line += reason;
  };
  add(full, "A panel holds at most " + std::to_string(core::max_overlay_items_per_panel) +
                " items.");
  add(opaque, opaque_reason);
  add(last, last_reason);
  return line;
}

// The height of `text` wrapped as `widget` draws an inside label
// (Fl_Widget::draw_label insets a left-aligned label by 3 px a side).
int wrapped_label_height(const Fl_Widget &widget, const std::string &text) {
  int width = widget.w() - Fl::box_dw(widget.box());
  if (width > 11 && (widget.align() & (FL_ALIGN_LEFT | FL_ALIGN_RIGHT)) != 0) width -= 6;
  int height = 0;
  fl_font(widget.labelfont(), widget.labelsize());
  fl_measure(text.c_str(), width, height, 0);
  return height;
}
} // namespace

void PanelItemsControls::build(const int top, const int bottom) {
  group_ = new Fl_Group(495, top, 540, bottom - top);
  group_->begin();
  // Created in focus order, then laid out from their measured text.
  items_ = new Fl_Hold_Browser(500, top, 355, 0, "Panel items (ordered)");
  items_->align(FL_ALIGN_TOP_LEFT);
  items_->format_char(0);
  reason_ = make_disabled_reason(500, top, 355, 0);
  items_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->select();
  }, this);
  const auto button = [&](const char *label) {
    auto *value = new Fl_Button(500, top, 80, editor_button_height, label);
    value->labelsize(FL_NORMAL_SIZE);
    return value;
  };
  add_ = button("Add item");
  edit_ = button("Edit item");
  remove_ = button("Remove item");
  up_ = button("Move up");
  down_ = button("Move down");
  add_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->edit(true);
  }, this);
  edit_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->edit(false);
  }, this);
  remove_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->remove();
  }, this);
  up_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->move(-1);
  }, this);
  down_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->move(1);
  }, this);
  group_->end();

  // FLTK draws a top label at the bottom of the space between the group's top
  // and the list, so that space is one group gap plus the label's own height
  // at the current text size (DesignSystem.md, Spacing).
  int label_width = 0, line_height = 0;
  items_->measure_label(label_width, line_height);
  const int band = top + editor_group_gap;
  // Two action columns at the group's right edge, each as wide as the widest
  // caption inside its box (80 px as authored, never narrower).
  int column = 80;
  for (const auto *action : {add_, edit_, remove_, up_, down_}) {
    int width = 0, height = 0;
    action->measure_label(width, height);
    column = std::max(column, width + Fl::box_dw(action->box()) + 6);
  }
  const int second = group_->x() + group_->w() - 5 - column;
  const int first = second - 5 - column;
  const int pitch = editor_button_height + editor_group_gap;
  const auto place = [&](Fl_Button *action, const int x, const int y) {
    action->resize(x, y, column, editor_button_height);
  };
  place(add_, first, band);
  place(edit_, second, band);
  place(remove_, first, band + pitch);
  place(up_, second, band + pitch);
  place(down_, second, band + 2 * pitch);
  // The list and its reason line fill the rest; the line keeps room for the
  // longest reason it can show, wrapped. A full panel has more than one item,
  // so the "full" and "last item" reasons never show together.
  const int list_width = first - editor_group_gap - items_->x();
  reason_->resize(items_->x(), bottom, list_width, 0);
  int reason_height = line_height;
  for (const auto &line : {reason_line(true, true, false), reason_line(false, true, true)})
    reason_height = std::max(reason_height, wrapped_label_height(*reason_, line));
  const int reason_y = bottom - reason_height;
  const int list_y = band + line_height;
  reason_->resize(items_->x(), reason_y, list_width, reason_height);
  items_->resize(items_->x(), list_y, list_width, reason_y - list_y);
}

void PanelItemsControls::show(const core::GameProfile &profile,
                              const std::optional<std::size_t> panel) {
  const int selection = items_->value();
  items_->clear();
  if (panel) {
    const auto &value = profile.panels[*panel];
    for (std::size_t row = 0; row < row_count(value); ++row) {
      const auto item = row_item(value, row);
      items_->add((item_kind_text(item) + " — " + item_display_name(profile, item)).c_str());
    }
    if (items_->size()) items_->value(std::clamp(selection, 1, items_->size()));
  }
  select();
}

std::optional<std::size_t> PanelItemsControls::selected() const {
  const auto row = items_->value();
  if (row < 1 || row > items_->size()) return std::nullopt;
  return static_cast<std::size_t>(row - 1);
}

void PanelItemsControls::select() {
  const auto index = app_.panel_index();
  const auto *panel = index ? &app_.session.document().profile().panels[*index] : nullptr;
  const auto row = selected();
  const auto count = panel ? row_count(*panel) : 0;
  const bool has_row = panel && row && *row < count;
  const bool opaque = has_row && !core::overlay_item_kind(row_item(*panel, *row));
  const bool last = has_row && count == 1;
  const bool full = count >= core::max_overlay_items_per_panel;
  const auto active = [](Fl_Widget *widget, const bool enabled) {
    if (enabled) widget->activate(); else widget->deactivate();
  };
  const auto action = [&](Fl_Button *button, const bool enabled, const std::string &reason) {
    active(button, enabled);
    accessibility::describe_with_tooltip(*button, reason);
  };
  action(add_, panel && !full, reason_line(full, false, false));
  action(edit_, has_row && !opaque, reason_line(false, opaque, false));
  action(remove_, has_row && !last, reason_line(false, false, last));
  active(up_, has_row && *row > 0);
  active(down_, has_row && *row + 1 < count);
  const auto line = reason_line(full, opaque, last);
  reason_->copy_label(line.c_str());
  if (line.empty()) reason_->hide(); else reason_->show();
}

void PanelItemsControls::edit(const bool adding) {
  if (!app_.apply_fields()) return;
  const auto panel = app_.panel_index();
  const auto row = selected();
  if (!panel || (!adding && !row)) return;
  std::optional<core::OverlayItem> current;
  if (!adding) {
    current = row_item(app_.session.document().profile().panels[*panel], *row);
    if (!core::overlay_item_kind(*current)) return;
  }
  PanelItemDialog dialog(
      app_.session.document().profile(), app_.session.document().automation(),
      std::move(current), [&](const PanelItemDraft &draft) {
        const auto error = app_.session.edit_items(
            [&](auto &profile, auto &automation) -> std::optional<std::string> {
              if (draft.timer_preset) {
                const auto &source = std::get<core::CooldownItem>(draft.item.content);
                auto result = add_automation_timer_preset(profile, automation, *panel,
                    source.cooldown_id, draft.interval, draft.item.label);
                if (const auto *failure = std::get_if<ItemEditError>(&result))
                  return failure->message;
                return std::nullopt;
              }
              const auto failure = adding
                  ? add_panel_item(profile, automation, *panel, draft.item)
                  : update_panel_item(profile, automation, *panel, *row, draft.item);
              return failure ? std::optional<std::string>(failure->message) : std::nullopt;
            });
        if (!error) {
          app_.refresh();
          if (adding) items_->value(items_->size());
          select();
          app_.report(StatusKind::success, "Overlay item applied. Save publishes the profile and modules together.");
        }
        return error;
      });
  dialog.show();
  while (dialog.shown()) Fl::wait();
}

void PanelItemsControls::remove() {
  if (!app_.apply_fields()) return;
  const auto panel = app_.panel_index(), row = selected();
  if (!panel || !row) return;
  const auto error = app_.session.edit_items(
      [&](auto &profile, auto &automation) -> std::optional<std::string> {
        const auto failure = remove_panel_item(profile, automation, *panel, *row);
        return failure ? std::optional<std::string>(failure->message) : std::nullopt;
      });
  app_.refresh();
  app_.report(error ? StatusKind::failure : StatusKind::success,
              error.value_or("Overlay item removed."));
}

void PanelItemsControls::move(const int direction) {
  if (!app_.apply_fields()) return;
  const auto panel = app_.panel_index(), row = selected();
  if (!panel || !row) return;
  if ((direction < 0 && *row == 0) ||
      (direction > 0 && *row + 1 >= static_cast<std::size_t>(items_->size()))) return;
  const auto target = direction < 0 ? *row - 1 : *row + 1;
  const auto error = app_.session.edit_items(
      [&](auto &profile, auto &automation) -> std::optional<std::string> {
        const auto failure = move_panel_item(profile, automation, *panel, *row, target);
        return failure ? std::optional<std::string>(failure->message) : std::nullopt;
      });
  app_.refresh();
  if (!error) items_->value(static_cast<int>(target + 1));
  select();
  app_.report(error ? StatusKind::failure : StatusKind::success,
              error.value_or("Overlay item reordered."));
}

bool PanelItemsControls::owns(const Fl_Widget *widget) const {
  return widget && (widget == group_ || widget == items_ || widget == add_ ||
      widget == edit_ || widget == remove_ || widget == up_ || widget == down_);
}

} // namespace layerc::editor
