#include "binding_capture.hpp"
#include "editor_appearance.hpp"
#include "editor_session.hpp"
#include "editor_style.hpp"
#include "hid_binding_dialog.hpp"
#include "platform/win32/binding_controls.hpp"
#include "profile_mutations.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <windowsx.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace core = layerc::core;
namespace editor = layerc::editor;
namespace platform = layerc::platform;
using layerc::test::expect;

using DialogStep = void (*)(Fl_Window &, HWND);

class ForegroundOverride final {
public:
  explicit ForegroundOverride(const HWND window) noexcept {
    set_window(window);
    editor::foreground_window_query = []() noexcept { return target_; };
  }
  ~ForegroundOverride() {
    set_window(nullptr);
    editor::foreground_window_query = &editor::system_foreground_window;
  }

  ForegroundOverride(const ForegroundOverride &) = delete;
  ForegroundOverride &operator=(const ForegroundOverride &) = delete;

  static void set_window(const HWND window) noexcept { target_ = window; }

private:
  inline static HWND target_{};
};

LPARAM key_data(const unsigned scan, const bool extended = false) {
  return static_cast<LPARAM>(1U | (scan << 16U) |
                             (extended ? 1U << 24U : 0U));
}

Fl_Widget *child(const Fl_Window &window, const std::string_view label) {
  for (int i = 0; i < window.children(); ++i) {
    auto *widget = window.child(i);
    if (widget->label() != nullptr && std::string_view(widget->label()) == label)
      return widget;
  }
  return nullptr;
}

void run_step(void *data) {
  const auto step = *static_cast<const DialogStep *>(data);
  auto *window = Fl::modal();
  if (!expect(window != nullptr && window->shown(), "capture dialog opened as a modal")) {
    if (window != nullptr) window->hide();
    return;
  }
  const HWND hwnd = fl_xid(window);
  if (expect(hwnd != nullptr, "capture dialog owns a native window")) {
    const ForegroundOverride foreground(hwnd);
    ::SetFocus(hwnd);
    if (expect(::GetFocus() == hwnd, "capture fixture owns native focus (foreground is simulated)"))
      step(*window, hwnd);
  }
  if (window->shown()) window->hide();
}

std::optional<core::InputChord> capture(const DialogStep step) {
  auto action = step;
  Fl::add_timeout(0.01, run_step, &action);
  auto result = editor::capture_focused_binding();
  Fl::remove_timeout(run_step, &action);
  return result;
}

void keyboard_and_composition() {
  const auto result = capture([](Fl_Window &window, const HWND hwnd) {
    auto *accept = child(window, "Accept");
    auto *clear = child(window, "Clear");
    if (!expect(accept != nullptr && clear != nullptr, "capture actions exist")) return;
    expect(!accept->active(), "empty capture cannot be accepted");
    // Deliver messages directly to the test-owned window. No system input,
    // keyboard layout changes, global hooks or physical IME are involved.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', key_data(0x1e));
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, key_data(0x01));
    SendMessageW(hwnd, WM_CHAR, 0x3042, 1);
    if (!expect(window.shown(), "composition Escape did not cancel binding capture")) return;
    expect(!accept->active(), "composing key and text cannot create a chord");
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_KEYDOWN, VK_PROCESSKEY, key_data(0x1e));
    SendMessageW(hwnd, WM_KEYDOWN, VK_PACKET, key_data(0x1e));
    SendMessageW(hwnd, WM_CHAR, 0x3042, 1);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', key_data(0));
    expect(!accept->active(), "IME pseudo-keys, standalone text and scan zero add no token");

    SendMessageW(hwnd, WM_KEYDOWN, VK_SPACE, key_data(0x39));
    expect(accept->active() != 0, "physical Space is captured on initial focus");
    if (expect(clear->take_focus() != 0, "Clear can receive FLTK focus")) {
      // FLTK's previous keysym is Space. An independent translated space
      // must not clear the chord by impersonating a key press on Clear.
      SendMessageW(hwnd, WM_CHAR, L' ', key_data(0x39));
      expect(accept->active() != 0, "translated text did not activate Clear");
    }
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_MENU, key_data(0x38, true));
    SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, key_data(0x1c, true));
    SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, key_data(0x1c, true));
    if (accept->active()) accept->do_callback();
  });
  const std::vector<core::InputToken> expected{
      {core::InputDevice::keyboard, 0x0039, 0},
      {core::InputDevice::keyboard, 0xe038, 0},
      {core::InputDevice::keyboard, 0xe01c, 0}};
  expect(result && result->tokens == expected,
         "captures physical scans, keeps the extended bit and de-duplicates repeat");
}

