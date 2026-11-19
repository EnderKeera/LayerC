#include "fltk_actions.hpp"

#include "fltk_hooks.hpp"
#include "fltk_semantics.hpp"

#include "layerc/core/utf8.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Valuator.H>
#include <FL/Fl_Widget.H>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace layerc::editor::a11y {
namespace {

// Application callbacks may open modal dialogs, so, like GTK, they run from
// FLTK's own loop after the protocol call has returned; a deleted widget
// cancels its step.
std::vector<std::function<void()>> deferred;

ChangedHandler changed_handler = nullptr;
void *changed_handler_data = nullptr;

// A bridge that published a tree hears about every action that took effect;
// the hook only marks that tree stale, so it never walks here.
void notify_changed() {
  if (changed_handler != nullptr) changed_handler(changed_handler_data);
}

void run_deferred(void *) {
  auto steps = std::move(deferred);
  deferred.clear();
  for (auto &step : steps) step();
}

void defer(Fl_Widget *widget, std::function<void(Fl_Widget &)> step) {
  auto tracker = std::make_shared<Fl_Widget_Tracker>(widget);
  if (deferred.empty()) Fl::add_timeout(0.0, run_deferred);
  deferred.push_back([tracker, step = std::move(step)] {
    if (auto *live_widget = tracker->widget();
        live_widget && live_widget->active_r() && modal_allows(*live_widget)) {
      step(*live_widget);
      notify_changed();
    }
  });
}

void queue_callback(Fl_Widget &widget) {
  defer(&widget, [](Fl_Widget &target) { target.do_callback(); });
}

Fl_Input_ *input_of(const Target &target) {
  if (target.kind != TargetKind::widget || target.widget == nullptr) return nullptr;
  return dynamic_cast<Fl_Input_ *>(target.widget);
}

Fl_Valuator *valuator_of(const Target &target) {
  if (target.kind != TargetKind::widget || target.widget == nullptr) return nullptr;
  return dynamic_cast<Fl_Valuator *>(target.widget);
}

std::string field_text(const Fl_Input_ &field) {
  return field.value() != nullptr
             ? std::string(field.value(), static_cast<std::size_t>(field.size()))
             : std::string{};
}

// A byte offset the model can publish: inside the text and on a code point
// boundary. The adaptors translate their own units before they get here.
bool addressable(const std::string &text, std::size_t offset) {
  return offset <= text.size() &&
         (offset == text.size() || (static_cast<unsigned char>(text[offset]) & 0xc0U) != 0x80U);
}

// The field a text action may read offsets from, or nullptr when its value is
// not what the model exported.
Fl_Input_ *addressable_field(const Target &target, std::string &text) {
  auto *field = input_of(target);
  if (field == nullptr) return nullptr;
  text = field_text(*field);
  return core::is_valid_utf8(text) ? field : nullptr;
}

} // namespace

void set_changed_handler(ChangedHandler handler, void *data) {
  changed_handler = handler;
  changed_handler_data = data;
}

void cancel_deferred() {
  Fl::remove_timeout(run_deferred);
  deferred.clear();
}

// Actions invoke the application's own callbacks; no input is synthesized.
bool activate(const Target &target) {
  auto *widget = target.widget;
  if (widget == nullptr || !enabled(target) ||
      (target.kind == TargetKind::menu_item ? !widget->visible_r() : !showing(target)))
    return false;
  if (target.kind == TargetKind::menu_item && menu_item_of(target) == nullptr) return false;
  const auto kind = target.kind;
  const int index = target.index;
  defer(widget, [kind, index](Fl_Widget &chosen) {
    if (kind == TargetKind::row) {
      auto &list = static_cast<Fl_Browser &>(chosen);
      if (index > list.size()) return;
      list.value(index);
      list.do_callback();
      list.redraw();
    } else if (kind == TargetKind::menu_item) {
      auto &menu = static_cast<Fl_Menu_ &>(chosen);
      if (index >= menu_size(menu) - 1 || !menu.menu()[index].activevisible()) return;
      if (popup_pick(menu, index)) return;
      // FLTK's own popup only when no frontend popup can open this title.
      if (menu.menu()[index].submenu() && popup_open(menu, index)) return;
      (void)menu.picked(&menu.menu()[index]);
    } else if (auto *tabs = dynamic_cast<Fl_Tabs *>(chosen.parent())) {
      if (tabs->value(&chosen)) tabs->do_callback();
      tabs->redraw();
    } else if (auto *button = dynamic_cast<Fl_Button *>(&chosen);
               button != nullptr && button->type() == FL_RADIO_BUTTON) {
      button->setonly();
      button->do_callback();
    } else if (button != nullptr && button->type() == FL_TOGGLE_BUTTON) {
      button->value(!button->value());
      button->do_callback();
    } else {
      chosen.do_callback();
    }
  });
  return true;
}

