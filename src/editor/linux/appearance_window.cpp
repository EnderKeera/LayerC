#include "appearance_window.hpp"
#include "profile_mutations.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Light_Button.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>

#include <array>
#include <string>

namespace layerc::editor {
namespace {

Fl_Color fl(const std::uint32_t rgb) {
  return fl_rgb_color(static_cast<uchar>(rgb >> 16U), static_cast<uchar>(rgb >> 8U),
                      static_cast<uchar>(rgb));
}

// FLTK's startup palette, captured before the first theme replaces it.
struct SystemPalette {
  std::array<std::array<uchar, 3>, 4> rgb{};
  bool captured{};
  static constexpr std::array<Fl_Color, 4> indices{FL_BACKGROUND_COLOR, FL_BACKGROUND2_COLOR,
                                                   FL_FOREGROUND_COLOR, FL_SELECTION_COLOR};
  void capture() {
    if (captured) return;
    for (std::size_t index = 0; index < indices.size(); ++index)
      Fl::get_color(indices[index], rgb[index][0], rgb[index][1], rgb[index][2]);
    captured = true;
  }
  void restore() const {
    Fl::background(rgb[0][0], rgb[0][1], rgb[0][2]);
    Fl::background2(rgb[1][0], rgb[1][1], rgb[1][2]);
    Fl::foreground(rgb[2][0], rgb[2][1], rgb[2][2]);
    Fl::set_color(FL_SELECTION_COLOR, rgb[3][0], rgb[3][1], rgb[3][2]);
  }
};
SystemPalette system_palette;

void set_global(const RoleColors &window, const RoleColors &edit) {
  const auto channel = [](std::uint32_t rgb, unsigned shift) { return static_cast<uchar>(rgb >> shift); };
  Fl::background(channel(window.fill, 16), channel(window.fill, 8), channel(window.fill, 0));
  Fl::background2(channel(edit.fill, 16), channel(edit.fill, 8), channel(edit.fill, 0));
  Fl::foreground(channel(window.text, 16), channel(window.text, 8), channel(window.text, 0));
  Fl::set_color(FL_SELECTION_COLOR, channel(window.selection, 16), channel(window.selection, 8),
                channel(window.selection, 0));
}

std::optional<EditorRole> role_of(Fl_Widget *widget, const Fl_Widget *sidebar,
                                  const Fl_Widget *preview) {
  if (widget == sidebar) return EditorRole::sidebar;
  if (widget == preview) return EditorRole::preview;
  if (dynamic_cast<Fl_Menu_Bar *>(widget)) return EditorRole::toolbar;
  if (dynamic_cast<Fl_Output *>(widget)) return EditorRole::status;
  if (dynamic_cast<Fl_Input_ *>(widget) || dynamic_cast<Fl_Choice *>(widget)) return EditorRole::edit;
  if (dynamic_cast<Fl_Browser_ *>(widget)) return EditorRole::list;
  if (dynamic_cast<Fl_Light_Button *>(widget)) return EditorRole::checkbox;
  if (dynamic_cast<Fl_Button *>(widget)) return EditorRole::button;
  if (dynamic_cast<Fl_Box *>(widget)) return EditorRole::label;
  if (dynamic_cast<Fl_Group *>(widget)) return EditorRole::window;
  return std::nullopt;
}

// FLTK's class colours, which the Linux editor otherwise never changes.
RoleColors system_colors(Fl_Widget *widget, const EditorRole role) {
  const bool field = role == EditorRole::status || role == EditorRole::list ||
      role == EditorRole::sidebar || (role == EditorRole::edit && dynamic_cast<Fl_Input_ *>(widget));
  Fl_Color fill = field ? FL_BACKGROUND2_COLOR : FL_BACKGROUND_COLOR;
  if (role == EditorRole::preview) fill = fl_rgb_color(31, 36, 41);
  // Tabs and their page groups use this colour as the tab's fill, not a text highlight.
  const Fl_Color selection = (role == EditorRole::button || role == EditorRole::window)
                                 ? FL_BACKGROUND_COLOR
                                 : role == EditorRole::checkbox ? FL_FOREGROUND_COLOR
                                                                : FL_SELECTION_COLOR;
  const auto rgb = [](Fl_Color color) {
    uchar r{}, g{}, b{};
    Fl::get_color(color, r, g, b);
    return (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b;
  };
  return {rgb(fill), rgb(FL_FOREGROUND_COLOR), rgb(selection)};
}

void paint(Fl_Widget *widget, const RoleColors &colors, const Fl_Color label) {
  widget->color(fl(colors.fill));
  widget->selection_color(fl(colors.selection));
  widget->labelcolor(label);
  if (auto *input = dynamic_cast<Fl_Input_ *>(widget)) input->textcolor(fl(colors.text));
  else if (auto *browser = dynamic_cast<Fl_Browser_ *>(widget)) browser->textcolor(fl(colors.text));
  else if (auto *menu = dynamic_cast<Fl_Menu_ *>(widget)) menu->textcolor(fl(colors.text));
  if (dynamic_cast<Fl_Button *>(widget) || dynamic_cast<Fl_Box *>(widget)) widget->labelcolor(fl(colors.text));
}

void apply_tree(Fl_Widget *widget, const EditorAppearance &appearance, const Fl_Widget *sidebar,
                const Fl_Widget *preview, const Fl_Color label) {
  if (const auto role = role_of(widget, sidebar, preview)) {
    auto colors = theme_role_colors(appearance, *role);
    if (!colors) {
      colors = system_colors(widget, *role);
      const auto &item = appearance.overrides[static_cast<std::size_t>(*role)];
      if (item.fill) colors->fill = *item.fill;
      if (item.text) colors->text = *item.text;
      if (item.selection) colors->selection = *item.selection;
    }
    paint(widget, *colors, label);
    // Fl_Tabs fills each unselected tab with its page's selection colour and
    // only the selected tab with its own: neutral pages keep the accent tint
    // on the selected tab alone.
    if (const auto button = theme_role_colors(appearance, EditorRole::button);
        button && widget->as_group() && dynamic_cast<Fl_Tabs *>(widget->parent()))
      widget->selection_color(fl(button->fill));
  }
  if (auto *group = widget->as_group())
    for (int index = 0; index < group->children(); ++index)
      apply_tree(group->child(index), appearance, sidebar, preview, label);
}

std::string hex_or_empty(const std::optional<std::uint32_t> &color) {
  return color ? format_rgb(*color) : std::string{};
}

class AppearanceDialog final : public Fl_Double_Window {
public:
  AppearanceDialog(const EditorAppearance &current, Fl_Window &root, const Fl_Widget *sidebar,
                   const Fl_Widget *preview)
      : Fl_Double_Window(520, 430, "Appearance — LayerE"), staged(current), root_(root),
        sidebar_(sidebar), preview_(preview) {
    begin();
    theme = new Fl_Choice(150, 15, 355, 28, "Theme");
    // Menu paths treat '/' as a submenu: add a placeholder, then the label.
    for (std::size_t index = 0; index < static_cast<std::size_t>(EditorTheme::count); ++index) {
      const auto item = theme->add(("theme-" + std::to_string(index)).c_str());
      theme->replace(item, std::string(theme_label(static_cast<EditorTheme>(index))).c_str());
    }
    accent = input(150, 55, 200, "Accent #RRGGBB");
    chooser(360, 55, "Choose accent...", &AppearanceDialog::choose_accent);
    auto *note = new Fl_Box(15, 90, 490, 40, "An empty accent uses the preset's. Colours below "
        "override one role; empty fields use the theme.");
    note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    role = new Fl_Choice(150, 135, 355, 28, "Role");
    for (std::size_t index = 0; index < editor_role_count; ++index)
      role->add(std::string(role_label(static_cast<EditorRole>(index))).c_str());
    role->value(0);
    role->callback([](Fl_Widget *, void *self) { static_cast<AppearanceDialog *>(self)->load_role(); }, this);
    fill = input(150, 175, 200, "Role fill");
    chooser(360, 175, "Choose fill...", &AppearanceDialog::choose_fill);
    text = input(150, 210, 200, "Role text");
    chooser(360, 210, "Choose text...", &AppearanceDialog::choose_text);
    selection = input(150, 245, 200, "Role selection");
    chooser(360, 245, "Choose selection...", &AppearanceDialog::choose_selection);
    auto *reset_role = new Fl_Button(150, 285, 170, 30, "Reset role colours");
    reset_role->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      dialog.staged.overrides[static_cast<std::size_t>(dialog.role->value())] = {};
      dialog.load_role(); dialog.preview_changes();
    }, this);
    auto *reset_all = new Fl_Button(335, 285, 170, 30, "Reset all colours");
    reset_all->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      dialog.staged.overrides = {}; dialog.staged.accent.reset();
      dialog.load(); dialog.preview_changes();
    }, this);
    status = new Fl_Box(15, 325, 490, 50);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    auto *cancel = new Fl_Button(265, 385, 115, 30, "Cancel");
    cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
    auto *apply = new Fl_Button(390, 385, 115, 30, "Apply");
    apply->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      if (dialog.read_fields()) { dialog.applied = true; dialog.hide(); }
    }, this);
    theme->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      dialog.staged.theme = static_cast<EditorTheme>(dialog.theme->value());
      dialog.preview_changes();
    }, this);
    end();
    set_modal();
    load();
  }

  EditorAppearance staged;
  bool applied{};

