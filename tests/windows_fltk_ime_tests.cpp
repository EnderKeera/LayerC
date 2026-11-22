#include "editor_app.hpp"
#include "editor_test_environment.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "linux_editor_test_access.hpp"
#include "options_window.hpp"
#include "profile_edit.hpp"
#include "windows_fltk_uia_text_client.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/platform.H>
#include <imm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

namespace {
namespace editor = layerc::editor;
namespace uia = layerc::test::uia_text;
using Access = editor::EditorAppTestAccess;
using layerc::test::expect;

class InputContext {
public:
  explicit InputContext(HWND window) : window_(window), context_(ImmGetContext(window)) {}
  ~InputContext() { if (context_) ImmReleaseContext(window_, context_); }
  InputContext(const InputContext &) = delete;
  InputContext &operator=(const InputContext &) = delete;
  HIMC get() const { return context_; }
private:
  HWND window_;
  HIMC context_;
};

struct Fixture {
  layerc::test::EditorTestEnvironment environment{"layerc-ime"};
  editor::EditorApp app;
  bool show() {
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    const auto path = layerc::platform::profile_directory() / "fixture.json";
    if (!expect(layerc::platform::write_profile_file(path, layerc::core::serialize_profile_json(profile)).ok(),
                "seed the private editor profile")) return false;
    app.session.rescan();
    app.build();
    Access::window(app).show();
    Fl::check();
    return expect(app.session.has_document(), "the shown editor opened the fixture");
  }
  ~Fixture() { if (Access::window(app).shown()) Access::window(app).hide(); }
};

bool pump_until(const auto &predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) Fl::wait(.01);
  return predicate();
}

bool focus(Fl_Input &field) {
  if (!expect(field.visible_r() && field.active_r(), "the field is visible and editable")) return false;
  const HWND hwnd = fl_xid(field.top_window());
  SetFocus(hwnd);
  field.take_focus();
  Fl::check();
  return expect(GetFocus() == hwnd && Fl::focus() == &field,
                "keyboard focus stays in the test's own FLTK field");
}

void input_contexts_on_main_and_dialog() {
  Fixture fixture;
  if (!fixture.show()) return;
  const InputContext main(fl_xid(&Access::window(fixture.app)));
  expect(main.get() != nullptr, "the editor main window has an IMM context");
  editor::OptionsWindow dialog({}, {}, 800, {}, {}, {});
  dialog.show();
  Fl::check();
  {
    const InputContext options(fl_xid(&dialog));
    expect(options.get() != nullptr, "the real Options dialog has an IMM context");
  }
  dialog.hide();
}

template <typename Widget>
Widget *find_visible(Fl_Group &group, std::string_view label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (!child->visible_r()) continue;
    if (auto *widget = dynamic_cast<Widget *>(child);
        widget && widget->label() && label == widget->label())
      return widget;
    if (auto *nested = child->as_group())
      if (auto *found = find_visible<Widget>(*nested, label)) return found;
  }
  return nullptr;
}

void verify_candidate_placement(Fl_Input &field, int end, bool multiline) {
  if (!focus(field)) return;
  auto *window = field.top_window();
  const InputContext context(fl_xid(window));
  if (!expect(context.get() != nullptr, "candidate placement has an input context")) return;
  const double scale = Fl::screen_scale(window->screen_num());
  // FLTK subtracts the top-level label height; include it plus two pixels of rounding.
  const int tolerance = static_cast<int>(std::ceil(window->labelsize() * scale)) + 2;
  const auto position = [&](int index) {
    field.insert_position(index);
    window->redraw();
    Fl::flush();
    COMPOSITIONFORM form{};
    expect(ImmGetCompositionWindow(context.get(), &form) && form.dwStyle == CFS_POINT,
           "focused input puts the IME candidate point in top-level client coordinates");
    expect(form.ptCurrentPos.x >= field.x() * scale - 2 &&
               form.ptCurrentPos.x <= (field.x() + field.w()) * scale + 2 &&
               form.ptCurrentPos.y >= field.y() * scale - tolerance &&
               form.ptCurrentPos.y <= (field.y() + field.h()) * scale + 2,
           "candidate point lies within the field, allowing FLTK's label offset");
    return form.ptCurrentPos;
  };
  const auto first = position(0);
  const auto last = position(end);
  std::cout << (multiline ? "Multiline" : "Single-line") << " candidate point: ("
            << first.x << ", " << first.y << ") -> (" << last.x << ", " << last.y
            << "), label-offset tolerance " << tolerance << " px\n";
  expect(multiline ? last.y > first.y : last.x > first.x && last.y == first.y,
         "candidate point follows the moved caret");
}

void candidate_window_follows_both_carets() {
  Fixture fixture;
  if (!fixture.show()) return;
  auto *single = Access::name(fixture.app);
  single->value("Candidate position");
  verify_candidate_placement(*single, single->size(), false);
  fixture.app.command(editor::EditorCommand::automation_page);
  auto &window = Access::window(fixture.app);
  auto *create = find_visible<Fl_Button>(window, "New automation");
  if (!expect(create != nullptr, "the Automation page offers New automation")) return;
  create->do_callback();
  auto *view = find_visible<Fl_Choice>(window, "Automation view");
  if (!expect(view != nullptr, "the Automation page offers the view selector")) return;
  view->value(2);
  view->do_callback();
  auto *multi = find_visible<Fl_Multiline_Input>(window, "Advanced automation JSON");
  if (!expect(multi != nullptr, "the editor shows the real multiline JSON input")) return;
  multi->value("First\nSecond\nThird");
  verify_candidate_placement(*multi, multi->size(), true);
}