void loss_of_focus_and_cancel() {
  const auto result = capture([](Fl_Window &window, const HWND hwnd) {
    auto *accept = child(window, "Accept");
    if (!expect(accept != nullptr, "capture action exists")) return;
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    ::SetFocus(nullptr);
    if (expect(::GetFocus() == nullptr, "test removed native focus")) {
      SendMessageW(hwnd, WM_KEYDOWN, 'B', key_data(0x30));
      expect(!accept->active(), "no record after native focus loss even with modal shown");
    }
    ::SetFocus(hwnd);
    if (!expect(::GetFocus() == hwnd, "capture regained native focus")) return;
    SendMessageW(hwnd, WM_KEYDOWN, 'C', key_data(0x2e));
    expect(accept->active() != 0, "new focus does not inherit a prior composition");
    SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, key_data(0x01));
    expect(!window.shown(), "physical Escape cancels the focused capture");
  });
  expect(!result, "cancelling never returns the captured keys");
}

void mouse_uses_event_location_and_layerd_codes() {
  const auto result = capture([](Fl_Window &window, const HWND hwnd) {
    auto *accept = child(window, "Accept");
    const auto *pad = child(window, "Mouse capture area: click a button or turn the wheel here");
    if (!expect(accept != nullptr && pad != nullptr, "capture actions and pad exist")) return;

    const auto scale = static_cast<double>(Fl::screen_scale(window.screen_num()));
    const auto x = static_cast<int>((pad->x() + pad->w() / 2) * scale);
    const auto y = static_cast<int>((pad->y() + pad->h() / 2) * scale);
    const auto packed = [](const LONG px, const LONG py) {
      return MAKELPARAM(static_cast<WORD>(px), static_cast<WORD>(py));
    };
    POINT inside{x, y};
    POINT outside{x, 0};
    if (!expect(ClientToScreen(hwnd, &inside) && ClientToScreen(hwnd, &outside),
                "wheel fixture has native screen positions"))
      return;
    const LPARAM point_in_pad = packed(x, y);
    const auto wheel_in = packed(inside.x, inside.y);
    const auto wheel_out = packed(outside.x, outside.y);
    const auto down = MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA));
    const auto up = MAKEWPARAM(0, WHEEL_DELTA);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point_in_pad);
    SendMessageW(hwnd, WM_MOUSEWHEEL, up, wheel_out);
    SendMessageW(hwnd, WM_MOUSEHWHEEL, up, wheel_in);
    expect(!accept->active(), "outside and horizontal wheel motion add no token");
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, packed(x, 0));
    SendMessageW(hwnd, WM_MOUSEWHEEL, up, wheel_in);
    SendMessageW(hwnd, WM_MOUSEWHEEL, down, wheel_in);
    SendMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, point_in_pad);
    SendMessageW(hwnd, WM_RBUTTONUP, 0, point_in_pad);
    SendMessageW(hwnd, WM_MBUTTONDOWN, MK_MBUTTON, point_in_pad);
    SendMessageW(hwnd, WM_MBUTTONUP, 0, point_in_pad);
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), point_in_pad);
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), point_in_pad);
    if (expect(accept->active() != 0, "mouse capture enables Accept")) accept->do_callback();
  });
  const std::vector<core::InputToken> expected{
      {core::InputDevice::mouse, 6, 0}, {core::InputDevice::mouse, 7, 0},
      {core::InputDevice::mouse, 2, 0}, {core::InputDevice::mouse, 3, 0},
      {core::InputDevice::mouse, 5, 0}};
  expect(result && result->tokens == expected,
         "wheel location comes from this event; right, middle and X2 match LayerD");
}