private:
  using Choose = void (AppearanceDialog::*)();
  Fl_Input *input(int x, int y, int width, const char *label) {
    auto *field = new Fl_Input(x, y, width, 28, label);
    field->when(FL_WHEN_CHANGED);
    field->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      if (dialog.read_fields()) dialog.preview_changes();
    }, this);
    return field;
  }
  void chooser(int x, int y, const char *label, Choose choose) {
    auto *button = new Fl_Button(x, y, 145, 28, label);
    choosers_[chooser_count_] = {this, choose};
    button->callback([](Fl_Widget *, void *data) {
      const auto &target = *static_cast<const Target *>(data);
      (target.dialog->*target.choose)();
    }, &choosers_[chooser_count_++]);
  }
  void pick(Fl_Input *field, const char *title, std::uint32_t fallback) {
    const auto initial = parse_rgb(field->value()).value_or(fallback);
    if (const auto color = choose_color(title, initial)) {
      field->value(format_rgb(*color).c_str());
      if (read_fields()) preview_changes();
    }
  }
  [[nodiscard]] RoleColors current_role_colors() const {
    const auto colors = theme_role_colors(staged, static_cast<EditorRole>(role->value()));
    return colors.value_or(RoleColors{0xFFFFFF, 0x000000, 0x0000FF});
  }
  void choose_accent() {
    pick(accent, "Accent colour", staged.accent.value_or(preset_accent(staged.theme)));
  }
  void choose_fill() { pick(fill, "Role fill colour", current_role_colors().fill); }
  void choose_text() { pick(text, "Role text colour", current_role_colors().text); }
  void choose_selection() { pick(selection, "Role selection colour", current_role_colors().selection); }

  void load() {
    theme->value(static_cast<int>(staged.theme));
    accent->value(hex_or_empty(staged.accent).c_str());
    load_role();
  }
  void load_role() {
    const auto &item = staged.overrides[static_cast<std::size_t>(role->value())];
    fill->value(hex_or_empty(item.fill).c_str());
    text->value(hex_or_empty(item.text).c_str());
    selection->value(hex_or_empty(item.selection).c_str());
  }
  // Empty means "not overridden"; anything else must be #RRGGBB.
  [[nodiscard]] bool read_fields() {
    std::string invalid;
    const auto read = [&](Fl_Input *field, const char *name) -> std::optional<std::uint32_t> {
      const std::string_view value = field->value();
      if (value.empty()) return std::nullopt;
      auto color = parse_rgb(value);
      if (!color && invalid.empty()) invalid = name;
      return color;
    };
    const auto accent_value = read(accent, "Accent");
    auto &item = staged.overrides[static_cast<std::size_t>(role->value())];
    RoleOverride next{read(fill, "Role fill"), read(text, "Role text"), read(selection, "Role selection")};
    if (!invalid.empty()) {
      status->copy_label((invalid + " must be #RRGGBB or empty; the last valid colours stay previewed.").c_str());
      status->redraw();
      return false;
    }
    staged.accent = accent_value;
    item = next;
    status->copy_label("");
    status->redraw();
    return true;
  }
  void preview_changes() {
    apply_editor_appearance(staged, root_, sidebar_, preview_);
    apply_editor_appearance(staged, *this);
  }

  struct Target { AppearanceDialog *dialog{}; Choose choose{}; };
  std::array<Target, 4> choosers_{};
  std::size_t chooser_count_{};
  Fl_Choice *theme{}, *role{};
  Fl_Input *accent{}, *fill{}, *text{}, *selection{};
  Fl_Box *status{};
  Fl_Window &root_;
  const Fl_Widget *sidebar_;
  const Fl_Widget *preview_;
};

} // namespace

