#pragma once

#include "editor_commands.hpp"
#include "editor_menus.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Multiline_Output.H>

#include <optional>
#include <string>

class Fl_Button;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Group;
class Fl_Input;

namespace layerc::core {
struct GameProfile;
struct CooldownDefinition;
struct Panel;
}

namespace layerc::editor {
class EditorApp;

// Adds an authored name as a labelled choice item ('/' and '&' are text).
void add_choice(Fl_Choice &choice, const std::string &name);

// Builds the main window's standard controls with EditorApp's field, selection
// and command callbacks. Geometry and creation order belong to the callers.
class FormBuilder final {
public:
  explicit FormBuilder(EditorApp &app) noexcept : app_(app) {}
  [[nodiscard]] Fl_Input *input(int x, int y, int width, const char *label);
  [[nodiscard]] Fl_Choice *choice(int x, int y, int width, const char *label,
                                  const char *items);
  [[nodiscard]] Fl_Check_Button *check(int x, int y, int width, const char *label);
  [[nodiscard]] ContextBrowser *browser(int x, int y, int width, int height,
                                        const char *label, EditorMenuContext context);
  [[nodiscard]] Fl_Button *button(int x, int y, int width, const char *label,
                                  EditorCommand action);
private:
  EditorApp &app_;
};

// The status column and footer: the outcome panes, the module-fault pane, the
// accessibility label and the history/save buttons.
class StatusPane final {
public:
  explicit StatusPane(EditorApp &app) noexcept : app_(app) {}
  void build_modules(FormBuilder &builder, int x, int y, int width, int height);
  void build_footer(FormBuilder &builder);
  void set_status(const std::string &text);
  void set_daemon(const std::string &text);
  void set_modules(const std::string &text);
  void set_accessibility(const std::string &text);
  void set_history(bool undo_active, bool redo_active);
  [[nodiscard]] std::string accessibility_text() const;
private:
  friend struct EditorAppTestAccess;
  [[nodiscard]] Fl_Multiline_Output *status() const { return status_; }
  [[nodiscard]] Fl_Multiline_Output *daemon() const { return daemon_status_; }
  [[nodiscard]] Fl_Multiline_Output *modules() const { return module_status_; }
  [[nodiscard]] Fl_Box *accessibility() const { return accessibility_status_; }
  [[nodiscard]] Fl_Button *undo() const { return undo_button_; }
  [[nodiscard]] Fl_Button *redo() const { return redo_button_; }
  EditorApp &app_;
  Fl_Multiline_Output *status_{};
  Fl_Multiline_Output *daemon_status_{};
  Fl_Multiline_Output *module_status_{};
  Fl_Box *accessibility_status_{};
  Fl_Button *undo_button_{};
  Fl_Button *redo_button_{};
};

// The Profile tab: identity, executable and window matching fields.
class ProfilePage final {
public:
  explicit ProfilePage(EditorApp &app) noexcept : app_(app) {}
  void build(FormBuilder &builder);
  [[nodiscard]] Fl_Group *group() const { return profile_tab_; }
  // Fills every field from the document, or clears the identity for no document.
  void show(const layerc::core::GameProfile &profile);
  void show(const layerc::core::GameProfile *profile);
  // Writes only the identity line, so a refresh cannot disturb typing.
  void show_identity(const std::string &profile_id);
  void read(layerc::core::GameProfile &candidate) const;
  void focus_name();
  void set_enabled_value(bool enabled);
  [[nodiscard]] bool owns(const Fl_Widget *widget) const;
private:
  friend struct EditorAppTestAccess;
  [[nodiscard]] Fl_Input *name() const { return name_; }
  [[nodiscard]] Fl_Input *executable() const { return executable_; }
  [[nodiscard]] Fl_Input *executable_path() const { return executable_path_; }
  [[nodiscard]] Fl_Input *window_class() const { return window_class_; }
  [[nodiscard]] Fl_Input *window_title() const { return window_title_; }
  [[nodiscard]] Fl_Check_Button *enabled() const { return enabled_; }
  [[nodiscard]] Fl_Multiline_Output *profile_id() const { return profile_id_; }
  EditorApp &app_;
  Fl_Group *profile_tab_{};
  Fl_Input *name_{};
  Fl_Input *executable_{};
  Fl_Input *executable_path_{};
  Fl_Input *window_class_{};
  Fl_Input *window_title_{};
  Fl_Check_Button *enabled_{};
  Fl_Multiline_Output *profile_id_{};
};

// The Cooldowns tab: the list, its buttons and the five selected-item fields.
class CooldownPage final {
public:
  explicit CooldownPage(EditorApp &app) noexcept : app_(app) {}
  void build(FormBuilder &builder);
  [[nodiscard]] Fl_Group *group() const { return cooldown_tab_; }
  void clear_list();
  void fill_list(const layerc::core::GameProfile &profile,
                 std::optional<std::size_t> selected);
  void show(const layerc::core::GameProfile &profile, std::optional<std::size_t> index);
  // The F17 field-specific messages, verbatim, or nullopt when the buffer is valid.
  [[nodiscard]] std::optional<std::string>
  read(layerc::core::CooldownDefinition &item) const;
  void focus_name();
  [[nodiscard]] bool owns_list(const Fl_Widget *widget) const;
  [[nodiscard]] bool owns(const Fl_Widget *widget) const;
private:
  friend struct EditorAppTestAccess;
  [[nodiscard]] ContextBrowser *list() const { return cooldowns_; }
  [[nodiscard]] Fl_Input *name() const { return cooldown_name_; }
  [[nodiscard]] Fl_Input *duration() const { return duration_; }
  [[nodiscard]] Fl_Input *charges() const { return charges_; }
  [[nodiscard]] Fl_Choice *recovery() const { return recovery_; }
  [[nodiscard]] Fl_Choice *retrigger() const { return retrigger_; }
  EditorApp &app_;
  Fl_Group *cooldown_tab_{};
  Fl_Group *cooldown_fields_{};
  ContextBrowser *cooldowns_{};
  Fl_Input *cooldown_name_{};
  Fl_Input *duration_{};
  Fl_Input *charges_{};
  Fl_Choice *recovery_{};
  Fl_Choice *retrigger_{};
};

// The Bindings tab: the list, the target/key/mouse inputs and the capture note.
class BindingPage final {
public:
  explicit BindingPage(EditorApp &app) noexcept : app_(app) {}
  void build(FormBuilder &builder);
  [[nodiscard]] Fl_Group *group() const { return binding_tab_; }
  void clear_list();
  void fill_list(const layerc::core::GameProfile &profile, int selected);
  // Rebuilds the target choice and keeps the selection clamp.
  void show(const layerc::core::GameProfile &profile);
  [[nodiscard]] int target() const;
  [[nodiscard]] std::string key_code() const;
  [[nodiscard]] int mouse() const;
  [[nodiscard]] bool owns_list(const Fl_Widget *widget) const;
  [[nodiscard]] bool owns(const Fl_Widget *widget) const;
private:
  friend struct EditorAppTestAccess;
  [[nodiscard]] ContextBrowser *list() const { return bindings_; }
  [[nodiscard]] Fl_Choice *target_choice() const { return binding_target_; }
  [[nodiscard]] Fl_Input *key_code_field() const { return key_code_; }
  [[nodiscard]] Fl_Choice *mouse_choice() const { return mouse_button_; }
  EditorApp &app_;
  Fl_Group *binding_tab_{};
  ContextBrowser *bindings_{};
  Fl_Choice *binding_target_{};
  Fl_Input *key_code_{};
  Fl_Choice *mouse_button_{};
};

// The Panels tab: the list, its buttons and the placement fields.
class PanelPage final {
public:
  explicit PanelPage(EditorApp &app) noexcept : app_(app) {}
  void build(FormBuilder &builder);
  [[nodiscard]] Fl_Group *group() const { return panel_tab_; }
  void clear_list();
  void fill_list(const layerc::core::GameProfile &profile,
                 std::optional<std::size_t> selected);
  void show(const layerc::core::GameProfile &profile, std::optional<std::size_t> selected);
  [[nodiscard]] std::optional<std::string> read(layerc::core::Panel &item) const;
  [[nodiscard]] int member_index() const;
  void set_member_index(int index);
  [[nodiscard]] bool member_checked() const;
  [[nodiscard]] bool owns_member(const Fl_Widget *widget) const;
  [[nodiscard]] bool owns_member_toggle(const Fl_Widget *widget) const;
  [[nodiscard]] bool owns_list(const Fl_Widget *widget) const;
  [[nodiscard]] bool owns(const Fl_Widget *widget) const;
  void show_position(bool active);
private:
  friend struct EditorAppTestAccess;
  EditorApp &app_;
  Fl_Group *panel_tab_{}, *panel_fields_{};
  ContextBrowser *panels_{};
  Fl_Input *panel_name_{}, *anchor_x_{}, *anchor_y_{}, *bar_width_{}, *bar_height_{};
  Fl_Input *fill_{}, *background_{}, *border_{}, *text_color_{};
  Fl_Choice *layout_{}, *style_{}, *member_{};
  Fl_Check_Button *member_enabled_{}, *show_text_{}, *hide_ready_{};
  Fl_Button *position_button_{};
};

} // namespace layerc::editor
