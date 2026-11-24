#include "accessible_widgets.hpp"
#include "editor_app.hpp"
#include "editor_file_chooser.hpp"
#include "editor_style.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/window_query.hpp"
#include "linux_editor_test_access.hpp"
#include "platform/editor_options.hpp"
#include "platform/win32/window_chrome.hpp"
#include "platform/win32/window_settings.hpp"
#include "platform/window_chrome.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/platform.H>

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>
#include <vector>

namespace {
namespace editor = layerc::editor;
namespace platform = layerc::platform;
using layerc::test::expect;
using enum editor::FilePromptStage;
using Mode = editor::EditorFileRequest::Mode;

constexpr auto refusal =
    "Could not hide this window from screen capture, so typing a path is off. Open the file dialog anyway, or cancel.";
constexpr auto drive_error =
    "Could not use that path: typed paths start with a drive letter, like C:\\. Use Open file dialog... for other locations.";
constexpr auto file_error =
    "Could not use that path: no file is there. Type the full path to an existing file.";
constexpr auto title = "Export base JSON only";
constexpr auto atom_name = L"LayerC.Editor.CaptureLifetime";

struct PromptSeams {
  ~PromptSeams() {
    editor::set_native_file_presenter_for_test({});
    editor::set_file_prompt_observer_for_test({});
  }
};

struct Probe {
  enum class Operation { set, clear, read, frame };
  enum class Result { allow, deny, unchanged };
  struct Call { HWND window; Operation operation; };
  static inline std::vector<Call> calls;
  static inline HWND main{};
  static inline Result result{};

  static bool set(const HWND window, const bool enabled) {
    calls.push_back({window, enabled ? Operation::set : Operation::clear});
    if (enabled && window != main && result != Result::allow) return result == Result::unchanged;
    return platform::apply_capture_exclusion(window, enabled);
  }
  static BOOL WINAPI read(const HWND window, DWORD *affinity) {
    calls.push_back({window, Operation::read});
    return GetWindowDisplayAffinity(window, affinity);
  }
  static void frame(HWND window, COLORREF caption, COLORREF border, COLORREF ink) {
    calls.push_back({window, Operation::frame});
    platform::set_window_frame_colors(window, caption, border, ink);
  }
  static std::vector<Operation> for_window(const HWND window) {
    std::vector<Operation> operations;
    for (const auto &call : calls)
      if (call.window == window) operations.push_back(call.operation);
    return operations;
  }
};

bool affinity(HWND window, DWORD expected) {
  DWORD actual{};
  return GetWindowDisplayAffinity(window, &actual) && actual == expected;
}

std::string_view text(const char *value) { return value ? value : ""; }

std::string utf8(const std::filesystem::path &path) {
  return platform::path_to_utf8(path).value_or("");
}

bool private_text_absent(const Fl_Group &group, std::string_view token) {
  if (text(group.label()).find(token) != text(group.label()).npos ||
      text(group.tooltip()).find(token) != text(group.tooltip()).npos ||
      editor::accessibility::accessible_description(group).find(token) != std::string::npos)
    return false;
  for (int i = 0; i < group.children(); ++i) {
    const auto &widget = *group.child(i);
    if (const auto *nested = dynamic_cast<const Fl_Group *>(&widget)) {
      if (!private_text_absent(*nested, token)) return false;
    } else if (text(widget.label()).find(token) != text(widget.label()).npos ||
               text(widget.tooltip()).find(token) != text(widget.tooltip()).npos ||
               editor::accessibility::accessible_name(widget).value_or("").find(token) != std::string::npos ||
               editor::accessibility::accessible_description(widget).find(token) != std::string::npos) {
      return false;
    }
  }
  return true;
}

editor::EditorFileRequest request(editor::EditorApp &app, Mode mode) {
  return {app, mode, title, mode == Mode::save ? "Export" : "Import", "JSON", {"json"},
          editor::EditorFileRequest::DirectoryMemory::profiles};
}

INT_PTR CALLBACK hidden_dialog_proc(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

class HiddenNativeDialog {
public:
  explicit HiddenNativeDialog(HWND owner) {
    struct alignas(DWORD) DialogTemplate {
      DLGTEMPLATE header;
      WORD menu, window_class, title_string;
    } data{};
    data.header.style = WS_POPUP | WS_CAPTION;
    data.header.cx = 120;
    data.header.cy = 80;
    window = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), &data.header, owner,
                                        hidden_dialog_proc, 0);
  }
  ~HiddenNativeDialog() { if (window) DestroyWindow(window); }
  HWND window{};
};

