#include "panel_item_dialog.hpp"

#include <array>
#include "accessible_widgets.hpp"
#include "editor_pages.hpp"
#include "profile_edit.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>

#include <algorithm>
#include <cmath>
#include <utility>

#include "editor_style.hpp"

namespace layerc::editor {
namespace {
constexpr const char *item_help =
    "Select a source. Active-duration timers use the cooldown's active duration.\n"
    "Timer preset starts a one-shot automation timer on an accepted cooldown trigger.";

std::optional<double> number(const std::string_view text) {
  const auto parsed = core::json::parse(text, 1);
  const auto *value = std::get_if<core::json::Value>(&parsed);
  if (!value || !value->is_number()) return std::nullopt;
  return value->as_number();
}
std::string number_text(const double value) {
  std::string text;
  core::json::append_number(text, value);
  return text;
}
} // namespace

PanelItemDialog::PanelItemDialog(
    const core::GameProfile &profile,
    const std::optional<core::AutomationProfile> &automation,
    std::optional<core::OverlayItem> current, Apply apply)
    : Fl_Double_Window(640, 585, current ? "Edit overlay item — LayerE"
                                        : "Add overlay item — LayerE"),
      profile_(profile), automation_(automation), current_(std::move(current)),
      apply_(std::move(apply)) {
  begin();
  const auto input = [](const int y, const char *label) {
    auto *field = new EditorField<Fl_Input>(235, y, 380, kit::metric(kit::Metric::control_height), label);
    return field;
  };
  kind_ = new EditorChoice(235, 25, 380, kit::metric(kit::Metric::combo_height), "Item type");
  kind_->add("Cooldown|Automation timer|Active-duration timer|Counter|Indicator");
  if (!current_) kind_->add("Timer preset");
  kind_->value(current_ ? static_cast<int>(current_->content.index()) : 0);
  source_ = new EditorChoice(235, 78, 380, kit::metric(kit::Metric::combo_height), "Source");
  custom_label_ = new Fl_Check_Button(20, 130, 200, kit::metric(kit::Metric::control_height), "Custom label");
  label_ = input(166, "Item label");
  label_->tooltip("An explicitly empty custom label is preserved.");
  maximum_ = input(222, "Counter maximum (optional)");
  display_ = new EditorChoice(235, 278, 380, kit::metric(kit::Metric::combo_height), "Counter display");
  display_->add("Number|Pips"); display_->value(0);
  flash_ = input(334, "Flash Hz (0 = off)"); flash_->value("0");
  interval_ = input(390, "Preset seconds"); interval_->value("5");
  message_mark_ = new EditorStatusMark(20, 440, 18, 80);
  message_ = new Fl_Box(38, 440, 577, 80, item_help);
  message_->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  auto *cancel = new Fl_Button(375, 540, 110, kit::metric(kit::Metric::control_height), "Cancel");
  cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  auto *apply_button = new Fl_Button(505, 540, 110, kit::metric(kit::Metric::control_height), "Apply item");
  primary_button(*apply_button);
  apply_button->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemDialog *>(data)->accept();
  }, this);
  kind_->callback([](Fl_Widget *, void *data) {
    auto &dialog = *static_cast<PanelItemDialog *>(data);
    dialog.fill_sources();
    dialog.update_fields();
  }, this);
  custom_label_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemDialog *>(data)->update_fields();
  }, this);
  flash_->when(FL_WHEN_CHANGED);
  flash_->callback([](Fl_Widget *, void *data) {
    static_cast<PanelItemDialog *>(data)->update_fields();
  }, this);
  for (Fl_Widget *field :
       std::array<Fl_Widget *, 8>{kind_, source_, custom_label_, label_, maximum_,
                                  display_, flash_, interval_})
    accessibility::describe_with(*field, *message_);
  std::string source;
  if (current_) {
    custom_label_->value(current_->label.has_value());
    label_->value(current_->label.value_or("").c_str());
    if (const auto *item = std::get_if<core::CooldownItem>(&current_->content))
      source = item->cooldown_id;
    if (const auto *item = std::get_if<core::AutomationTimerItem>(&current_->content))
      source = item->timer_id;
    if (const auto *item = std::get_if<core::CooldownActiveTimerItem>(&current_->content))
      source = item->cooldown_id;
    if (const auto *item = std::get_if<core::CounterItem>(&current_->content)) {
      source = item->variable;
      if (item->max) maximum_->value(number_text(*item->max).c_str());
      display_->value(item->display == core::CounterDisplay::pips ? 1 : 0);
    }
    if (const auto *item = std::get_if<core::IndicatorItem>(&current_->content)) {
      source = item->variable;
      flash_->value(number_text(item->flash_hz).c_str());
    }
  }
  fill_sources(source);
  update_fields();
  end();
  set_modal();
}

