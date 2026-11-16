#include "accessibility_tree.hpp"
#include "editor_popup.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace layerc::editor::accessibility {

AccessibilityTree::~AccessibilityTree() {
  if (!root_) return;
  detach();
  release();
  clear_root();
}
bool AccessibilityTree::initialize(std::string &fault) {
  if (root_registered()) {
    fault = "another LayerE window owns the accessibility bridge";
    return false;
  }
  root_ = make({nullptr, Kind::application, 0}, nullptr);
  register_root(root_);
  return true;
}
void AccessibilityTree::reconcile(bool quiet) {
  TreePass pass;
  reconcile(root_, quiet);
}
void AccessibilityTree::sync() {
  TreePass pass;
  reconcile(root_, false);
  report_window();
}
void AccessibilityTree::collect(AtkObject *object, std::vector<AtkObject *> &objects) {
  objects.push_back(object);
  for (auto *child : node(object).children) collect(child, objects);
}
AtkObject *AccessibilityTree::find(AtkObject *object, const Fl_Widget *widget) {
  auto &n = node(object);
  if (n.kind == Kind::widget && n.widget() == widget) return object;
  for (auto *child : n.children)
    if (auto *found = find(child, widget)) return found;
  return nullptr;
}
void AccessibilityTree::announce(Fl_Widget *source, const std::string &text) {
  if (root_ == nullptr || text.empty()) return;
  auto *object = source != nullptr ? find(root_, source) : nullptr;
  if (object == nullptr) object = active_window_;
  if (object == nullptr) object = root_;
  // ATK's notification event is what the adaptor forwards as an AT-SPI
  // announcement; the text is spoken, the widget is only its origin.
  g_signal_emit_by_name(object, "notification", valid_utf8(text).c_str(),
                        ATK_LIVE_POLITE);
}
void AccessibilityTree::detach() {
  if (!root_ || !detached_.empty()) return;
  collect(root_, detached_);
  for (auto *object : detached_) {
    node(object).children.clear();
    node(object).parent = nullptr;
    atk_object_set_parent(object, nullptr);
  }
}
void AccessibilityTree::release() {
  for (auto it = detached_.rbegin(); it != detached_.rend(); ++it) g_object_unref(*it);
  detached_.clear();
  root_ = active_window_ = nullptr;
}

void AccessibilityTree::retire(AtkObject *object, bool quiet) {
  auto &n = node(object);
  for (auto *child : n.children) retire(child, quiet);
  n.children.clear();
  if (!quiet && ATK_IS_WINDOW(object)) g_signal_emit_by_name(object, "destroy");
  if (active_window_ == object) active_window_ = nullptr;
  n.retired = true;
  n.tracker.reset();
  n.parent = nullptr;
  atk_object_set_parent(object, nullptr);
  if (!quiet) atk_object_notify_state_change(object, ATK_STATE_DEFUNCT, TRUE);
  g_object_unref(object);
}

void AccessibilityTree::reconcile(AtkObject *object, bool quiet) {
  auto &n = node(object);
  if (!live(n)) return;
  auto previous = n.children;
  std::vector<AtkObject *> next;
  std::vector<bool> added;
  for (const auto &key : desired(object)) {
    // Children usually keep their position; search only when they moved.
    const auto index = static_cast<std::ptrdiff_t>(next.size());
    const auto found = index < static_cast<std::ptrdiff_t>(previous.size()) &&
                               previous[static_cast<std::size_t>(index)] &&
                               same(node(previous[static_cast<std::size_t>(index)]), key)
                           ? previous.begin() + index
                           : std::find_if(previous.begin(), previous.end(), [&](AtkObject *child) {
                               return child && same(node(child), key);
                             });
    if (found != previous.end()) {
      next.push_back(*found);
      *found = nullptr;
      added.push_back(false);
    } else {
      next.push_back(make(key, object));
      added.push_back(true);
    }
  }
  for (std::size_t i = previous.size(); i-- > 0;) {
    if (!previous[i]) continue;
    // Unlist first, so a listener never reaches a child being retired.
    n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(i));
    if (!quiet)
      g_signal_emit_by_name(object, "children-changed::remove", static_cast<gint>(i), previous[i]);
    retire(previous[i], quiet);
  }
  n.children = std::move(next);
  for (std::size_t i = 0; i < n.children.size(); ++i) {
    auto *child = n.children[i];
    if (added[i] && !quiet) {
      // Announce a new subtree once, already complete.
      reconcile(child, true);
      g_signal_emit_by_name(object, "children-changed::add", static_cast<gint>(i), child);
      if (ATK_IS_WINDOW(child)) g_signal_emit_by_name(child, "create");
    } else {
      reconcile(child, quiet);
    }
  }
  if (!quiet) report_changes(object);
}

