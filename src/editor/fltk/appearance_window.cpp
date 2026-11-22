#include "appearance_window.hpp"
#include "accessible_widgets.hpp"
#include "editor_style.hpp"
#include "profile_mutations.hpp"
#include "platform/window_chrome.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Light_Button.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Tooltip.H>
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
  if (role == EditorRole::preview) {
    const auto input = current_editor_tokens().input;
    fill = fl_rgb_color(static_cast<uchar>((input >> 16U) & 0xFFU),
                        static_cast<uchar>((input >> 8U) & 0xFFU),
                        static_cast<uchar>(input & 0xFFU));
  }
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

void paint(Fl_Widget *widget, const RoleColors &colors, const Fl_Color label,
           const bool use_verbose) {
  widget->color(fl(colors.fill));
  widget->selection_color(fl(colors.selection));
  widget->labelcolor(label);
  if (auto *input = dynamic_cast<Fl_Input_ *>(widget)) input->textcolor(fl(colors.text));
  else if (auto *browser = dynamic_cast<Fl_Browser_ *>(widget)) browser->textcolor(fl(colors.text));
  else if (auto *menu = dynamic_cast<Fl_Menu_ *>(widget)) menu->textcolor(fl(colors.text));
  if (dynamic_cast<Fl_Button *>(widget) || dynamic_cast<Fl_Box *>(widget)) widget->labelcolor(fl(colors.text));
  // Mono diagnostics keep their own ink at the verbose tone unless the
  // status role overrides it (DesignSystem.md, Status strip).
  if (auto *output = dynamic_cast<Fl_Multiline_Output *>(widget);
      output != nullptr && output->textfont() == FL_COURIER && use_verbose) {
    output->textcolor(fl(current_editor_tokens().verbose));
    return;
  }
  // A primary button keeps its accent fill and contrasting label through a
  // live theme-role application (DesignSystem.md, Buttons).
  if (auto *button = dynamic_cast<Fl_Button *>(widget);
      button != nullptr && is_primary_button(*button))
    refresh_primary_button(*button);
}

std::optional<RoleColors> applied_role_colors(const EditorAppearance &appearance,
                                              const EditorRole role) {
  if (const auto &contrast = editor_style_contrast_palette())
    return contrast->roles[static_cast<std::size_t>(role)];
  return theme_role_colors(appearance, role);
}

void apply_tree(Fl_Widget *widget, const EditorAppearance &appearance, const Fl_Widget *sidebar,
                const Fl_Widget *preview, const Fl_Color label) {
  if (const auto role = role_of(widget, sidebar, preview)) {
    auto colors = applied_role_colors(appearance, *role);
    if (!colors) {
      colors = system_colors(widget, *role);
      const auto &item = appearance.overrides[static_cast<std::size_t>(*role)];
      if (item.fill) colors->fill = *item.fill;
      if (item.text) colors->text = *item.text;
      if (item.selection) colors->selection = *item.selection;
    }
    paint(widget, *colors, label,
          editor_style_contrast_palette() ||
              !appearance.overrides[static_cast<std::size_t>(*role)].text.has_value());
    // Fl_Tabs fills each unselected tab with its page's selection colour and
    // only the selected tab with its own: neutral pages keep the accent tint
    // on the selected tab alone.
    if (const auto button = applied_role_colors(appearance, EditorRole::button);
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
    theme = new EditorChoice(150, 15, 355, editor_input_height, "Theme");
    // Menu paths treat '/' as a submenu: add a placeholder, then the label.
    for (std::size_t index = 0; index < static_cast<std::size_t>(EditorTheme::count); ++index) {
      const auto item = theme->add(("theme-" + std::to_string(index)).c_str());
      theme->replace(item, std::string(theme_label(static_cast<EditorTheme>(index))).c_str());
    }
    accent = input(150, 55, 200, "Accent #RRGGBB");
    chooser(360, 55, "Choose accent...", &AppearanceDialog::choose_accent);
    auto *note = new Fl_Box(15, 90, 490, 40, "An empty accent uses the preset's. Colors below "
        "override one role; empty fields use the theme.");
    note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    role = new EditorChoice(150, 135, 355, editor_input_height, "Role");
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
    auto *reset_role = new Fl_Button(150, 285, 170, editor_button_height, "Reset role colors");
    reset_role->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      dialog.staged.overrides[static_cast<std::size_t>(dialog.role->value())] = {};
      dialog.load_role(); dialog.preview_changes();
    }, this);
    auto *reset_all = new Fl_Button(335, 285, 170, editor_button_height, "Reset all colors");
    reset_all->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      dialog.staged.overrides = {}; dialog.staged.accent.reset();
      dialog.load(); dialog.preview_changes();
    }, this);
    status = new Fl_Box(15, 325, 490, 50);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    // The line under the fields explains every colour control the way the
    // role list's contrast line does (DesignSystem.md, Screen readers).
    for (Fl_Widget *control : std::array<Fl_Widget *, 8>{
             accent, fill, text, selection, choosers_[0].button, choosers_[1].button,
             choosers_[2].button, choosers_[3].button})
      accessibility::describe_with(*control, *status);
    auto *cancel = new Fl_Button(265, 385, 115, editor_button_height, "Cancel");
    cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
    auto *apply = new Fl_Button(390, 385, 115, editor_button_height, "Apply");
    // Cancel stays left, the primary Apply right (DesignSystem.md, Dialogs).
    primary_button(*apply);
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

  void refresh_contrast_notice() {
    if (invalid_fields_) return;
    status->label(editor_style_contrast_palette()
        ? "A contrast theme is on, so Windows colors are in use." : "");
    status->redraw();
  }

  EditorAppearance staged;
  bool applied{};