bool grab_focus(const Target &target) {
  auto *widget = target.widget;
  if (widget == nullptr || !showing(target) || !enabled(target) ||
      target.kind == TargetKind::menu_item)
    return false;
  // take_focus() runs unfocus/focus handlers that may open a modal; like every
  // action, it runs from FLTK's loop rather than inside this call.
  const bool accepted = target.kind == TargetKind::row || widget->visible_focus();
  if (!accepted) return false;
  defer(widget, [](Fl_Widget &chosen) { (void)chosen.take_focus(); });
  if (target.kind == TargetKind::row) {
    defer(widget, [row = target.index](Fl_Widget &chosen) {
      auto &list = static_cast<Fl_Browser &>(chosen);
      if (row > list.size()) return;
      list.value(row);
      list.do_callback();
    });
  }
  widget->redraw();
  return true;
}

bool set_caret(const Target &target, std::size_t offset) {
  std::string text;
  auto *field = addressable_field(target, text);
  if (field == nullptr || !addressable(text, offset)) return false;
  field->insert_position(static_cast<int>(offset));
  field->redraw();
  notify_changed();
  return true;
}

bool select_text(const Target &target, std::size_t anchor, std::size_t caret) {
  std::string text;
  auto *field = addressable_field(target, text);
  if (field == nullptr || !addressable(text, anchor) || !addressable(text, caret)) return false;
  field->insert_position(static_cast<int>(caret), static_cast<int>(anchor));
  field->redraw();
  notify_changed();
  return true;
}

// Edits go through the field's own callback, exactly like typed text.
bool set_text(const Target &target, std::string_view text) {
  auto *field = input_of(target);
  if (field == nullptr || !editable(target) || !core::is_valid_utf8(text)) return false;
  field->value(text.data(), static_cast<int>(text.size()));
  field->redraw();
  queue_callback(*field);
  notify_changed();
  return true;
}

bool insert_text(const Target &target, std::size_t offset, std::string_view text) {
  auto *field = input_of(target);
  std::string value;
  if (field == nullptr || !editable(target) || !core::is_valid_utf8(text)) return false;
  value = field_text(*field);
  if (!addressable(value, offset)) return false;
  value.insert(offset, text.data(), text.size());
  // value() never runs callbacks; replace() would run FL_WHEN_CHANGED ones
  // inside this protocol call, before the deferred callback repeats them.
  field->value(value.data(), static_cast<int>(value.size()));
  field->insert_position(static_cast<int>(offset + text.size()));
  field->redraw();
  queue_callback(*field);
  notify_changed();
  return true;
}

bool delete_text(const Target &target, std::size_t begin, std::size_t end) {
  auto *field = input_of(target);
  std::string value;
  if (field == nullptr || !editable(target) || !addressable_field(target, value)) return false;
  if (begin > end || !addressable(value, begin) || !addressable(value, end)) return false;
  value.erase(begin, end - begin);
  field->value(value.data(), static_cast<int>(value.size()));
  field->insert_position(static_cast<int>(begin));
  field->redraw();
  queue_callback(*field);
  notify_changed();
  return true;
}

bool set_number(const Target &target, double value) {
  auto *valuator = valuator_of(target);
  if (valuator == nullptr || !enabled(target) || !showing(target)) return false;
  const auto low = std::min(valuator->minimum(), valuator->maximum());
  const auto high = std::max(valuator->minimum(), valuator->maximum());
  if (!std::isfinite(value) || value < low || value > high) return false;
  if (valuator->value(value)) {
    notify_changed();
    queue_callback(*valuator);
  }
  valuator->redraw();
  return true;
}

bool expand(const Target &target) {
  auto *menu = expansion_menu(target);
  if (menu == nullptr || !expansion_published(target) || expanded_now(target)) return false;
  // A popup runs its own modal loop, so it opens from FLTK's loop and only when
  // a frontend owns popups; otherwise the caller sees the failure.
  if (!popup_supported() || !enabled(target) || !showing(target)) return false;
  const int title = target.kind == TargetKind::menu_item ? target.index : -1;
  defer(menu,
        [title](Fl_Widget &owner) { (void)popup_open(static_cast<Fl_Menu_ &>(owner), title); });
  return true;
}

bool collapse(const Target &target) {
  auto *menu = expansion_menu(target);
  if (menu == nullptr || !expanded_now(target) || !popup_dismiss(*menu)) return false;
  notify_changed();
  return true;
}

bool scroll_to(const Target &target, double horizontal, double vertical) {
  if ((horizontal < 0 && vertical < 0) || !set_scroll(target, horizontal, vertical)) return false;
  notify_changed();
  return true;
}

} // namespace layerc::editor::a11y