void background_dialog_records_nothing() {
  const auto result = capture([](Fl_Window &window, const HWND hwnd) {
    const auto *accept = child(window, "Accept");
    auto *cancel = child(window, "Cancel");
    const auto *pad = child(window, "Mouse capture area: click a button or turn the wheel here");
    if (!expect(accept && cancel && pad, "capture actions and pad exist")) return;

    ForegroundOverride::set_window(nullptr);
    const auto scale = static_cast<double>(Fl::screen_scale(window.screen_num()));
    const auto x = static_cast<int>((pad->x() + pad->w() / 2.0) * scale);
    const auto y = static_cast<int>((pad->y() + pad->h() / 2.0) * scale);
    const LPARAM point_in_pad = MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
    SendMessageW(hwnd, WM_KEYDOWN, 'A', key_data(0x1e));
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point_in_pad);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, point_in_pad);
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), point_in_pad);
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), point_in_pad);
    expect(!accept->active(), "background capture does not record keyboard or mouse input");
    cancel->do_callback();
  });
  expect(!result, "cancelling background capture returns no chord");
}

class HidFixture final : public editor::HidBindingCapture {
public:
  std::vector<std::string> scan(const bool include_system) override {
    stop();
    included_system = include_system;
    if (include_system) return {"Fixture joystick", "Fixture keyboard"};
    return {"Fixture joystick"};
  }
  std::optional<std::string> start(const std::size_t row, Update next) override {
    if (row != 0) return "The device was disconnected.";
    ++starts;
    update = std::move(next);
    return std::nullopt;
  }
  void stop() noexcept override { update = {}; }
  std::optional<std::string> assign_slot(const core::HidDeviceSelector &selector,
                                        const bool replace) override {
    ++slot_attempts;
    if (!replace) return "This slot belongs to another device.";
    assigned_slot = selector.stable_slot;
    return std::nullopt;
  }

  void baseline() {
    if (update) update({true, std::nullopt, "Idle baseline captured."});
  }
  void end_capture(std::optional<core::HidControlBinding> control) {
    if (auto notify = std::exchange(update, {})) {
      const auto text = control ? "Captured fixture axis." : "The device was disconnected.";
      notify({false, std::move(control), text});
    }
  }

  unsigned starts{};
  unsigned slot_attempts{};
  std::uint16_t assigned_slot{};
  bool included_system{};
  Update update;
};

core::HidControlBinding fixture_axis() {
  core::HidControlBinding binding;
  binding.device.vendor_id = 1;
  binding.device.product_id = 2;
  binding.device.usage_page = 1;
  binding.device.usage = 4;
  binding.device.descriptor_hash.assign(64, 'a');
  binding.usage_page = 1;
  binding.usage = 0x30;
  binding.activation = core::HidActivationMode::axis_positive;
  binding.threshold = 0.55;
  return binding;
}

template <class Action>
bool with_hid_dialog(HidFixture &fixture, const editor::AcceptHidBinding &accept,
                     Action action, const editor::HidBindingAnnouncement &announce = {}) {
  const auto step = [](void *data) {
    auto *window = Fl::modal();
    if (!expect(window != nullptr && window->shown(), "HID dialog is modal")) return;
    (*static_cast<Action *>(data))(*window);
    if (window->shown()) window->hide();
  };
  Fl::add_timeout(0.01, step, &action);
  const bool accepted = editor::capture_hid_binding(fixture, {}, accept, announce);
  Fl::remove_timeout(step, &action);
  return accepted;
}