private:
  using Choose = void (AppearanceDialog::*)();
  Fl_Input *input(int x, int y, int width, const char *label) {
    auto *field = new EditorField<Fl_Input>(x, y, width, editor_input_height, label);
    field->when(FL_WHEN_CHANGED);
    field->callback([](Fl_Widget *, void *self) {
      auto &dialog = *static_cast<AppearanceDialog *>(self);
      if (dialog.read_fields()) dialog.preview_changes();
    }, this);
    return field;
  }
  void chooser(int x, int y, const char *label, Choose choose) {
    auto *button = new Fl_Button(x, y, 145, editor_button_height, label);
    choosers_[chooser_count_] = {this, choose, button, label};
    button->callback([](Fl_Widget *, void *data) {
      const auto &target = *static_cast<const Target *>(data);
      (target.dialog->*target.choose)();
    }, &choosers_[chooser_count_++]);
  }
  // Each chooser button says the colour it would open with: what its field
  // parses, else the fallback pick() starts from (DesignSystem.md, Screen
  // readers).
  void name_choosers() {
    const std::array<Fl_Input *, 4> fields{accent, fill, text, selection};
    for (std::size_t index = 0; index < chooser_count_; ++index) {
      const auto &target = choosers_[index];
      if (target.button == nullptr || target.label == nullptr) continue;
      const auto color =
          parse_rgb(fields[index]->value()).value_or(fallback_color(index));
      accessibility::set_accessible_name(
          *target.button,
          accessibility::color_button_name(target.label, format_rgb(color)));
    }
  }
  [[nodiscard]] std::uint32_t fallback_color(const std::size_t chooser) const {
    const auto role_colors = current_role_colors();
    if (chooser == 0) return staged.accent.value_or(preset_accent(staged.theme));
    return chooser == 1   ? role_colors.fill
           : chooser == 2 ? role_colors.text
                          : role_colors.selection;
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
    const auto tokens = current_editor_tokens();
    return colors.value_or(RoleColors{tokens.input, tokens.text, tokens.selection});
  }
  void choose_accent() {
    pick(accent, "Accent color", staged.accent.value_or(preset_accent(staged.theme)));
  }
  void choose_fill() { pick(fill, "Role fill color", current_role_colors().fill); }
  void choose_text() { pick(text, "Role text color", current_role_colors().text); }
  void choose_selection() { pick(selection, "Role selection color", current_role_colors().selection); }

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
    name_choosers();
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
      invalid_fields_ = true;
      status->copy_label((invalid + " must be #RRGGBB or empty; the last valid colors stay previewed.").c_str());
      status->redraw();
      return false;
    }
    staged.accent = accent_value;
    item = next;
    invalid_fields_ = false;
    refresh_contrast_notice();
    return true;
  }
  void preview_changes() {
    apply_editor_appearance(staged, root_, sidebar_, preview_);
    apply_editor_appearance(staged, *this);
    name_choosers();
  }

  bool invalid_fields_{};

  struct Target {
    AppearanceDialog *dialog{};
    Choose choose{};
    Fl_Button *button{};
    const char *label{};
  };
  std::array<Target, 4> choosers_{};
  std::size_t chooser_count_{};
  Fl_Choice *theme{}, *role{};
  Fl_Input *accent{}, *fill{}, *text{}, *selection{};
  Fl_Box *status{};
  Fl_Window &root_;
  const Fl_Widget *sidebar_;
  const Fl_Widget *preview_;
};

void set_appearance_palette(const EditorAppearance &appearance) {
  system_palette.capture();
  set_editor_style_appearance(appearance);
  const auto window = applied_role_colors(appearance, EditorRole::window);
  const auto edit = applied_role_colors(appearance, EditorRole::edit);
  if (window && edit) set_global(*window, *edit);
  else system_palette.restore();
  const auto toolbar = current_editor_toolbar_colors();
  Fl_Tooltip::color(fl(toolbar.fill));
  Fl_Tooltip::textcolor(fl(toolbar.text));
  Fl_Tooltip::size(FL_NORMAL_SIZE);
  Fl_Tooltip::wrap_width(360);
}

void paint_appearance_window(const EditorAppearance &appearance, Fl_Window &root,
                             const Fl_Widget *sidebar, const Fl_Widget *preview) {
  const auto label = applied_role_colors(appearance, EditorRole::label);
  const auto &label_override = appearance.overrides[static_cast<std::size_t>(EditorRole::label)];
  const Fl_Color label_color = label ? fl(label->text)
                               : label_override.text ? fl(*label_override.text) : FL_FOREGROUND_COLOR;
  apply_tree(&root, appearance, sidebar, preview, label_color);
  if (auto *dialog = dynamic_cast<AppearanceDialog *>(&root)) dialog->refresh_contrast_notice();
  root.redraw();
}

} // namespace

void apply_editor_appearance(const EditorAppearance &appearance, Fl_Window &root,
                             const Fl_Widget *sidebar, const Fl_Widget *preview) {
  set_appearance_palette(appearance);
  paint_appearance_window(appearance, root, sidebar, preview);
  refresh_editor_window_chrome();
}

void reapply_editor_desktop_appearance(Fl_Window &root, const Fl_Widget *sidebar,
                                       const Fl_Widget *preview) {
  const auto appearance = current_editor_style_appearance();
  set_appearance_palette(appearance);
  paint_appearance_window(appearance, root, sidebar, preview);
  for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
    if (window != &root) paint_appearance_window(appearance, *window, nullptr, nullptr);
  refresh_editor_window_chrome();
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
