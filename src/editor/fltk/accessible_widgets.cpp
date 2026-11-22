#include "accessible_widgets.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace layerc::editor::accessibility {
namespace {

struct Entry {
  explicit Entry(Fl_Widget &widget) : control(&widget) {}
  Fl_Widget_Tracker control;
  // Set by describe_with; the hint is read on every request, never copied.
  std::unique_ptr<Fl_Widget_Tracker> hint;
  std::string name;        // empty: the widget names itself
  std::string description; // used while no hint is registered
  std::optional<a11y::Role> role; // set for controls FLTK cannot classify
};

// One list for the whole process: the editor has one widget tree and one
// FLTK thread. Entries are owned by pointer so a tracker never moves.
std::vector<std::unique_ptr<Entry>> &entries() {
  static std::vector<std::unique_ptr<Entry>> registry;
  return registry;
}

// Every lookup prunes first: a dialog's widgets are gone by the time the next
// one registers, and the allocator reuses their addresses.
Entry *lookup(const Fl_Widget *control) {
  auto &registry = entries();
  std::erase_if(registry, [](const std::unique_ptr<Entry> &entry) {
    return entry->control.widget() == nullptr;
  });
  for (auto &entry : registry)
    if (entry->control.widget() == control) return entry.get();
  return nullptr;
}

Entry &entry_for(Fl_Widget &control) {
  if (auto *found = lookup(&control)) return *found;
  entries().push_back(std::make_unique<Entry>(control));
  return *entries().back();
}

void drop_if_empty(const Entry &entry) {
  if (!entry.name.empty() || !entry.description.empty() || entry.hint || entry.role)
    return;
  std::erase_if(entries(), [&](const std::unique_ptr<Entry> &candidate) {
    return candidate.get() == &entry;
  });
}

// A hint reads its own words: a field its value, anything else its label.
std::string hint_text(const Fl_Widget &hint) {
  if (const auto *input = dynamic_cast<const Fl_Input_ *>(&hint))
    return input->value() != nullptr ? input->value() : std::string();
  return hint.label() != nullptr ? hint.label() : std::string();
}

} // namespace

void set_accessible_name(Fl_Widget &control, std::string text) {
  auto &entry = entry_for(control);
  entry.name = std::move(text);
  drop_if_empty(entry);
}

void set_accessible_description(Fl_Widget &control, std::string text) {
  auto &entry = entry_for(control);
  entry.hint.reset();
  entry.description = std::move(text);
  drop_if_empty(entry);
}

void describe_with_tooltip(Fl_Widget &control, std::string text) {
  control.copy_tooltip(text.empty() ? nullptr : text.c_str());
  set_accessible_description(control, std::move(text));
}

void describe_with(Fl_Widget &control, Fl_Widget &hint) {
  auto &entry = entry_for(control);
  entry.description.clear();
  entry.hint = std::make_unique<Fl_Widget_Tracker>(&hint);
}

void set_accessible_role(Fl_Widget &control, a11y::Role role) {
  entry_for(control).role = role;
}

std::optional<a11y::Role> accessible_role(const Fl_Widget &control) {
  const auto *entry = lookup(&control);
  return entry == nullptr ? std::nullopt : entry->role;
}

std::optional<std::string> accessible_name(const Fl_Widget &control) {
  const auto *entry = lookup(&control);
  if (entry == nullptr || entry->name.empty()) return std::nullopt;
  return entry->name;
}

std::string accessible_description(const Fl_Widget &control) {
  const auto *entry = lookup(&control);
  if (entry == nullptr) return {};
  if (!entry->hint) return entry->description;
  // The line that explained the control is gone: say nothing rather than
  // leave a description nobody can see.
  const auto *hint = entry->hint->widget();
  return hint != nullptr ? hint_text(*hint) : std::string();
}

std::string color_button_name(const std::string_view label,
                              const std::string_view hex) {
  std::string name;
  name.reserve(label.size() + hex.size() + 8);
  for (std::size_t index = 0; index < label.size(); ++index) {
    if (label[index] != '&') {
      name += label[index];
      continue;
    }
    // "&&" is a literal ampersand; a single one marks the access key.
    if (index + 1 < label.size() && label[index + 1] == '&') {
      name += '&';
      ++index;
    }
  }
  // An ellipsis opens a chooser; the name says the words, not the dots.
  for (const std::string_view ellipsis : {std::string_view("..."),
                                          std::string_view("\xE2\x80\xA6")})
    if (name.ends_with(ellipsis)) {
      name.erase(name.size() - ellipsis.size());
      break;
    }
  while (!name.empty() && name.back() == ' ') name.pop_back();
  name += ", color ";
  name += hex;
  return name;
}

} // namespace layerc::editor::accessibility