void hid_abort_does_not_assign() {
  HidFixture fixture;
  unsigned accepted{};
  const auto result = with_hid_dialog(fixture, [&](const auto &, bool) {
    ++accepted;
    return std::nullopt;
  }, [&](Fl_Window &window) {
    auto *include = dynamic_cast<Fl_Check_Button *>(
        child(window, "Include keyboard and mouse devices (advanced)"));
    auto *capture = child(window, "Capture");
    auto *use = child(window, "Use binding");
    auto *cancel = child(window, "Cancel");
    if (!expect(include && capture && use && cancel, "HID capture has explicit actions")) return;
    expect(!include->value() && !fixture.included_system && !use->active(),
           "system classes and acceptance start disabled");
    include->value(1);
    include->do_callback();
    expect(fixture.included_system && !use->active(), "system-class scan is explicit");
    capture->do_callback();
    fixture.baseline();
    expect(fixture.update && !use->active(), "baseline is not a completed binding");
    fixture.end_capture(std::nullopt);
    expect(!fixture.update && !use->active() && capture->active(),
           "disconnect stops listening and permits capture again");
    capture->do_callback();
    expect(fixture.update && fixture.starts == 2, "capture can restart");
    cancel->do_callback();
  });
  expect(!result && !fixture.update && accepted == 0 && fixture.slot_attempts == 0,
         "Cancel tears down an active listener without assigning a slot");
}

void hid_accept_validates_and_records_once() {
  HidFixture fixture;
  layerc::test::TempDirectory directory("layerc-fltk-hid");
  editor::EditorSession session(directory.path());
  session.new_profile();
  if (!expect(!session.edit([](auto &profile) { profile.game.global = true; }),
              "make the disposable draft valid")) return;
  const auto before = session.document().profile();
  const auto generation = session.edit_generation();
  if (!expect(!before.cooldowns.empty(), "draft owns a cooldown")) return;
  const auto id = before.cooldowns.front().id;
  auto invalid = fixture_axis();
  invalid.device.descriptor_hash.clear();
  expect(session.add_hid_binding(id, invalid, [&] {
    ++fixture.slot_attempts;
    return std::nullopt;
  }).has_value() && fixture.slot_attempts == 0 && session.edit_generation() == generation,
      "document validation precedes any machine-local effect");
  std::string announcement;
  const auto result = with_hid_dialog(fixture, [&](const auto &binding, const bool replace) {
    return session.add_hid_binding(id, binding, [&] {
      return fixture.assign_slot(binding.device, replace);
    });
  }, [&](Fl_Window &window) {
    auto *capture = child(window, "Capture");
    auto *use = child(window, "Use binding");
    auto *slot = dynamic_cast<Fl_Input *>(child(window, "Stable slot (1–32)"));
    auto *hysteresis = dynamic_cast<Fl_Input *>(child(window, "Axis hysteresis (%)"));
    auto *replace = dynamic_cast<Fl_Check_Button *>(
        child(window, "Allow replacing another device in this slot"));
    if (!expect(capture && use && slot && hysteresis && replace, "HID fields are labelled")) return;
    capture->do_callback();
    fixture.end_capture(fixture_axis());
    expect(use->active() && hysteresis->active() && !replace->value(),
           "captured axis enables its settings without opting into slot replacement");
    slot->value("33");
    use->do_callback();
    expect(fixture.slot_attempts == 0 && announcement.starts_with("Stable slot"),
           "invalid slot is announced without a write");
    slot->value("3");
    hysteresis->value("46");
    use->do_callback();
    expect(fixture.slot_attempts == 0 && announcement == "Axis hysteresis crosses the threshold.",
           "hysteresis cannot cross the threshold");
    hysteresis->value("45"); // Exact 55 + 45 boundary, including floating-point conversion.
    use->do_callback();
    expect(window.shown() && fixture.slot_attempts == 1 &&
               session.document().profile() == before && session.edit_generation() == generation,
           "occupied slot refuses without editing the profile or history");
    replace->value(1);
    use->do_callback();
  }, [&](Fl_Widget *, const std::string &message) { announcement = message; });
  const auto after = session.document().profile();
  if (!expect(result && fixture.slot_attempts == 2 && fixture.assigned_slot == 3 &&
                 session.edit_generation() == generation + 1 && after.hid_bindings.size() == 1 &&
                 after.bindings.size() == before.bindings.size() + 1,
             "one validated acceptance assigns the machine slot and appends a binding")) return;
  expect(after.bindings.back().chord.tokens ==
             std::vector<core::InputToken>{{core::InputDevice::generic_hid, 1, 0}} &&
             after.hid_bindings[0].device.stable_slot == 3 &&
             !after.hid_bindings[0].allow_system_class &&
             after.bindings.front() == before.bindings.front(),
         "the generic HID token references its descriptor and leaves earlier chords unchanged");
  expect(session.undo() && session.document().profile() == before &&
             session.redo() && session.document().profile() == after && fixture.slot_attempts == 2,
         "descriptor and chord undo/redo as one edit without rewriting the slot");
  expect(!session.edit([](auto &profile) {
           (void)editor::remove_binding(profile, profile.bindings.size() - 1);
         }) && session.document().profile() == before,
         "removal compacts the unreferenced descriptor and retains the other bindings");
}

