#include "panel_items_controls.hpp"
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
  items_ = new Fl_Hold_Browser(500, 310, 355, 112, "Panel items (ordered)");
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
  const auto panel = app_.panel_index();
  const auto row = selected();
  bool supported = false;
  if (panel && row) {
    const auto &value = app_.session.document().profile().panels[*panel];
    supported = *row < row_count(value) &&
                core::overlay_item_kind(row_item(value, *row)).has_value();
  }
  const auto active = [](Fl_Widget *widget, const bool enabled) {
    if (enabled) widget->activate(); else widget->deactivate();
  };
  active(add_, panel.has_value());
  active(edit_, supported);
  active(remove_, row.has_value());
  active(up_, row && *row > 0);
  active(down_, row && *row + 1 < static_cast<std::size_t>(items_->size()));
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
          app_.report("Overlay item applied. Save publishes the profile and modules together.");
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
  app_.report(error.value_or("Overlay item removed."));
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
  app_.report(error.value_or("Overlay item reordered."));
}

bool PanelItemsControls::owns(const Fl_Widget *widget) const {
  return widget && (widget == group_ || widget == items_ || widget == add_ ||
      widget == edit_ || widget == remove_ || widget == up_ || widget == down_);
}

} // namespace layerc::editor