void file_prompt_verifies_before_paint_and_dispatch(editor::EditorApp &app) {
  const PromptSeams reset;
  editor::WindowsWindowDecorator decorator(Probe::set, Probe::read);
  Probe::main = fl_xid(app.main_window());
  Probe::result = Probe::Result::allow;
  editor::set_editor_capture_exclusion(true);
  Probe::calls.clear();
  int observed{}, native{};
  editor::set_native_file_presenter_for_test([&](const auto &, const auto &) {
    ++native; return std::nullopt;
  });
  editor::set_file_prompt_observer_for_test([&](const auto &view, const auto stage) {
    ++observed;
    const HWND window = fl_xid(&view.window);
    using enum Probe::Operation;
    const auto calls = Probe::for_window(window);
    constexpr std::array sequence{set, read};
    expect(stage == shown_and_verified && std::ranges::search(calls, sequence).begin() != calls.end() &&
               affinity(window, platform::capture_exclusion_affinity) && view.draw_count == 0,
           "the owned prompt has set and matching readback before the first draw or event dispatch");
    expect(GetWindow(window, GW_OWNER) == Probe::main && text(view.window.label()) == title,
           "the prompt is owned by the main editor and uses only the request title");
    expect(view.input.active() && view.primary.active() && Fl::focus() == &view.input &&
               view.input.size() == 0 && text(view.reason.label()).empty() &&
               private_text_absent(view.window, utf8(platform::profile_directory())),
           "the verified prompt starts empty and focused, with no private path in the tree");
    expect(editor::is_primary_button(view.primary) && !editor::is_primary_button(view.alternative) &&
               view.primary.x() < view.alternative.x() && view.alternative.x() < view.cancel.x() &&
               editor::accessibility::accessible_description(view.input) == text(view.message.label()),
           "only the request action is primary, and the warning describes the input");
    view.cancel.do_callback();
  });
  expect(!editor::choose_editor_file(request(app, Mode::save)) && observed == 1 && native == 0,
         "Cancel returns no path and never calls the presenter");
}

void file_prompt_refuses_unverified_typing(editor::EditorApp &app) {
  const PromptSeams reset;
  editor::WindowsWindowDecorator decorator(Probe::set, Probe::read);
  Probe::main = fl_xid(app.main_window());
  decorator.set_warning_reporter([&](std::string_view warning) { app.set_capture_exclusion_warning(warning); });
  editor::set_editor_capture_exclusion(true);
  const auto *status = editor::EditorAppTestAccess::status(app);
  if (!expect(status, "main has its warning status")) return;
  for (const auto result : {Probe::Result::deny, Probe::Result::unchanged}) {
    Probe::result = result;
    Probe::calls.clear();
    int shown{}, native{};
    HWND prompt{};
    const auto destination = platform::profile_directory() / "not-opened.json";
    editor::set_native_file_presenter_for_test([&](const auto &, const auto &) {
      ++native;
      expect(prompt && !IsWindow(prompt), "consent hides the prompt before the native presenter runs");
      return std::optional{destination};
    });
    editor::set_file_prompt_observer_for_test([&](const auto &view, const auto stage) {
      ++shown;
      prompt = fl_xid(&view.window);
      expect(stage == shown_and_verified && view.draw_count == 0 && native == 0 &&
                 affinity(Probe::main, platform::capture_exclusion_affinity) && affinity(prompt, WDA_NONE),
             "refusal is limited to the prompt and never calls the presenter implicitly");
      expect(!view.input.active() && !view.primary.active() && view.reason.visible_r() &&
                 text(view.reason.label()) == refusal && Fl::focus() == &view.cancel,
             "refused affinity disables the input and primary and focuses Cancel");
      expect(editor::accessibility::accessible_description(view.input) == refusal &&
                 editor::accessibility::accessible_description(view.primary) == refusal &&
                 text(view.primary.tooltip()) == refusal,
             "the refusal describes the disabled input and primary and is the primary tooltip");
      expect(std::string_view(status->value()).starts_with(editor::capture_exclusion_incomplete),
             "a prompt refusal is visible on the verified main window");
      view.alternative.do_callback();
    });
    expect(editor::choose_editor_file(request(app, Mode::save)) == destination && shown == 1 && native == 1,
           "only Open file dialog explicitly consents once, even after a wrong affinity readback");
  }
  Probe::result = Probe::Result::allow;
  decorator.refresh();
}

