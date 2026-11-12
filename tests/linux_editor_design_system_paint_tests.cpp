// Native FLTK painting contracts. All windows belong to this disposable test;
// no input events or desktop settings are synthesized or changed.
#include "appearance_window.hpp"
#include "editor_geometry.hpp"
#include "editor_menus.hpp"
#include "editor_style.hpp"
#include "test_support.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {
using layerc::test::expect;
namespace editor = layerc::editor;

class PaintWindow final : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void paint() { damage(FL_DAMAGE_ALL); draw(); }
};

class EmptyStateBox final : public Fl_Box {
public:
  using Fl_Box::Fl_Box;
protected:
  void draw() override {
    Fl_Box::draw();
    editor::draw_editor_empty_state({x(), y(), w(), h()},
        editor::editor_empty_state(editor::EditorMenuContext::profiles), FL_HELVETICA, FL_NORMAL_SIZE);
  }
};

class Offscreen final {
public:
  Offscreen(int width, int height) : surface_(fl_create_offscreen(width, height)) {
    expect(surface_ != 0, "the fixture gets an offscreen drawing surface");
    fl_begin_offscreen(surface_);
  }
  ~Offscreen() { fl_end_offscreen(); fl_delete_offscreen(surface_); }
  Offscreen(const Offscreen &) = delete;
  Offscreen &operator=(const Offscreen &) = delete;
private:
  Fl_Offscreen surface_{};
};

struct Pixels {
  int width{};
  std::vector<unsigned char> rgb;
  [[nodiscard]] std::uint32_t at(int x, int y) const {
    const auto index = static_cast<std::size_t>((y * width + x) * 3);
    return (static_cast<std::uint32_t>(rgb.at(index)) << 16U) |
           (static_cast<std::uint32_t>(rgb.at(index + 1)) << 8U) | rgb.at(index + 2);
  }
};

Pixels capture(PaintWindow &window) {
  window.make_current();
  Offscreen surface(window.w(), window.h());
  window.paint();
  std::unique_ptr<unsigned char[]> pixels(fl_read_image(nullptr, 0, 0, window.w(), window.h()));
  expect(pixels != nullptr, "the fixture's pixels can be read");
  const auto bytes = static_cast<std::size_t>(window.w() * window.h() * 3);
  return {window.w(), std::vector<unsigned char>(pixels.get(), pixels.get() + bytes)};
}

void ring(const Pixels &pixels, const Fl_Widget &widget, std::uint32_t color) {
  expect(pixels.at(widget.x(), widget.y()) == color &&
             pixels.at(widget.x() + 1, widget.y() + 1) == color &&
             pixels.at(widget.x() + widget.w() - 1, widget.y() + widget.h() - 1) == color &&
             pixels.at(widget.x() + widget.w() - 2, widget.y() + widget.h() - 2) == color,
         "focus is a square two-pixel outline around the full widget");
}

class RestoreSystem final {
public:
  explicit RestoreSystem(Fl_Window &window) : window_(window) {}
  ~RestoreSystem() {
    Fl::focus(nullptr);
    editor::EditorAppearance system;
    system.theme = editor::EditorTheme::system;
    editor::apply_editor_appearance(system, window_);
  }
private:
  Fl_Window &window_;
};
} // namespace

void linux_editor_design_system_paint_contracts() {
  layerc::test::require_x11();
  editor::install_editor_style();
  PaintWindow window(440, 200, "LayerE paint test fixture");
  editor::EditorField<Fl_Input> field(20, 25, 260, editor::editor_input_height);
  editor::EditorChoice choice(20, 80, 260, editor::editor_input_height);
  choice.add("Copper|Teal");
  choice.value(0);
  Fl_Check_Button check(20, 130, 260, editor::editor_input_height, "Whole checkbox label");
  Fl_Button apply(310, 25, 110, editor::editor_button_height, "Apply");
  editor::primary_button(apply);
  window.end();
  RestoreSystem restore(window);
  editor::EditorAppearance appearance;
  editor::apply_editor_appearance(appearance, window);
  window.show();
  Fl::check();
  const auto tokens = editor::current_editor_tokens();
  Fl::focus(nullptr);
  const auto before = capture(window);
  Fl::focus(&field);
  const auto focused_field = capture(window);
  ring(focused_field, field, tokens.accent);
  expect(focused_field.at(field.x() + field.w() - 8, field.y() + field.h() / 2) ==
             before.at(field.x() + field.w() - 8, field.y() + field.h() / 2),
         "focusing an edit does not tint its fill");
  Fl::focus(&choice);
  const auto focused_choice = capture(window);
  ring(focused_choice, choice, tokens.accent);
  expect(choice.value() == 0 &&
             focused_choice.at(choice.x() + choice.w() - 35, choice.y() + choice.h() / 2) ==
                 before.at(choice.x() + choice.w() - 35, choice.y() + choice.h() / 2),
         "a focused choice keeps both its selection and its ordinary fill");
  Fl::focus(&check);
  ring(capture(window), check, tokens.accent);
  Fl::focus(&apply);
  ring(capture(window), apply, tokens.text);
}

