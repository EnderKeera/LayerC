// Native FLTK painting contracts. All windows belong to this disposable test;
// no input events or desktop settings are synthesized or changed.
#include "appearance_window.hpp"
#include "editor_app.hpp"
#include "editor_geometry.hpp"
#include "editor_menus.hpp"
#include "editor_style.hpp"
#include "editor_test_environment.hpp"
#include "layerc/platform/profile_store.hpp"
#include "linux_editor_test_access.hpp"
#include "platform/services.hpp"
#include "profile_edit.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

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

class PreviewEdit final : public editor::OnWindowEdit {
public:
  void begin(std::uint64_t, editor::OnWindowEditBegin) override {}
  void live_profile(std::string) override {}
  void anchor(std::string, double, double, bool) override {}
  void direct_drag(bool) override {}
  void end(bool) override {}
  std::vector<editor::OnWindowEditEvent> take_events() override { return {}; }
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

Pixels capture(Fl_Widget &widget) {
  Fl_Image_Surface surface(widget.w(), widget.h());
  surface.draw(&widget);
  const std::unique_ptr<Fl_RGB_Image> image(surface.image());
  if (!expect(image && image->data_w() == widget.w() && image->data_h() == widget.h() &&
                  image->d() == 3,
              "the fixture paints RGB pixels at one sample per layout pixel")) return {};
  const auto row_bytes = widget.w() * 3;
  const auto stride = image->ld() ? image->ld() : row_bytes;
  Pixels pixels{widget.w(), {}};
  pixels.rgb.reserve(static_cast<std::size_t>(row_bytes) * widget.h());
  for (int y = 0; y < widget.h(); ++y) {
    const auto *row = image->array + y * stride;
    pixels.rgb.insert(pixels.rgb.end(), row, row + row_bytes);
  }
  return pixels;
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
  layerc::test::require_fltk_editor_window();
  editor::install_editor_style();
  PaintWindow window(440, 200, "LayerE paint test fixture");
  editor::EditorField<Fl_Input> field(20, 25, 260, editor::kit::metric(editor::kit::Metric::control_height));
  editor::EditorChoice choice(20, 80, 260, editor::kit::metric(editor::kit::Metric::combo_height));
  choice.add("Copper|Teal");
  choice.value(0);
  Fl_Check_Button check(20, 130, 260, editor::kit::metric(editor::kit::Metric::control_height), "Whole checkbox label");
  Fl_Button apply(310, 25, 110, editor::kit::metric(editor::kit::Metric::control_height), "Apply");
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
  layerc::test::require_fltk_editor_window();
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
  const auto unmarked = capture(window);
  const std::array kinds{editor::StatusKind::success, editor::StatusKind::failure,
                         editor::StatusKind::warning, editor::StatusKind::information};
  const std::array colors{tokens.yes, tokens.no, tokens.warning, tokens.accent_2};
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    mark.kind(kinds[index]);
    const auto image = capture(window);
    int ink = 0;
    for (int y = mark.y(); y < mark.y() + mark.h(); ++y)
      for (int x = mark.x(); x < mark.x() + mark.w(); ++x)
        if (image.at(x, y) == colors[index]) ++ink;
    expect(ink > 0 && ink < 72,
           "each status glyph is sparse token-colored ink, not a solid message background");
  }
  mark.kind(editor::StatusKind::none);
  const auto cleared = capture(window);
  bool no_ink = true;
  for (int y = mark.y(); y < mark.y() + mark.h(); ++y)
    for (int x = mark.x(); x < mark.x() + mark.w(); ++x)
      no_ink = no_ink && cleared.at(x, y) == unmarked.at(x, y);
  expect(mark.kind() == editor::StatusKind::none && no_ink,
         "clearing a status removes every pixel of its previous colored glyph");
  expect((!mark.label() || !*mark.label()) && !mark.visible_focus(),
          "the decorative glyph adds no duplicate accessible text or tab stop");
}

void linux_editor_empty_state_contracts() {
  layerc::test::require_fltk_editor_window();
  editor::install_editor_style();
  layerc::test::EditorTestEnvironment store("layerc-editor-empty-ink-paint", layerc::test::EditorTestScope::widget,
                                            "fixture control namespace is private");
  editor::EditorApp app;
  PaintWindow window(300, 200, "LayerE empty-state fixture");
  editor::ContextBrowser empty(20, 20, 260, 160, nullptr, app, editor::EditorMenuContext::profiles);
  empty.textsize(FL_NORMAL_SIZE);
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
  const auto fill = editor::theme_role_colors(appearance, editor::EditorRole::list)->fill;
  const auto partner = editor::theme_role_colors(appearance, editor::EditorRole::window)->fill;
  expect(image.at(empty.x() + 12, empty.y() + 12) == fill,
         "the real list paints the active Lists fill under its ink");
  empty.deactivate();
  const auto inactive = capture(window);
  expect(inactive.at(empty.x() + 12, empty.y() + 12) == editor::fltk_inactive_rgb(fill, partner),
         "the real inactive list paints the modeled dimmed fill");
  expect(std::string(editor::editor_empty_state(editor::EditorMenuContext::profiles).hint).find("New") != std::string::npos,
         "the profiles hint names its visible New action");
  expect(std::string(editor::editor_empty_state(editor::EditorMenuContext::cooldowns).hint).find("Add cooldown") != std::string::npos &&
         std::string(editor::editor_empty_state(editor::EditorMenuContext::bindings).hint).find("Add key") != std::string::npos &&
         std::string(editor::editor_empty_state(editor::EditorMenuContext::panels).hint).find("Add panel") != std::string::npos,
         "every context names its existing add button");
}