void file_prompt_typed_paths_and_inline_replacement(editor::EditorApp &app) {
  const PromptSeams reset;
  const layerc::test::TempDirectory directory("LayerC-PrivatePathFixture");
  const auto root = directory.path();
  const auto non_ascii = platform::path_from_utf8("private-\xC3\xA9.JSON");
  if (!expect(non_ascii.has_value(), "decode a non-ASCII fixture name")) return;
  const auto existing = root / *non_ascii;
  const auto folder = root / "folder";
  std::ofstream(existing, std::ios::binary) << "{}\n";
  std::filesystem::create_directory(folder);
  editor::WindowsWindowDecorator decorator;
  editor::set_editor_capture_exclusion(true);
  int native{};
  std::string native_start;
  editor::set_native_file_presenter_for_test([&](const auto &, const std::string &start) {
    ++native; native_start = start; return std::nullopt;
  });

  const auto refuse = [&](const auto mode, const std::string &input, std::string_view expected) {
    int errors{};
    editor::set_file_prompt_observer_for_test([&](const auto &view, const auto stage) {
      if (stage == shown_and_verified) {
        view.input.value(input.c_str());
        view.primary.do_callback();
        if (view.window.shown()) view.cancel.do_callback();
      } else if (stage == error) {
        ++errors;
        expect(view.reason.visible_r() && text(view.reason.label()).starts_with(expected) &&
                   (input.empty() || text(view.reason.label()).find(input) == std::string_view::npos) &&
                   private_text_absent(view.window, utf8(root)),
               "the inline error explains how to fix the path without repeating it");
      }
    });
    expect(!editor::choose_editor_file(request(app, mode)) && errors == 1 && native == 0,
           "a refused typed path stays inline and cannot reach a presenter");
  };
  constexpr std::string_view invalid_name =
      "Could not use that path: Windows does not allow that file name.";
  refuse(Mode::open, "", drive_error);
  refuse(Mode::open, "relative.json", drive_error);
  refuse(Mode::open, R"(\\not-a-fixture.invalid\share\private.json)", drive_error);
  refuse(Mode::open, R"(\\?\)" + utf8(existing), drive_error);
  refuse(Mode::open, R"(\\.\)" + utf8(existing), drive_error);
  refuse(Mode::open, utf8(root / "missing.json"), file_error);
  refuse(Mode::open, utf8(folder), file_error);
  refuse(Mode::open, utf8(root / "private.txt"), "Could not use that path: the file extension is not supported.");
  refuse(Mode::save, utf8(root / "NUL.json"), invalid_name);
  refuse(Mode::save, utf8(root / "LPT9.txt.json"), invalid_name);
  refuse(Mode::save, utf8(root / "private.json:stream"), invalid_name);
  refuse(Mode::save, utf8(root / "private.json."), invalid_name);
  refuse(Mode::save, "\"" + utf8(root / "private.json ") + "\"", invalid_name);
  refuse(Mode::save, utf8(root / "directory. " / "private.json"), invalid_name);
  refuse(Mode::save, utf8(root / "missing" / "private.json"), "Could not use that path: the parent folder");
  refuse(Mode::save, utf8(folder), "Could not use that path: the destination is not an available file.");

  const auto accept = [&](const auto mode, const std::string &input) {
    editor::set_file_prompt_observer_for_test([&](const auto &view, const auto stage) {
      if (stage != shown_and_verified) return;
      view.input.value(input.c_str());
      view.primary.do_callback();
      if (view.window.shown()) view.cancel.do_callback();
    });
    return editor::choose_editor_file(request(app, mode));
  };
  expect(accept(Mode::open, "  \"" + utf8(existing) + "\" \t") == existing,
         "quoted UTF-8 paths open and extension comparison ignores ASCII case");
  expect(accept(Mode::save, utf8(root / "a-new-file")) == root / "a-new-file.json",
         "save completes a missing extension without writing the file");
  for (const bool replace : {false, true}) {
    HWND prompt{};
    int confirmations{};
    editor::set_file_prompt_observer_for_test([&](const auto &view, const auto stage) {
      if (stage == shown_and_verified) {
        prompt = fl_xid(&view.window);
        view.input.value(utf8(existing).c_str());
        view.primary.do_callback();
        if (view.window.shown()) view.cancel.do_callback();
      } else if (stage == confirm_replace) {
        ++confirmations;
        expect(fl_xid(&view.window) == prompt && text(view.input.value()) == utf8(existing) &&
                   text(view.reason.label()) == "That file already exists. Replace it?" &&
                   text(view.primary.label()) == "&Replace" && text(view.alternative.label()) == "&Back" &&
                   editor::is_primary_button(view.primary) && private_text_absent(view.window, utf8(root)),
               "replace is inline under the same verified HWND, with no path text outside the input");
        if (replace) view.primary.do_callback();
        else {
          view.alternative.do_callback();
          expect(view.window.shown() && view.input.active() && Fl::focus() == &view.input &&
                     text(view.input.value()) == utf8(existing) && text(view.reason.label()).empty() &&
                     text(view.alternative.label()) == "Open file &dialog...",
                 "Back restores the path, focus and edit actions without closing");
        }
      }
    });
    const auto selected = editor::choose_editor_file(request(app, Mode::save));
    expect(confirmations == 1 && selected == (replace ? std::optional{existing} : std::nullopt),
           "only Replace returns the existing path");
  }
  editor::set_editor_capture_exclusion(false);
  expect(!editor::choose_editor_file(request(app, Mode::open)) && native == 1 && native_start == utf8(root),
         "a successful typed selection updates the native start directory");
  expect(layerc::test::read_file(existing) == "{}\n" && !std::filesystem::exists(root / "a-new-file.json"),
         "chooser validation and replacement consent do not write files");
}

