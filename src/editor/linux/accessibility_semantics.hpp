#pragma once

#include <FL/Fl.H>
#include <atk/atk.h>

#include <memory>
#include <string>
#include <vector>

class Fl_Menu_;
struct Fl_Menu_Item;

namespace layerc::editor::accessibility {

enum class Kind { application, widget, row, menu_item };
struct Key {
  Fl_Widget *widget;
  Kind kind;
  int index;
};

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
std::string input_text(void *object);
gint caret(AtkText *object);
gint char_count(const std::string &text);
AtkObject *make(const Key &key, AtkObject *parent);
std::vector<Key> desired(AtkObject *object);

bool root_registered() noexcept;
void register_root(AtkObject *root);
void clear_root() noexcept;
void cancel_deferred();

// Fl_Menu_::size() walks the whole menu, and every menu node asks several
// times per tree pass or adaptor request; a pass reuses each menu's size.
// Menus never change while a pass runs: AT-SPI requests only read widgets
// or defer application callbacks to FLTK's loop.
struct MenuSize {
  const Fl_Menu_ *menu;
  const Fl_Menu_Item *items;
  int size;
};
class TreePass final {
public:
  TreePass();
  ~TreePass();
  TreePass(const TreePass &) = delete;
  TreePass &operator=(const TreePass &) = delete;
private:
  std::vector<MenuSize> sizes_;
  std::vector<MenuSize> *previous_;
};
int menu_size(const Fl_Menu_ &menu);

} // namespace layerc::editor::accessibility