void linux_editor_inactive_rgb_matches_fltk() {
  layerc::test::require_fltk_editor_window();
  Fl_Double_Window window(220, 100, "LayerE inactive blend fixture");
  window.end();
  RestoreSystem restore(window);
  editor::EditorAppearance appearance;
  const auto index = static_cast<std::size_t>(editor::EditorRole::window);
  const auto color = [](std::uint32_t rgb) {
    return fl_rgb_color(static_cast<uchar>(rgb >> 16U),
                        static_cast<uchar>(rgb >> 8U), static_cast<uchar>(rgb));
  };
  for (const auto fill : {0x171717U, 0xFF0000U, 0x0C34EFU}) {
    appearance.overrides[index].fill = fill == 0x0C34EFU ? 0x001EFFU : 0x008C30U;
    editor::apply_editor_appearance(appearance, window);
    const auto partner = fill == 0x0C34EFU ? 0x011EFEU : 0x018C30U;
    expect(editor::fltk_inactive_rgb(fill, partner) == (Fl::get_color(fl_inactive(color(fill))) >> 8U),
           "the modeled inactive fill equals FLTK's RGB, including a clamped Window partner");
  }
}

void linux_editor_preview_caption_uses_preview_ink() {
  layerc::test::require_fltk_editor_window();
  layerc::test::EditorTestEnvironment store("layerc-editor-preview-ink-paint", layerc::test::EditorTestScope::widget,
                                            "fixture control namespace is private");
  auto profile = editor::make_new_profile();
  profile.game.executable_name = "preview-fixture.exe";
  constexpr auto profile_ink = 0x2860B8U;
  auto &bar = profile.panels.front().bar;
  bar.background_color = bar.fill_color = bar.border_color = profile_ink;
  if (!expect(layerc::platform::write_profile_file(layerc::platform::profile_directory() / "fixture.json",
                  layerc::core::serialize_profile_json(profile)).ok(), "write a private preview profile")) return;

  constexpr auto preview_role = static_cast<std::size_t>(editor::EditorRole::preview);
  constexpr auto labels = static_cast<std::size_t>(editor::EditorRole::label);
  constexpr auto preview_ink = 0xF8F4D0U, next_ink = 0xD0F4F8U, label_ink = 0xF0A0C0U;
  editor::EditorAppearance appearance;
  appearance.overrides[preview_role].text = preview_ink;
  appearance.overrides[labels].text = label_ink;
  editor::EditorApp app;
  app.build(editor::AppearanceLoad{appearance, {}});
  using Access = editor::EditorAppTestAccess;
  auto &window = Access::window(app);
  auto &preview = *Access::preview(app);
  RestoreSystem restore(window);
  window.show();
  Fl::check();
  expect((Fl::get_color(preview.labelcolor()) >> 8U) == preview_ink,
         "startup sets the Preview label color before Appearance is ever opened");
  Access::set_on_window_edit(app, std::make_unique<PreviewEdit>());
  editor::TaskCandidate target;
  target.window = 1;
  target.title = "Preview fixture";
  target.executable_name = profile.game.executable_name;
  target.width = preview.w() - 10;
  target.height = preview.h() - 10;
  Access::position_on(app, target);
  if (!expect(app.placement_active(), "the private matching target starts positioning")) return;
  Fl::focus(nullptr);
  const auto before = capture(preview);
  appearance.overrides[preview_role].text = next_ink;
  editor::apply_editor_appearance(appearance, window, Access::profiles(app), &preview);
  const auto after = capture(preview);
  if (before.rgb.empty() || after.rgb.empty()) return;
  const auto frame = editor::placement_frame(5, 5, preview.w() - 10, preview.h() - 10,
                                             target.width, target.height);
  bool caption = false, new_caption = false, labels_in_caption = false;
  for (int y = frame.y + 1; y < frame.y + 17; ++y)
    for (int x = frame.x + 1; x < frame.x + frame.width - 1; ++x) {
      caption = caption || before.at(x, y) == preview_ink;
      new_caption = new_caption || after.at(x, y) == next_ink;
      labels_in_caption = labels_in_caption || before.at(x, y) == label_ink || after.at(x, y) == label_ink;
    }
  expect(caption && new_caption && !labels_in_caption,
         "the positioning caption paints Preview text (including live changes), not Labels text");

  bool panel = false, unchanged = true;
  for (int y = frame.y + 20; y < preview.h(); ++y)
    for (int x = 0; x < preview.w(); ++x) {
      panel = panel || before.at(x, y) == profile_ink;
      unchanged = unchanged && before.at(x, y) == after.at(x, y);
    }
  expect(panel && unchanged, "changing Preview text leaves the whole profile-painted area unchanged");
}