void capture_verdict_is_scoped_to_window_lifetime(Fl_Window &root) {
  const HWND window = fl_xid(&root);
  HiddenNativeDialog unrelated(window);
  if (!expect(unrelated.window && !IsWindowVisible(unrelated.window) &&
                  GetWindow(unrelated.window, GW_OWNER) == window &&
                  GetWindowThreadProcessId(unrelated.window, nullptr) == GetCurrentThreadId(),
              "created a hidden same-thread owned native common-dialog-class HWND")) return;
  expect(!editor::verify_editor_window_capture(root), "verification fails closed without a decorator");
  ATOM atom{};
  {
    Probe::result = Probe::Result::allow;
    editor::WindowsWindowDecorator decorator(Probe::set, Probe::read, Probe::frame);
    editor::set_editor_capture_exclusion(true);
    atom = GlobalFindAtomW(atom_name);
    const auto property = MAKEINTATOM(atom);
    const auto predecessor = GetPropW(window, property);
    if (!expect(atom && predecessor && affinity(window, platform::capture_exclusion_affinity),
                "the live FLTK root has a verified lifetime property")) return;
    decorator.adopt_for_test(unrelated.window, true, false);
    expect(decorator.capture_warning() == editor::capture_exclusion_incomplete,
           "the simulated predecessor has a failed verdict");
    Probe::calls.clear();
    RemovePropW(window, property);
    expect(editor::verify_editor_window_capture(root) && GetPropW(window, property) &&
               GetPropW(window, property) != predecessor && decorator.capture_warning().empty() &&
               decorator.decorated_window_count() == 1 && Probe::for_window(unrelated.window).empty(),
           "the check prunes stale live HWNDs without touching non-FLTK roots and re-enrolls the FLTK root");
    expect(std::ranges::count(Probe::for_window(window), Probe::Operation::frame) == 1,
           "fresh lifetime enrollment reapplies the caption");
    decorator.adopt_for_test(unrelated.window, true, false);
    Probe::calls.clear();
  }
  expect(!GetPropW(window, MAKEINTATOM(atom)) && affinity(window, WDA_NONE) &&
             Probe::for_window(window) == std::vector{Probe::Operation::clear} &&
             Probe::for_window(unrelated.window).empty() && affinity(unrelated.window, WDA_NONE) &&
             editor::editor_capture_exclusion_warning().empty(),
         "destruction removes only the current lifetime property and affinity and leaves the stale native HWND alone");
}
} // namespace

void file_prompt_and_capture_lifetime_contracts() {
  editor::EditorApp app;
  app.build();
  app.show();
  file_prompt_verifies_before_paint_and_dispatch(app);
  file_prompt_refuses_unverified_typing(app);
  file_prompt_typed_paths_and_inline_replacement(app);
  capture_verdict_is_scoped_to_window_lifetime(*app.main_window());
}
