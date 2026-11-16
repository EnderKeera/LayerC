#pragma once

#include "editor_session.hpp"

#include <FL/Fl_Double_Window.H>
#include <vector>

class Fl_Box;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Hold_Browser;
class Fl_Input;

namespace layerc::editor {
// Demand-created modal. All fields remain local until a complete, validated
// session edit succeeds; Cancel and failed Apply have no document effects.
class CooldownGroupsDialog final : public Fl_Double_Window {
public:
  explicit CooldownGroupsDialog(EditorSession &session);
  Fl_Hold_Browser *groups{};
  Fl_Input *id{}, *name{}, *duration{}, *charges{};
  Fl_Choice *mode{}, *recovery{}, *retrigger{};
  std::vector<Fl_Check_Button *> members;
  bool applied{};
private:
  EditorSession &session_;
  core::GameProfile source_;
  std::vector<core::CooldownGroup> staged_;
  Fl_Box *status_{};
  int selected_{-1};
  void show_error(const std::string &text);
  void fill_list();
  void load();
  [[nodiscard]] bool read_fields();
  void select();
  void add();
  void remove();
  void apply();
};
} // namespace layerc::editor