void AccessibilityTree::report_changes(AtkObject *object) {
  auto &n = node(object);
  if (!live(n)) return;
  if (auto name = current_name(n); name != n.name) {
    n.name = std::move(name);
    g_object_notify(G_OBJECT(object), "accessible-name");
  }
  if (auto description = current_description(object);
      description != n.description) {
    n.description = std::move(description);
    g_object_notify(G_OBJECT(object), "accessible-description");
  }
  const auto state = [&](bool &last, bool next, AtkStateType type) {
    if (next == last) return false;
    last = next;
    atk_object_notify_state_change(object, type, next);
    return true;
  };
  if (bool next = enabled(n); next != n.enabled) {
    state(n.enabled, next, ATK_STATE_ENABLED);
    atk_object_notify_state_change(object, ATK_STATE_SENSITIVE, next);
  }
  state(n.showing, showing(n), ATK_STATE_SHOWING);
  state(n.checked, checked(n), ATK_STATE_CHECKED);
  if (state(n.selected, selected(n), ATK_STATE_SELECTED) && n.selected && n.parent &&
      ATK_IS_SELECTION(n.parent))
    g_signal_emit_by_name(n.parent, "selection-changed");
  if (ATK_IS_TEXT(object)) {
    const auto text = input_text(object);
    if (text != n.text) report_text(object, n, text);
    if (const auto next = caret(ATK_TEXT(object)); next != n.caret) {
      n.caret = next;
      if (Fl::focus() == n.widget()) g_signal_emit_by_name(object, "text-caret-moved", next);
    }
  }
  // Focus is reported last, after the object's new name and state.
  state(n.focused, focused(n), ATK_STATE_FOCUSED);
}

// Reports only the changed span, with the removed text itself: the
// adaptor cannot read removed text back from the updated field.
void AccessibilityTree::report_text(AtkObject *object, Node &n, const std::string &text) {
  const auto old = std::move(n.text);
  n.text = text;
  const auto continuation = [](const std::string &s, std::size_t at) {
    return at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80;
  };
  std::size_t prefix = 0;
  while (prefix < old.size() && prefix < text.size() && old[prefix] == text[prefix]) ++prefix;
  while (prefix && (continuation(old, prefix) || continuation(text, prefix))) --prefix;
  std::size_t suffix = 0;
  while (suffix < old.size() - prefix && suffix < text.size() - prefix &&
         old[old.size() - 1 - suffix] == text[text.size() - 1 - suffix]) ++suffix;
  while (suffix && continuation(old, old.size() - suffix)) --suffix;
  const auto removed = old.substr(prefix, old.size() - prefix - suffix);
  const auto inserted = text.substr(prefix, text.size() - prefix - suffix);
  const auto position = static_cast<gint>(g_utf8_strlen(old.data(), static_cast<gssize>(prefix)));
  if (!removed.empty())
    g_signal_emit_by_name(object, "text-remove", position, char_count(removed), removed.c_str());
  if (!inserted.empty())
    g_signal_emit_by_name(object, "text-insert", position, char_count(inserted), inserted.c_str());
}

void AccessibilityTree::report_window() {
  AtkObject *next{};
  Fl_Window *window = Fl::modal() ? Fl::modal() : (Fl::focus() ? Fl::focus()->top_window() : nullptr);
  if (auto *document = popup_document_window(window)) window = document;
  for (auto *child : node(root_).children)
    if (node(child).widget() == window) next = child;
  if (next == active_window_) return;
  if (active_window_) {
    atk_object_notify_state_change(active_window_, ATK_STATE_ACTIVE, FALSE);
    g_signal_emit_by_name(active_window_, "deactivate");
  }
  active_window_ = next;
  if (next) {
    atk_object_notify_state_change(next, ATK_STATE_ACTIVE, TRUE);
    g_signal_emit_by_name(next, "activate");
  }
}

} // namespace layerc::editor::accessibility