void apply_editor_appearance(const EditorAppearance &appearance, Fl_Window &root,
                             const Fl_Widget *sidebar, const Fl_Widget *preview) {
  system_palette.capture();
  const auto window = theme_role_colors(appearance, EditorRole::window);
  const auto edit = theme_role_colors(appearance, EditorRole::edit);
  if (window && edit) set_global(*window, *edit);
  else system_palette.restore();
  const auto label = theme_role_colors(appearance, EditorRole::label);
  const auto &label_override = appearance.overrides[static_cast<std::size_t>(EditorRole::label)];
  const Fl_Color label_color = label ? fl(label->text)
                               : label_override.text ? fl(*label_override.text) : FL_FOREGROUND_COLOR;
  apply_tree(&root, appearance, sidebar, preview, label_color);
  root.redraw();
}

std::optional<std::uint32_t> choose_color(const char *title, const std::uint32_t initial) {
  auto r = static_cast<uchar>(initial >> 16U), g = static_cast<uchar>(initial >> 8U),
       b = static_cast<uchar>(initial);
  if (!fl_color_chooser(title, r, g, b, 2)) return std::nullopt;
  return (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b;
}

std::optional<EditorAppearance>
edit_editor_appearance(const EditorAppearance &current, Fl_Window &root,
                       const Fl_Widget *sidebar, const Fl_Widget *preview) {
  AppearanceDialog dialog(current, root, sidebar, preview);
  apply_editor_appearance(current, dialog);
  dialog.show();
  while (dialog.shown()) Fl::wait();
  if (!dialog.applied) {
    apply_editor_appearance(current, root, sidebar, preview);
    return std::nullopt;
  }
  return dialog.staged;
}

} // namespace layerc::editor
