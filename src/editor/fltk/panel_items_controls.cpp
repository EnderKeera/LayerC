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

#include <algorithm>
#include <string>
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
} // namespace

void PanelItemsControls::build() {
  group_ = new Fl_Group(495, 295, 540, 140);
  group_->begin();
  const int reason_height = 2 * editor_input_height;
  const int reason_y = group_->y() + group_->h() - reason_height;
  items_ = new Fl_Hold_Browser(500, 310, 355, reason_y - 310, "Panel items (ordered)");
  reason_ = make_disabled_reason(items_->x(), reason_y, items_->w(), reason_height);
  items_->align(FL_ALIGN_TOP_LEFT);
  items_->format_char(0);
  items_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemsControls *>(data)->select();
  }, this);
  const auto button = [&](const int x, const int y, const char *label) {
    auto *value = new Fl_Button(x, y, 80, editor_button_height, label);
    value->labelsize(FL_NORMAL_SIZE);
    return value;
  };
  add_ = button(865, 310, "Add item");
  edit_ = button(950, 310, "Edit item");
  remove_ = button(865, 350, "Remove item");
  up_ = button(950, 350, "Move up");
  down_ = button(950, 390, "Move down");
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
  std::string line;
  const auto action = [&](Fl_Button *button, const bool enabled, const std::string &reason) {
    active(button, enabled);
    accessibility::describe_with_tooltip(*button, reason);
    if (reason.empty()) return;
    if (!line.empty()) line += ' ';
    line += reason;
  };
  action(add_, panel && !full,
         full ? "A panel holds at most " + std::to_string(core::max_overlay_items_per_panel) +
                    " items." : "");
  action(edit_, has_row && !opaque,
         opaque ? "This item kind is kept as it is and can't be edited here." : "");
  action(remove_, has_row && !last, last ? "A panel needs at least one item." : "");
  active(up_, has_row && *row > 0);
  active(down_, has_row && *row + 1 < count);
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
