// The Windows on-window edit session: a thin client of the editor's one
// DaemonHub, which owns the live pipe. The hub's worker posts to a
// message-only window; its procedure defers to an FLTK timeout so `ready`
// always runs on the UI loop, outside FLTK's own event dispatch.
#include "platform/on_window_edit.hpp"
#include "platform/win32/daemon_hub.hpp"

#include <FL/Fl.H>

#include <utility>

#include <windows.h>

namespace layerc::editor {
namespace {

constexpr wchar_t notify_class[] = L"LayerC.LayerE.FLTK.OnWindowEdit";
constexpr UINT events_message = WM_APP + 1;
thread_local std::function<bool()> release_for_test;

class HubOnWindowEdit final : public OnWindowEdit {
public:
  HubOnWindowEdit(DaemonHub &hub, std::function<void()> ready)
      : hub_(hub), ready_(std::move(ready)), instance_(GetModuleHandleW(nullptr)) {
    WNDCLASSW definition{};
    definition.lpfnWndProc = procedure;
    definition.hInstance = instance_;
    definition.lpszClassName = notify_class;
    // A second instance reuses the class an earlier one registered.
    window_class_ = RegisterClassW(&definition) != 0;
    window_ = CreateWindowExW(0, notify_class, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
                              nullptr, instance_, this);
    if (window_ == nullptr) return;
    const HWND window = window_;
    hub_.set_edit_listener([window] { (void)PostMessageW(window, events_message, 0, 0); });
  }
  ~HubOnWindowEdit() override {
    hub_.set_edit_listener({});
    end(true);
    Fl::remove_timeout(deliver, this);
    if (window_ != nullptr) DestroyWindow(window_);
    if (window_class_) (void)UnregisterClassW(notify_class, instance_);
  }
  HubOnWindowEdit(const HubOnWindowEdit &) = delete;
  HubOnWindowEdit &operator=(const HubOnWindowEdit &) = delete;

  [[nodiscard]] bool usable() const noexcept { return window_ != nullptr; }

  void begin(const std::uint64_t window, OnWindowEditBegin payload) override {
    session_ = hub_.begin_edit(window, std::move(payload));
  }
  void live_profile(std::string profile_json) override {
    hub_.edit_live_profile(session_, std::move(profile_json));
  }
  void anchor(std::string panel_id, const double x, const double y,
              const bool final) override {
    hub_.edit_anchor(session_, std::move(panel_id), x, y, final);
  }
  void direct_drag(const bool enabled) override { hub_.edit_direct_drag(session_, enabled); }
  void end(const bool notify) override {
    if (session_ == 0) return;
    hub_.end_edit(session_, notify);
    session_ = 0;
  }
  [[nodiscard]] std::vector<OnWindowEditEvent> take_events() override {
    auto events = hub_.take_edit_events(session_);
    for (const auto &event : events)
      if (event.kind == OnWindowEditEvent::Kind::ended ||
          event.kind == OnWindowEditEvent::Kind::expired)
        session_ = 0;
    return events;
  }

private:
  static LRESULT CALLBACK procedure(const HWND window, const UINT message,
                                    const WPARAM wparam, const LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (message == events_message) {
      auto *self =
          reinterpret_cast<HubOnWindowEdit *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      if (self != nullptr && !self->pending_) {
        self->pending_ = true;
        Fl::add_timeout(0.0, deliver, self);
      }
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  static void deliver(void *data) {
    auto &self = *static_cast<HubOnWindowEdit *>(data);
    self.pending_ = false;
    if (self.ready_) self.ready_();
  }

  DaemonHub &hub_;
  std::function<void()> ready_;
  HINSTANCE instance_{};
  HWND window_{};
  bool window_class_{};
  bool pending_{};
  std::uint64_t session_{};
};

} // namespace

bool on_window_edit_sessions() noexcept { return true; }

bool on_window_direct_drag_released() {
  if (release_for_test) return release_for_test();
  const auto *hub = product_daemon_hub_if_created();
  return !hub || hub->direct_drag_released();
}

void set_on_window_release_for_test(std::function<bool()> released) {
  release_for_test = std::move(released);
}

std::unique_ptr<OnWindowEdit> make_on_window_edit(std::function<void()> ready) {
  auto edit = std::make_unique<HubOnWindowEdit>(product_daemon_hub(), std::move(ready));
  if (!edit->usable()) return nullptr;
  return edit;
}

} // namespace layerc::editor
