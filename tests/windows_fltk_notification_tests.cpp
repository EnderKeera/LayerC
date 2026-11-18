#include "a11y/fltk_tree.hpp"
#include "a11y/uia/adaptor.hpp"
#include "accessible_widgets.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using layerc::test::expect;
using layerc::editor::a11y::FltkTree;
using layerc::editor::a11y::Role;
using layerc::editor::a11y::uia::Adaptor;
using layerc::editor::accessibility::set_accessible_name;
using layerc::editor::accessibility::set_accessible_role;

template <typename T> class Ref {
public:
  Ref() = default;
  ~Ref() { if (value_) value_->Release(); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  Ref(Ref &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  Ref &operator=(Ref &&other) noexcept {
    if (this != &other) {
      if (value_) value_->Release();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  T *operator->() const { return value_; }
  T *get() const { return value_; }
  T **put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
  explicit operator bool() const { return value_ != nullptr; }
private:
  T *value_{};
};

struct Apartment {
  HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool ok(HRESULT result, const char *message) {
  if (FAILED(result))
    std::cerr << message << ": HRESULT=" << std::hex
              << static_cast<unsigned long>(result) << std::dec << '\n';
  return expect(SUCCEEDED(result), message);
}

// A real UI Automation client on its own apartment, with bounded timeouts so a
// stuck provider fails the case instead of hanging the suite.
struct Client {
  Apartment apartment;
  Ref<IUIAutomation> automation;
  Client() {
    if (!ok(apartment.result, "initialize client MTA")) return;
    if (!ok(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                             IID_IUIAutomation,
                             reinterpret_cast<void **>(automation.put())),
            "create real IUIAutomation client"))
      return;
    Ref<IUIAutomation2> timeouts;
    if (ok(automation->QueryInterface(IID_IUIAutomation2,
                                      reinterpret_cast<void **>(timeouts.put())),
           "UIA timeout interface")) {
      ok(timeouts->put_ConnectionTimeout(2000), "bound connection timeout");
      ok(timeouts->put_TransactionTimeout(2000), "bound transaction timeout");
    }
  }
  explicit operator bool() const { return static_cast<bool>(automation); }
};

// Only this process's announcements count: the desktop-wide subscription also
// relays whatever other applications post.
class Notifications final : public IUIAutomationNotificationEventHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAgileObject) ||
        IsEqualIID(iid, IID_IUIAutomationNotificationEventHandler))
      *result = static_cast<IUIAutomationNotificationEventHandler *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE HandleNotificationEvent(IUIAutomationElement *sender,
      NotificationKind, NotificationProcessing, BSTR text, BSTR) override {
    int process{};
    if (sender == nullptr || FAILED(sender->get_CurrentProcessId(&process)) ||
        static_cast<DWORD>(process) != GetCurrentProcessId())
      return S_OK;
    try {
      const std::lock_guard lock(mutex);
      texts.emplace_back(text, SysStringLen(text));
      return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
  }
  [[nodiscard]] std::size_t count() const {
    const std::lock_guard lock(mutex);
    return texts.size();
  }
  mutable std::mutex mutex;
  std::vector<std::wstring> texts;
private:
  std::atomic<ULONG> references_{1};
};

struct Fixture {
  // Disposable component fixture: no profile store, daemon or production paths.
  Fl_Double_Window window{320, 120, "LayerE UIA announcements"};
  Fl_Box status{20, 44, 280, 24, "Waiting"};
  FltkTree tree;
  std::unique_ptr<Adaptor> adaptor;
  std::atomic<bool> closed{};
  Fixture() {
    window.end();
    set_accessible_role(status, Role::status);
    set_accessible_name(status, "Announcement status");
    window.show();
    Fl::check();
    adaptor = std::make_unique<Adaptor>(tree);
  }
  void close_from_client() {
    Fl::awake([](void *data) {
      auto &self = *static_cast<Fixture *>(data);
      self.window.hide();
      self.closed.store(true);
    }, this);
  }
};

struct AnnounceStep {
  Fixture *fixture{};
  const char *text{};
};

// Announcements belong to the FLTK thread; the client asks for one through the
// loop the way a status callback would.
void announce_ui(void *data) {
  std::unique_ptr<AnnounceStep> step(static_cast<AnnounceStep *>(data));
  step->fixture->adaptor->announce(&step->fixture->status, step->text);
}

// D1: an announcement made while no client is listening must not be replayed,
// stale, to the first client that connects later.
void stale_announcement_is_not_replayed() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize announcement fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fixture fixture;
  // No client has asked for this window, so nobody can hear it: dropping the
  // announcement must not walk the tree either.
  const auto walks = fixture.adaptor->walks_for_testing();
  fixture.adaptor->announce(&fixture.status, "stale");
  expect(fixture.adaptor->walks_for_testing() == walks,
         "an announcement nobody can hear does not walk the tree");
  const HWND hwnd = fl_xid(&fixture.window);
  std::thread worker([&] {
    try {
      Client client;
      if (!client) return;
      // Listen from the desktop root so the handler covers this window's first
      // published generation.
      Ref<IUIAutomationElement> desktop;
      if (!ok(client.automation->GetRootElement(desktop.put()), "desktop root") || !desktop)
        return;
      Ref<Notifications> seen;
      *seen.put() = new Notifications;
      struct Unsubscribe {
        IUIAutomation *client;
        ~Unsubscribe() { client->RemoveAllEventHandlers(); }
      } unsubscribe{client.automation.get()};
      Ref<IUIAutomation5> notifications;
      if (!ok(client.automation->QueryInterface(IID_IUIAutomation5,
                                                reinterpret_cast<void **>(notifications.put())),
              "UIA notifications interface") || !notifications)
        return;
      if (!ok(notifications->AddNotificationEventHandler(desktop.get(), TreeScope_Children, nullptr,
                                                         seen.get()),
              "subscribe status notifications"))
        return;
      // First contact: the provider publishes a generation for this client.
      Ref<IUIAutomationElement> root;
      if (!ok(client.automation->ElementFromHandle(hwnd, root.put()),
              "client finds FLTK HWND") || !root)
        return;
      const auto quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (seen->count() == 0 && std::chrono::steady_clock::now() < quiet)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      expect(seen->count() == 0, "an announcement made with no client is not replayed");
      Fl::awake(announce_ui, new AnnounceStep{&fixture, "fresh"});
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (seen->count() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      { const std::lock_guard lock(seen->mutex);
        std::string detail = "exactly one announcement reaches the client that is listening (saw";
        detail += std::to_string(seen->texts.size());
        for (const auto &text : seen->texts) {
          detail += " '";
          for (const auto unit : text)
            detail += unit < 128 ? static_cast<char>(unit) : '?';
          detail += "'";
        }
        detail += ")";
        expect(seen->texts.size() == 1, detail);
        expect(!seen->texts.empty() && seen->texts.front() == L"fresh",
               "the connected client receives the new announcement, not the dropped one"); }
      fixture.close_from_client();
      const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!fixture.closed.load() && std::chrono::steady_clock::now() < close_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected UIA notification exception"); }
    if (!fixture.closed.load()) fixture.close_from_client();
  });
  Fl::run();
  worker.join();
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(stale_announcement_is_not_replayed))