void linux_editor_design_system_tab_and_glyph_contracts() {
  layerc::test::require_x11();
  editor::install_editor_style();
  PaintWindow window(440, 230, "LayerE tab and glyph test fixture");
  editor::EditorTabs tabs(20, 20, 400, 130);
  Fl_Group first(20, 55, 400, 95, "First tab");
  first.end();
  Fl_Group second(20, 55, 400, 95, "Second tab");
  second.end();
  tabs.end();
  tabs.value(&first);
  editor::EditorStatusMark mark(20, 175, 18, 38);
  window.end();
  RestoreSystem restore(window);
  editor::EditorAppearance appearance;
  editor::apply_editor_appearance(appearance, window);
  window.show();
  Fl::check();
  Fl::focus(nullptr);
  const auto tokens = editor::current_editor_tokens();
  const auto bounds = tabs.tab_bounds(0);
  expect(bounds.has_value(), "the selected tab exposes its measured bounds");
  const auto ordinary = capture(window);
  const int center = bounds->x + bounds->width / 2;
  expect(ordinary.at(center, bounds->y) == tokens.accent &&
             ordinary.at(center, bounds->y + 2) == tokens.accent,
         "the selected tab has a three-pixel TOP accent rail without focus");
  Fl::focus(&tabs);
  const auto focused = capture(window);
  expect(focused.at(bounds->x, bounds->y) == tokens.accent &&
             focused.at(bounds->x + 1, bounds->y + bounds->height - 2) == tokens.accent,
         "tab focus outlines the selected tab, not the page or its content");
  Fl::focus(nullptr);
  const std::array glyphs{editor::EditorStatusGlyph::yes, editor::EditorStatusGlyph::no,
                          editor::EditorStatusGlyph::warning, editor::EditorStatusGlyph::information};
  const std::array colors{tokens.yes, tokens.no, tokens.warning, tokens.accent_2};
  for (std::size_t index = 0; index < glyphs.size(); ++index) {
    mark.glyph(glyphs[index]);
    const auto image = capture(window);
    int ink = 0;
    for (int y = mark.y(); y < mark.y() + mark.h(); ++y)
      for (int x = mark.x(); x < mark.x() + mark.w(); ++x)
        if (image.at(x, y) == colors[index]) ++ink;
    expect(ink > 0 && ink < 72,
           "each status glyph is sparse token-colored ink, not a solid message background");
  }
  expect((!mark.label() || !*mark.label()) && !mark.visible_focus(),
          "the decorative glyph adds no duplicate accessible text or tab stop");
}

void linux_editor_empty_state_contracts() {
  layerc::test::require_x11();
  editor::install_editor_style();
  PaintWindow window(300, 200, "LayerE empty-state fixture");
  EmptyStateBox empty(20, 20, 260, 160);
  empty.box(FL_FLAT_BOX);
  window.end();
  RestoreSystem restore(window);
  editor::EditorAppearance appearance;
  editor::apply_editor_appearance(appearance, window);
  window.show();
  Fl::check();
  const auto image = capture(window);
  const auto tokens = editor::current_editor_tokens();
  bool title = false, hint = false;
  for (int y = empty.y() + 2; y < empty.y() + empty.h() - 2; ++y)
    for (int x = empty.x() + 2; x < empty.x() + empty.w() - 2; ++x) {
      title = title || image.at(x, y) == tokens.secondary;
      hint = hint || image.at(x, y) == tokens.subtext;
    }
  expect(title && hint,
         "empty lists draw both a secondary title and subtext instruction inside the list");
  expect(std::string(editor::editor_empty_state(editor::EditorMenuContext::profiles).hint).find("New") != std::string::npos,
         "the profiles hint names its visible New action");
  expect(std::string(editor::editor_empty_state(editor::EditorMenuContext::cooldowns).hint).find("Add cooldown") != std::string::npos &&
         std::string(editor::editor_empty_state(editor::EditorMenuContext::bindings).hint).find("Add key") != std::string::npos &&
         std::string(editor::editor_empty_state(editor::EditorMenuContext::panels).hint).find("Add panel") != std::string::npos,
         "every context names its existing add button");
}