void committed_text_reaches_field_model_and_uia() {
  uia::Apartment sta(COINIT_APARTMENTTHREADED);
  if (!uia::ok(sta.result, "initialize editor COM STA")) return;
  Fixture fixture;
  if (!fixture.show()) return;
  auto &window = Access::window(fixture.app);
  auto *field = Access::name(fixture.app);
  if (!focus(*field)) return;
  field->value("");
  field->insert_position(0);
  const HWND hwnd = fl_xid(&window);
  constexpr std::array input{L'\u65e5', L'\u672c', L'\u8a9e', L'\u4e2d', L'\u6587',
                             L'\ud55c', L'\uad6d', L'\uc5b4', wchar_t{0xd83d}, wchar_t{0xde00}};
  constexpr std::string_view utf8 = "日本語中文한국어😀";
  for (const auto unit : input) {
    if (!expect(PostMessageW(hwnd, WM_IME_CHAR, unit, 1) != FALSE,
                "post a UTF-16 unit only to the test's own window")) return;
  }
  if (!expect(pump_until([&] { return std::string_view(field->value()) == utf8; }),
              "IME delivers Japanese, Chinese, Korean and the surrogate pair as exact UTF-8")) return;
  expect(fixture.app.session.document().profile().name == utf8 &&
             std::string_view(window.label()).ends_with(std::string(utf8) + " *"),
         "native text callbacks update the profile model and dirty title");
  if (!expect(fixture.app.sync_accessibility(), "publish the editor's native UIA provider")) return;
  if (!expect(Fl::lock() == 0, "initialize FLTK for the UIA client thread")) return;
  std::atomic<bool> done{};
  std::jthread client([&] {
    uia::Client api;
    const auto root = api.root(hwnd);
    if (expect(static_cast<bool>(root), "UIA exposes the editor window")) {
      const auto name = uia::find(*api.automation, *root, L"Profile name");
      if (expect(static_cast<bool>(name), "UIA exposes the profile name")) {
        const auto pattern = uia::text_of(*name);
        if (expect(static_cast<bool>(pattern), "the profile name exposes the UIA Text pattern")) {
          const auto range = uia::document_of(*pattern);
          if (expect(static_cast<bool>(range), "UIA Text exposes its document range"))
            expect(uia::range_text(*range) == std::wstring(input.begin(), input.end()),
                   "the real UIA Text range preserves the exact UTF-16 units");
        }
      }
    }
    done.store(true);
  });
  while (!done.load()) Fl::wait(.01);
  client.join();
  Fl::unlock();
}

bool cjk_ime(HKL layout) {
  const WORD language = PRIMARYLANGID(LOWORD(reinterpret_cast<ULONG_PTR>(layout)));
  return (language == LANG_JAPANESE || language == LANG_CHINESE || language == LANG_KOREAN) &&
         ImmIsIME(layout) != FALSE;
}

void installed_system_ime_result() {
  std::vector<HKL> layouts(static_cast<std::size_t>(GetKeyboardLayoutList(0, nullptr)));
  if (!layouts.empty()) {
    const auto copied = GetKeyboardLayoutList(static_cast<int>(layouts.size()), layouts.data());
    layouts.resize(static_cast<std::size_t>(copied));
  }
  const auto selected = std::ranges::find_if(layouts, cjk_ime);
  if (selected == layouts.end()) {
    std::cout << "System IME not observed: GetKeyboardLayoutList reported no installed Japanese, Chinese or Korean IMM layout.\n";
    return;
  }
  Fixture fixture;
  if (!fixture.show()) return;
  auto *field = Access::name(fixture.app);
  if (!focus(*field)) return;
  const auto previous = ActivateKeyboardLayout(*selected, 0);
  if (!expect(previous != nullptr, "activate the existing IME on this test thread only")) return;
  struct RestoreLayout {
    HKL previous;
    ~RestoreLayout() { ActivateKeyboardLayout(previous, 0); }
  } restore_layout{previous};
  const InputContext context(fl_xid(&Access::window(fixture.app)));
  if (!expect(context.get() != nullptr, "the installed IME exposes this test window's context")) return;
  struct RestoreOpen {
    HIMC context;
    BOOL was_open;
    ~RestoreOpen() { ImmSetOpenStatus(context, was_open); }
  } restore_open{context.get(), ImmGetOpenStatus(context.get())};
  if (!expect(ImmSetOpenStatus(context.get(), TRUE) != FALSE, "open the test's IME context")) return;
  field->value("");
  field->insert_position(0);
  wchar_t composition[] = L"\u5b57";
  if (!expect(ImmSetCompositionStringW(context.get(), SCS_SETSTR, composition,
                                        sizeof(wchar_t), nullptr, 0) != FALSE,
              "set the installed system IME's composition")) return;
  if (!expect(ImmNotifyIME(context.get(), NI_COMPOSITIONSTR, CPS_COMPLETE, 0) != FALSE,
              "complete the installed system IME's composition")) return;
  expect(pump_until([&] { return std::string_view(field->value()) == "字"; }),
         "installed system IME commit reaches the focused FLTK input");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(input_contexts_on_main_and_dialog),
                 LAYERC_TEST_CASE(candidate_window_follows_both_carets),
                 LAYERC_TEST_CASE(committed_text_reaches_field_model_and_uia),
                 LAYERC_TEST_CASE(installed_system_ime_result))
