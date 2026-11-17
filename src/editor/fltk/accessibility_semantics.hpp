#pragma once

#include "a11y/fltk_semantics.hpp"

#include <FL/Fl.H>
#include <atk/atk.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Fl_Menu_;
struct Fl_Menu_Item;

namespace layerc::editor::accessibility {

using Kind = a11y::TargetKind;
using Key = a11y::Target;

struct Node {
  std::unique_ptr<Fl_Widget_Tracker> tracker;
  AtkObject *parent{};
  std::vector<AtkObject *> children; // Each holds this node's reference.
  Kind kind{Kind::application};
  int index{}; // Browser row (1-based) or menu item index.
  bool retired{};
  // Last state reported to the adaptor; changes become ATK events.
  std::string name, text, keybinding;
  std::string name_reply; // get_name's buffer; never the reported name.
  std::string description;
  std::string description_reply; // get_description's buffer, like name_reply.
  int caret{-1};
  bool focused{}, selected{}, checked{}, showing{}, enabled{};
  Fl_Widget *widget() const { return tracker ? tracker->widget() : nullptr; }
};

Node &node(void *object);
bool live(const Node &node);
bool same(const Node &node, const Key &key);
bool focused(const Node &node);
bool selected(const Node &node);
bool showing(const Node &node);
bool enabled(const Node &node);
bool checked(const Node &node);
std::string current_name(const Node &node);
// Text as ATK and its adaptor accept it: invalid UTF-8 is repaired.
std::string valid_utf8(const std::string &text);
std::string current_description(AtkObject *object);
std::string input_text(void *object);
gint caret(AtkText *object);
gint char_count(const std::string &text);
AtkObject *make(const Key &key, AtkObject *parent);
std::vector<Key> desired(AtkObject *object);

bool root_registered() noexcept;
void register_root(AtkObject *root);
void clear_root() noexcept;
void cancel_deferred();

using TreePass = a11y::TreePass;
using a11y::menu_size;

} // namespace layerc::editor::accessibility