void xinput_held_and_reconnected_baselines() {
  editor::FocusedControllerEdges edges;
  XINPUT_GAMEPAD gamepad{};
  gamepad.wButtons = XINPUT_GAMEPAD_A;
  expect(edges.sample(2, gamepad) == 0, "initial held button is only a baseline");
  expect(edges.sample(2, gamepad) == 0, "held button is not a repeated press");
  gamepad.wButtons = 0x0400; // Reserved button bit.
  gamepad.bLeftTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD - 1;
  expect(edges.sample(2, gamepad) == 0, "reserved bits and sub-threshold trigger ignored");
  gamepad.wButtons |= XINPUT_GAMEPAD_A;
  gamepad.bLeftTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  gamepad.bRightTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
  const auto a = std::uint32_t{1} << 12U;
  const auto lt = std::uint32_t{1} << 16U;
  const auto rt = std::uint32_t{1} << 17U;
  expect(edges.sample(2, gamepad) == (a | lt | rt), "one press per button or trigger edge");
  expect(platform::xinput_control_token(12, 2) == core::InputToken{core::InputDevice::controller, 13, 2} &&
         platform::xinput_control_token(16, 2) == core::InputToken{core::InputDevice::controller, 17, 2} &&
         platform::xinput_control_token(17, 2) == core::InputToken{core::InputDevice::controller, 18, 2},
         "button and trigger codes preserve controller slot and LayerD encoding");
  expect(edges.sample(2, std::nullopt) == 0, "disconnect drops the baseline");
  expect(edges.sample(2, gamepad) == 0, "reconnect records no held controls");
  expect(edges.sample(1, {}) == 0 && edges.sample(1, gamepad) == 0,
         "another slot starts independently");
  edges.reset();
  expect(edges.sample(2, gamepad) == 0, "refocus records no controls held outside capture");
}

// Optional rendered-evidence fixture: uses the real dialog, but never reads a
// device, opens a profile, or writes a stable slot.
int show_hid_fixture() {
  editor::install_editor_style();
  HidFixture fixture;
  const auto sample = [](void *data) {
    auto &capture = *static_cast<HidFixture *>(data);
    if (capture.update) capture.end_capture(fixture_axis());
  };
  Fl::add_check(sample, &fixture);
  (void)editor::capture_hid_binding(fixture, {}, [](const auto &, bool) {
    return "This slot belongs to another device. Choose a different slot.";
  });
  Fl::remove_check(sample, &fixture);
  return 0;
}

} // namespace

int main(const int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--show-hid-fixture")
    return show_hid_fixture();
  return layerc::test::run({LAYERC_TEST_CASE(keyboard_and_composition),
                          LAYERC_TEST_CASE(loss_of_focus_and_cancel),
                          LAYERC_TEST_CASE(mouse_uses_event_location_and_layerd_codes),
                          LAYERC_TEST_CASE(background_dialog_records_nothing),
                          LAYERC_TEST_CASE(xinput_held_and_reconnected_baselines),
                          LAYERC_TEST_CASE(hid_abort_does_not_assign),
                          LAYERC_TEST_CASE(hid_accept_validates_and_records_once)});
}