void PanelItemDialog::fill_sources(const std::string &selected) {
  source_->clear();
  source_ids_.clear();
  const auto add = [&](const std::string &id, const std::string &name) {
    if (std::ranges::find(source_ids_, id) != source_ids_.end()) return;
    source_ids_.push_back(id);
    add_choice(*source_, name.empty() || name == id ? id : name + " — " + id);
  };
  const auto kind = kind_->value();
  if (kind == 0 || kind == 2 || kind == 5) {
    for (const auto &cooldown : profile_.cooldowns) add(cooldown.id, cooldown.name);
  } else if (automation_) {
    if (kind == 1) {
      for (const auto &definition : automation_->automations)
        for (const auto &rule : definition.rules)
          for (const auto &action : rule.actions)
            if (action.kind == core::AutomationActionKind::create_timer)
              add(action.target, action.target);
    } else {
      for (const auto &variable : automation_->variables) {
        const bool numeric = variable.type == core::AutomationValueType::integer ||
                             variable.type == core::AutomationValueType::number;
        const bool compatible = numeric ||
            (kind == 4 && variable.type == core::AutomationValueType::boolean);
        if (compatible) add(variable.id, variable.id);
      }
    }
  }
  auto chosen = selected;
  // Existing unavailable sources are visible and round-trip unchanged.
  if (!chosen.empty() &&
      std::ranges::find(source_ids_, chosen) == source_ids_.end())
    add(chosen, chosen + " (unavailable)");
  // A new indicator keeps the Boolean-first default when one is available.
  if (chosen.empty() && kind == 4 && automation_) {
    const auto flag = std::ranges::find_if(
        automation_->variables, [](const core::AutomationVariable &variable) {
          return variable.type == core::AutomationValueType::boolean;
        });
    if (flag != automation_->variables.end() &&
        std::ranges::find(source_ids_, flag->id) != source_ids_.end())
      chosen = flag->id;
  }
  const auto found = std::ranges::find(source_ids_, chosen);
  source_->value(source_ids_.empty() ? -1 : found == source_ids_.end()
      ? 0 : static_cast<int>(found - source_ids_.begin()));
}

void PanelItemDialog::update_fields() {
  const auto kind = kind_->value();
  if (kind == 3) { maximum_->activate(); display_->activate(); }
  else { maximum_->deactivate(); display_->deactivate(); }
  if (kind == 4) flash_->activate(); else flash_->deactivate();
  if (kind == 5) interval_->activate(); else interval_->deactivate();
  if (custom_label_->value()) label_->activate(); else label_->deactivate();
  const auto rate = number(flash_->value());
  if (kind == 4 && rate && *rate > core::max_shown_indicator_flash_hz) {
    message_->copy_label("Flashes at 3 Hz; faster flashing is capped.");
    message_mark_->kind(StatusKind::warning);
  } else {
    message_->copy_label(item_help);
    message_mark_->kind(StatusKind::none);
  }
  redraw();
}

void PanelItemDialog::error(const std::string &message) {
  message_->copy_label(message.c_str());
  message_mark_->kind(StatusKind::failure);
  message_->redraw();
}

void PanelItemDialog::accept() {
  const auto source = source_->value();
  if (source < 0 || static_cast<std::size_t>(source) >= source_ids_.size()) {
    error("No compatible source. Configure a cooldown, timer action or variable first.");
    return;
  }
  const auto &id = source_ids_[static_cast<std::size_t>(source)];
  PanelItemDraft draft;
  if (current_) draft.item = *current_;
  draft.item.label = custom_label_->value()
      ? std::optional<std::string>(label_->value()) : std::nullopt;
  switch (kind_->value()) {
  case 0: draft.item.content = core::CooldownItem{id}; break;
  case 1: draft.item.content = core::AutomationTimerItem{id}; break;
  case 2: draft.item.content = core::CooldownActiveTimerItem{id}; break;
  case 3: {
    core::CounterItem counter;
    counter.variable = id;
    if (*maximum_->value()) {
      counter.max = number(maximum_->value());
      if (!counter.max || *counter.max <= 0.0) {
        error("Counter maximum must be a finite positive number, or empty.");
        return;
      }
    }
    counter.display = display_->value() == 1
        ? core::CounterDisplay::pips : core::CounterDisplay::number;
    if (counter.display == core::CounterDisplay::pips &&
        (!counter.max || *counter.max > core::max_counter_pips ||
         std::floor(*counter.max) != *counter.max)) {
      error("Pip display requires an integer maximum from 1 to " +
            std::to_string(core::max_counter_pips) + ".");
      return;
    }
    draft.item.content = std::move(counter);
    break;
  }
  case 4: {
    const auto rate = number(flash_->value());
    const auto *existing =
        current_ ? std::get_if<core::IndicatorItem>(&current_->content) : nullptr;
    const bool preserved_high_flash =
        existing != nullptr &&
        existing->flash_hz > core::max_shown_indicator_flash_hz &&
        existing->flash_hz <= core::max_indicator_flash_hz &&
        rate && *rate == existing->flash_hz;
    if (!rate ||
        (*rate != 0.0 && !preserved_high_flash &&
         (*rate < core::min_indicator_flash_hz ||
          *rate > core::max_shown_indicator_flash_hz))) {
      error("Flash Hz must be 0 (off), or between 0.5 and 3.");
      return;
    }
    draft.item.content = core::IndicatorItem{id, *rate};
    break;
  }
  case 5: {
    const auto interval = parse_duration_seconds(interval_->value());
    if (!interval || *interval < core::min_automation_timer_interval) {
      error("Preset seconds must be at least 0.05 and within the cooldown limit.");
      return;
    }
    draft.timer_preset = true;
    draft.interval = *interval;
    draft.item.content = core::CooldownItem{id};
    break;
  }
  default: error("Unknown item types are preserved but cannot be edited."); return;
  }
  if (const auto failure = apply_(draft)) { error(*failure); return; }
  hide();
}

} // namespace layerc::editor
