#include "a11y/fltk_hooks.hpp"
#include "a11y/fltk_tree.hpp"
#include "a11y/uia/adaptor.hpp"
#include "accessible_widgets.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Radio_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Tabs.H>
#include <FL/platform.H>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr HRESULT kInvalidOperation = static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
// MinGW does not declare the UI Automation NoScroll sentinel.
constexpr double kNoScroll = -1.0;
#ifdef UIA_ScrollPatternNoScroll
static_assert(kNoScroll == UIA_ScrollPatternNoScroll);
#endif
using layerc::test::expect;
using layerc::editor::a11y::FltkTree;
using layerc::editor::a11y::PopupMenus;
using layerc::editor::a11y::set_popup_menus;
using layerc::editor::a11y::uia::Adaptor;
using layerc::editor::accessibility::set_accessible_name;

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

// A real UI Automation client with bounded timeouts, so a stuck provider fails
// the case instead of hanging the suite.
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

// Property-changed and automation events arrive on UIA's own thread; the client
// thread only counts them under the lock.
class EventLog final : public IUIAutomationPropertyChangedEventHandler,
                       public IUIAutomationEventHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAgileObject) ||
        IsEqualIID(iid, IID_IUIAutomationPropertyChangedEventHandler))
      *result = static_cast<IUIAutomationPropertyChangedEventHandler *>(this);
    else if (IsEqualIID(iid, IID_IUIAutomationEventHandler))
      *result = static_cast<IUIAutomationEventHandler *>(this);
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
  HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement *sender,
                                                       PROPERTYID id, VARIANT) override {
    int process{};
    if (sender == nullptr || FAILED(sender->get_CurrentProcessId(&process)) ||
        static_cast<DWORD>(process) != GetCurrentProcessId())
      return S_OK;
    const std::lock_guard lock(mutex);
    properties.push_back(id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *sender,
                                                  EVENTID id) override {
    int process{};
    if (sender == nullptr || FAILED(sender->get_CurrentProcessId(&process)) ||
        static_cast<DWORD>(process) != GetCurrentProcessId())
      return S_OK;
    const std::lock_guard lock(mutex);
    events.push_back(id);
    return S_OK;
  }
  [[nodiscard]] std::size_t property_count(PROPERTYID id) const {
    const std::lock_guard lock(mutex);
    std::size_t count = 0;
    for (const auto seen : properties) count += seen == id ? 1u : 0u;
    return count;
  }
  [[nodiscard]] std::size_t event_count(EVENTID id) const {
    const std::lock_guard lock(mutex);
    std::size_t count = 0;
    for (const auto seen : events) count += seen == id ? 1u : 0u;
    return count;
  }
  mutable std::mutex mutex;
  std::vector<PROPERTYID> properties;
  std::vector<EVENTID> events;
private:
  std::atomic<ULONG> references_{1};
};

// The frontend's own popup menus are replaced by this recorder, so the test can
// watch an expansion without opening a window or running a modal loop.
struct FakePopups {
  static inline std::atomic<Fl_Menu_ *> open_owner{};
  static inline std::atomic<int> open_title{-1};
  static inline std::atomic<int> opens{};
  static inline std::atomic<int> dismisses{};
  static inline std::atomic<int> picks{};

  static Fl_Menu_ *owner() { return open_owner.load(); }
  static bool current(const Fl_Menu_ &, int) { return false; }
  static std::optional<layerc::editor::EditorBounds> item_bounds(const Fl_Menu_ &, int) {
    return std::nullopt;
  }
  static Fl_Window *document_window(const Fl_Window *) { return nullptr; }
  static bool pick(Fl_Menu_ &owner_menu, int index) {
    if (open_owner.load() != &owner_menu) return false;
    ++picks;
    (void)index;
    return true;
  }
  static void open(Fl_Menu_ &owner_menu, int title) {
    open_owner.store(&owner_menu);
    open_title.store(title);
    ++opens;
  }
  static bool dismiss(Fl_Menu_ &owner_menu) {
    if (open_owner.load() != &owner_menu) return false;
    open_owner.store(nullptr);
    open_title.store(-1);
    ++dismisses;
    return true;
  }
  static int title(const Fl_Menu_ &owner_menu) {
    return open_owner.load() == &owner_menu ? open_title.load() : -1;
  }
  static void install() {
    set_popup_menus(PopupMenus{&owner, &current, &item_bounds, &document_window, &pick, &open,
                               &dismiss, &title});
  }
  static void reset() {
    set_popup_menus(PopupMenus{});
    open_owner.store(nullptr);
    open_title.store(-1);
    opens.store(0);
    dismisses.store(0);
    picks.store(0);
  }
};

// Every FLTK group's constructor makes it the current group, so a group member
// is closed right after its last child; otherwise later members nest inside it.
struct EndGroup {
  explicit EndGroup(Fl_Group &group) { group.end(); }
};

// Every pattern case shares this fixture: each widget publishes the one pattern
// the case exercises, and nothing else in the window claims it.
// Reads a browser's protected content height through a base-class member
// pointer; no object is cast to a helper type.
struct ListHeight : Fl_Browser_ {
  static constexpr int (Fl_Browser_::*height)() const = &ListHeight::full_height;
  static constexpr int (Fl_Browser_::*width)() const = &ListHeight::full_width;
};

struct Fixture {
  Fl_Double_Window window{720, 470, "LayerE UIA patterns"};
  Fl_Check_Button check{20, 20, 240, 24, "Track cooldowns"};
  Fl_Button pin{300, 20, 140, 24, "Pin overlay"};
  Fl_Slider opacity{20, 60, 260, 24, "Opacity"};
  Fl_Hold_Browser rows{20, 100, 220, 80, "Rows"};
  Fl_Tabs tabs{300, 100, 220, 80, "Sections"};
  Fl_Group general{300, 120, 220, 60, "General"};
  EndGroup general_end{general};
  Fl_Group advanced{300, 120, 220, 60, "Advanced"};
  EndGroup advanced_end{advanced};
  EndGroup tabs_end{tabs};
  Fl_Group radio_group{20, 185, 260, 50, "Measurement system"};
  Fl_Radio_Button metric{20, 187, 120, 22, "Metric"};
  Fl_Radio_Button imperial{150, 187, 120, 22, "Imperial"};
  Fl_Check_Button radio_neighbor{20, 210, 220, 22, "Show measurements"};
  EndGroup radio_group_end{radio_group};
  Fl_Choice units{300, 200, 160, 24, "Units"};
  Fl_Scroll pane{20, 240, 320, 110, "Scrollable pane"};
  EndGroup pane_end{pane};
  Fl_Scroll wide{20, 360, 320, 90, "Wide pane"};
  EndGroup wide_end{wide};
  Fl_Hold_Browser long_rows{380, 300, 90, 80, "Long rows"};
  Fl_Box label{380, 240, 200, 24, "Plain label"};
  EndGroup window_end{window};
  FltkTree tree;
  std::unique_ptr<Adaptor> adaptor;
  std::atomic<bool> closed{};

  Fixture() {
    window.begin();
    pin.type(FL_TOGGLE_BUTTON);
    opacity.bounds(0, 100);
    opacity.step(5);
    opacity.value(20);
    set_accessible_name(opacity, "Opacity");
    rows.add("Alpha");
    rows.add("Beta");
    rows.add("Gamma");
    rows.value(1);
    set_accessible_name(rows, "Rows");
    tabs.value(&general);
    set_accessible_name(tabs, "Sections");
    metric.value(1);
    radio_neighbor.value(1);
    units.add("&Millimetres");
    units.add("&Inches");
    units.value(0);
    set_accessible_name(units, "Units");
    pane.begin();
    new Fl_Box(20, 240, 300, 400);
    pane.end();
    set_accessible_name(pane, "Scrollable pane");
    // Fits the pane until the vertical scrollbar takes its width away.
    wide.begin();
    new Fl_Box(20, 360, wide.w() - Fl::box_dw(wide.box()) - 5, 300);
    wide.end();
    set_accessible_name(wide, "Wide pane");
    for (int row = 1; row <= 200; ++row)
      long_rows.add(("row " + std::to_string(row) + " alpha beta gamma").c_str());
    set_accessible_name(long_rows, "Long rows");
    window.end();
    set_accessible_name(check, "Track cooldowns");
    set_accessible_name(pin, "Pin overlay");
    window.show();
    Fl::check();
    adaptor = std::make_unique<Adaptor>(tree);
  }
  ~Fixture() { FakePopups::reset(); }
  // Runs fn on the FLTK thread: a client thread never touches widgets directly.
  template <class Fn> void on_fltk(Fn fn) {
    struct Task {
      Fn fn;
      std::atomic<bool> done{};
    };
    auto *task = new Task{std::move(fn)};
    Fl::awake(
        [](void *data) {
          auto *task = static_cast<Task *>(data);
          task->fn();
          task->done.store(true);
        },
        task);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!task->done.load() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (task->done.load()) delete task;
  }
  void close_from_client() {
    Fl::awake([](void *data) {
      auto &self = *static_cast<Fixture *>(data);
      self.window.hide();
      self.closed.store(true);
    }, this);
  }
};

// Closes the fixture window however a client thread leaves, so a failed early
// check ends the case's FLTK loop instead of hanging it.
struct CloseOnExit {
  Fixture &fixture;
  ~CloseOnExit() {
    if (!fixture.closed.load()) fixture.close_from_client();
  }
};

Ref<IUIAutomationElement> find(IUIAutomation &automation, IUIAutomationElement &root,
                               const wchar_t *name) {
  VARIANT value{};
  value.vt = VT_BSTR;
  value.bstrVal = SysAllocString(name);
  Ref<IUIAutomationCondition> condition;
  const auto result = automation.CreatePropertyCondition(UIA_NamePropertyId, value,
                                                          condition.put());
  VariantClear(&value);
  Ref<IUIAutomationElement> element;
  if (ok(result, "build name condition") && condition)
    ok(root.FindFirst(TreeScope_Descendants, condition.get(), element.put()),
       "find real UIA descendant");
  expect(static_cast<bool>(element), "fixture element is in the client tree");
  return element;
}

template <typename Pattern>
bool pattern(IUIAutomationElement &element, PATTERNID id, REFIID iid, Ref<Pattern> &out) {
  return ok(element.GetCurrentPatternAs(id, iid, reinterpret_cast<void **>(out.put())),
            "read a published pattern") && out;
}

template <typename Pattern>
bool has_pattern(IUIAutomationElement &element, PATTERNID id, REFIID iid) {
  Ref<Pattern> probe;
  const bool found = SUCCEEDED(element.GetCurrentPatternAs(id, iid,
                                                           reinterpret_cast<void **>(probe.put()))) &&
                     probe;
  return found;
}

// Bounded polling: every action lands from FLTK's loop, so the test waits for the
// published value it asked for instead of sleeping a fixed period.
template <typename Done> bool wait(Done done, int milliseconds = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

std::string text_of(const std::wstring &wide) {
  std::string text;
  for (const auto unit : wide) text += unit < 128 ? static_cast<char>(unit) : '?';
  return text;
}

std::string name_of(IUIAutomationElement &element) {
  BSTR name{};
  if (FAILED(element.get_CurrentName(&name)) || name == nullptr) return {};
  std::wstring wide(name, SysStringLen(name));
  SysFreeString(name);
  return text_of(wide);
}

// Subscribes for the property and event ids the pattern cases watch, and drops
// every handler on the way out.
struct Subscription {
  IUIAutomation *automation{};
  ::EventLog *log{};
  ~Subscription() { if (automation != nullptr) automation->RemoveAllEventHandlers(); }
  bool add(IUIAutomationElement &root, const PROPERTYID *properties, int count) {
    SAFEARRAY *ids = SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(count));
    if (ids == nullptr) return false;
    for (LONG index = 0; index < count; ++index) {
      LONG value = static_cast<LONG>(properties[index]);
      if (FAILED(SafeArrayPutElement(ids, &index, &value))) {
        SafeArrayDestroy(ids);
        return false;
      }
    }
    const auto subscribed = automation->AddPropertyChangedEventHandler(
        &root, TreeScope_Descendants, nullptr, log, ids);
    SafeArrayDestroy(ids);
    if (!ok(subscribed, "subscribe property changes")) return false;
    return ok(automation->AddAutomationEventHandler(UIA_SelectionItem_ElementSelectedEventId,
                                                    &root, TreeScope_Descendants, nullptr, log),
              "subscribe selection events");
  }
};

// The adaptor's own clock republishes at least every refresh interval, so a
// change the actions do not notify still reaches the client through the next
// walk; these waits are bounded by that interval plus the action's own path.
void toggle_and_range_patterns() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize pattern fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fixture fixture;
  const HWND hwnd = fl_xid(&fixture.window);
  std::thread worker([&] {
    const CloseOnExit close_on_exit{fixture};
    try {
      Client client;
      if (!client) return;
      Ref<IUIAutomationElement> root;
      if (!ok(client.automation->ElementFromHandle(hwnd, root.put()), "client finds FLTK HWND") ||
          !root)
        return;
      Ref<EventLog> log;
      *log.put() = new ::EventLog;
      Subscription subscription{client.automation.get(), log.get()};
      const PROPERTYID watched[] = {UIA_ToggleToggleStatePropertyId,
                                    UIA_RangeValueValuePropertyId,
                                    UIA_ExpandCollapseExpandCollapseStatePropertyId,
                                    UIA_ScrollVerticalScrollPercentPropertyId};
      if (!subscription.add(*root.get(), watched, 4)) return;

      auto check = find(*client.automation.get(), *root.get(), L"Track cooldowns");
      auto slider = find(*client.automation.get(), *root.get(), L"Opacity");
      auto plain = find(*client.automation.get(), *root.get(), L"Pin overlay");
      if (!check || !slider || !plain) return;

      Ref<IUIAutomationTogglePattern> toggle;
      if (!pattern(*check.get(), UIA_TogglePatternId, IID_IUIAutomationTogglePattern, toggle))
        return;
      ToggleState state = ToggleState_Off;
      if (ok(toggle->get_CurrentToggleState(&state), "read toggle state"))
        expect(state == ToggleState_Off, "an unchecked check box starts Off");
      expect(toggle->Toggle() == S_OK, "toggle the check box through its own callback");
      const bool on = wait([&] {
        ToggleState now = ToggleState_Off;
        return SUCCEEDED(toggle->get_CurrentToggleState(&now)) && now == ToggleState_On;
      });
      expect(on, "the check box publishes ToggleState_On after the action");
      expect(wait([&] { return log->property_count(UIA_ToggleToggleStatePropertyId) > 0; }),
             "a toggle state change raises a property-changed event");
      expect(toggle->Toggle() == S_OK, "toggle the check box back");
      expect(wait([&] {
               ToggleState now = ToggleState_On;
               return SUCCEEDED(toggle->get_CurrentToggleState(&now)) &&
                      now == ToggleState_Off;
             }),
             "the check box publishes ToggleState_Off again");

      // A plain button and a label are not checkable, so nothing offers Toggle.
      expect(!has_pattern<IToggleProvider>(*plain.get(), UIA_TogglePatternId,
                                           IID_IToggleProvider),
             "a momentary button does not publish Toggle");

      Ref<IUIAutomationRangeValuePattern> range;
      if (!pattern(*slider.get(), UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern,
                   range))
        return;
      double minimum{};
      double maximum{};
      double value{};
      ok(range->get_CurrentMinimum(&minimum), "read range minimum");
      ok(range->get_CurrentMaximum(&maximum), "read range maximum");
      ok(range->get_CurrentValue(&value), "read range value");
      expect(minimum == 0.0 && maximum == 100.0, "the slider publishes its own bounds");
      expect(value == 20.0, "the slider publishes its current value");
      BOOL readonly{TRUE};
      ok(range->get_CurrentIsReadOnly(&readonly), "read range read-only state");
      expect(readonly == VARIANT_FALSE, "an editable slider is not read-only");
      expect(range->SetValue(50.0) == S_OK, "set the slider value through RangeValue");
      expect(wait([&] {
               double now = 0.0;
               return SUCCEEDED(range->get_CurrentValue(&now)) && now == 50.0;
             }),
             "the slider publishes the value RangeValue set");
      expect(wait([&] { return log->property_count(UIA_RangeValueValuePropertyId) > 0; }),
             "a range value change raises a property-changed event");
      expect(range->SetValue(500.0) == E_INVALIDARG, "an out-of-range value is rejected");
      expect(range->SetValue(-1.0) == E_INVALIDARG, "a negative value is rejected");
      expect(!has_pattern<IUIAutomationRangeValuePattern>(*check.get(), UIA_RangeValuePatternId,
                                                          IID_IUIAutomationRangeValuePattern),
             "a check box does not publish RangeValue");

      fixture.close_from_client();
      expect(wait([&] { return fixture.closed.load(); }), "the fixture window closes");
    } catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected UIA pattern exception"); }
  });
  Fl::run();
  worker.join();
}

void selection_and_selection_item() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize selection fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fixture fixture;
  const HWND hwnd = fl_xid(&fixture.window);
  std::thread worker([&] {
    const CloseOnExit close_on_exit{fixture};
    try {
      Client client;
      if (!client) return;
      Ref<IUIAutomationElement> root;
      if (!ok(client.automation->ElementFromHandle(hwnd, root.put()), "client finds FLTK HWND") ||
          !root)
        return;
      Ref<EventLog> log;
      *log.put() = new ::EventLog;
      Subscription subscription{client.automation.get(), log.get()};
      const PROPERTYID watched[] = {UIA_SelectionItemIsSelectedPropertyId,
                                    UIA_ToggleToggleStatePropertyId};
      if (!subscription.add(*root.get(), watched, 2)) return;

      auto list = find(*client.automation.get(), *root.get(), L"Rows");
      auto beta = find(*client.automation.get(), *root.get(), L"Beta");
      auto alpha = find(*client.automation.get(), *root.get(), L"Alpha");
      auto gamma = find(*client.automation.get(), *root.get(), L"Gamma");
      if (!list || !beta || !alpha || !gamma) return;
      for (IUIAutomationElement *each : {alpha.get(), beta.get(), gamma.get()}) {
        Ref<IUIAutomationSelectionItemPattern> probe;
        ok(pattern(*each, UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern, probe),
           "every row publishes SelectionItem");
      }

      Ref<IUIAutomationSelectionPattern> selection;
      if (!pattern(*list.get(), UIA_SelectionPatternId, IID_IUIAutomationSelectionPattern,
                   selection))
        return;
      BOOL multiple{TRUE};
      BOOL required{TRUE};
      ok(selection->get_CurrentCanSelectMultiple(&multiple), "read multiple selection");
      ok(selection->get_CurrentIsSelectionRequired(&required), "read required selection");
      expect(multiple == VARIANT_FALSE, "a single-choice list says so");
      expect(required == VARIANT_FALSE, "a list without a pinned choice says so");

      Ref<IUIAutomationSelectionItemPattern> item;
      if (!pattern(*beta.get(), UIA_SelectionItemPatternId,
                   IID_IUIAutomationSelectionItemPattern, item))
        return;
      expect(item->Select() == S_OK, "select the row through SelectionItem");
      expect(wait([&] {
               BOOL now{};
               return SUCCEEDED(item->get_CurrentIsSelected(&now)) && now != FALSE;
             }),
             "the selected row publishes IsSelected");
      expect(wait([&] {
               return log->event_count(UIA_SelectionItem_ElementSelectedEventId) > 0;
             }),
             "a selection change raises ElementSelected");
      Ref<IUIAutomationElementArray> selection_array;
      if (ok(selection->GetCurrentSelection(selection_array.put()), "read the list selection") &&
          selection_array) {
        int length{};
        ok(selection_array->get_Length(&length), "read the selection length");
        expect(length == 1, "a single-choice list publishes one selected row");
        Ref<IUIAutomationElement> selected;
        if (ok(selection_array->GetElement(0, selected.put()), "read the selected row") &&
            selected)
          expect(name_of(*selected.get()) == "Beta",
                 "the selection holds the row that was selected");
      }
      expect(!has_pattern<IUIAutomationSelectionPattern>(*beta.get(), UIA_SelectionPatternId,
                                                         IID_IUIAutomationSelectionPattern),
             "a row is not itself a selection container");

      // The list keeps one choice: adding to a non-empty selection and removing
      // the selected item are not expressible. The UIA runtime focuses the
      // element before it calls a selection-item method, and focus selects a row
      // in a single-selection browser, so every row this test reaches is already
      // selected here; the no-op removal is asserted on a tab below instead.
      Ref<IUIAutomationSelectionItemPattern> other;
      if (pattern(*alpha.get(), UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern,
                  other))
        expect(other->AddToSelection() == kInvalidOperation,
               "adding to a single-choice selection is refused");
      expect(item->RemoveFromSelection() == kInvalidOperation,
             "removing the selected item is refused");
      Ref<IUIAutomationElement> container;
      if (ok(item->get_CurrentSelectionContainer(container.put()), "read selection container") &&
          container)
        expect(name_of(*container.get()) == "Rows",
               "the row publishes its list as its selection container");

      // Tabs: selecting a page moves the tab list's choice.
      auto sections = find(*client.automation.get(), *root.get(), L"Sections");
      expect(sections.get() != nullptr, "the client sees the tab list");
      auto general = find(*client.automation.get(), *root.get(), L"General");
      expect(general.get() != nullptr, "the client sees the first page");
      auto advanced = find(*client.automation.get(), *root.get(), L"Advanced");
      expect(advanced.get() != nullptr, "the client sees the second page");
      if (!sections || !general || !advanced) return;
      Ref<IUIAutomationSelectionItemPattern> page;
      expect(pattern(*advanced.get(), UIA_SelectionItemPatternId,
                     IID_IUIAutomationSelectionItemPattern, page),
             "a page publishes the selection item pattern");
      Ref<IUIAutomationSelectionItemPattern> first;
      expect(pattern(*general.get(), UIA_SelectionItemPatternId,
                     IID_IUIAutomationSelectionItemPattern, first),
             "an unselected page publishes the selection item pattern");
      Ref<IUIAutomationSelectionPattern> pages;
      expect(pattern(*sections.get(), UIA_SelectionPatternId, IID_IUIAutomationSelectionPattern,
                     pages),
             "the tab list publishes the selection pattern");
      if (!page || !first || !pages) return;
      BOOL selected{TRUE};
      ok(page->get_CurrentIsSelected(&selected), "read the page selection");
      expect(selected == VARIANT_FALSE, "the second page starts unselected");
      expect(page->Select() == S_OK, "select the second page");
      expect(wait([&] {
               BOOL now{};
               return SUCCEEDED(page->get_CurrentIsSelected(&now)) && now != FALSE;
             }),
             "the selected page publishes IsSelected");
      // Focus does not choose a page, so the first page is genuinely unselected:
      // removing it is a no-op that leaves the tabs unchanged.
      BOOL before{FALSE};
      ok(first->get_CurrentIsSelected(&before), "read the unselected page");
      expect(before == VARIANT_FALSE, "the first page starts unselected");
      expect(first->RemoveFromSelection() == S_OK,
             "removing an unselected item succeeds without effect");
      BOOL after{TRUE};
      ok(first->get_CurrentIsSelected(&after), "re-read the unselected page");
      expect(after == VARIANT_FALSE, "the removal leaves the page unselected");
      Ref<IUIAutomationElementArray> chosen;
      if (ok(pages->GetCurrentSelection(chosen.put()), "read the tab selection") && chosen) {
        int length{};
        ok(chosen->get_Length(&length), "read the tab selection length");
        Ref<IUIAutomationElement> kept;
        if (length == 1 && ok(chosen->GetElement(0, kept.put()), "read the kept page") && kept)
          expect(name_of(*kept.get()) == "Advanced",
                 "the removal leaves the chosen page selected");
        else
          expect(false, "the tab selection holds one page");
      }

      // Radios: the chosen button is the group's selection. UI Automation's
      // RadioButton control type never offers Toggle, so a radio's checked state
      // is published only as IsSelected.
      auto imperial = find(*client.automation.get(), *root.get(), L"Imperial");
      auto metric = find(*client.automation.get(), *root.get(), L"Metric");
      if (!imperial || !metric) return;
      Ref<IUIAutomationSelectionItemPattern> radio;
      expect(pattern(*imperial.get(), UIA_SelectionItemPatternId,
                     IID_IUIAutomationSelectionItemPattern, radio),
             "a radio button publishes the selection item pattern");
      expect(!has_pattern<IUIAutomationTogglePattern>(*imperial.get(), UIA_TogglePatternId,
                                                      IID_IUIAutomationTogglePattern),
             "a radio button does not publish the toggle pattern");
      Ref<IUIAutomationSelectionItemPattern> other_radio;
      expect(pattern(*metric.get(), UIA_SelectionItemPatternId,
                     IID_IUIAutomationSelectionItemPattern, other_radio),
             "the other radio button publishes the selection item pattern");
      if (!radio || !other_radio) return;
      Ref<IUIAutomationElement> mode;
      if (!ok(radio->get_CurrentSelectionContainer(mode.put()), "get the radio container") ||
          !expect(static_cast<bool>(mode), "the radio names a selection container"))
        return;
      expect(name_of(*mode.get()) == "Measurement system",
             "the radio container is its named group");
      Ref<IUIAutomationSelectionPattern> radios;
      if (!pattern(*mode.get(), UIA_SelectionPatternId, IID_IUIAutomationSelectionPattern, radios))
        return;
      const auto chosen_radio_is = [&](const std::string &name) {
        Ref<IUIAutomationElementArray> items;
        int count{};
        Ref<IUIAutomationElement> element;
        return SUCCEEDED(radios->GetCurrentSelection(items.put())) && items &&
               SUCCEEDED(items->get_Length(&count)) && count == 1 &&
               SUCCEEDED(items->GetElement(0, element.put())) && element &&
               name_of(*element.get()) == name;
      };
      expect(chosen_radio_is("Metric"),
             "the checked radio, not the checked checkbox, is the initial selection");

      // Listen only to Imperial so earlier row or tab events cannot satisfy this assertion.
      Ref<EventLog> radio_log;
      *radio_log.put() = new ::EventLog;
      constexpr auto selected_event = UIA_SelectionItem_ElementSelectedEventId;
      if (!ok(client.automation->AddAutomationEventHandler(
                  selected_event, imperial.get(), TreeScope_Element, nullptr, radio_log.get()),
              "subscribe to Imperial's selection event"))
        return;
      expect(radio->Select() == S_OK, "choose the radio button");
      expect(wait([&] {
               BOOL now{};
               return SUCCEEDED(radio->get_CurrentIsSelected(&now)) && now != FALSE;
             }),
              "the chosen radio reports IsSelected");
      expect(wait([&] { return chosen_radio_is("Imperial"); }),
             "the same group now returns the newly checked radio");
      expect(wait([&] { return radio_log->event_count(selected_event) > 0; }),
             "UIA delivers ElementSelected for the chosen radio");
      // A chosen radio is checked rather than selected; it still fills the group.
      expect(other_radio->AddToSelection() == kInvalidOperation,
             "a radio cannot join a group whose choice is already made");

      fixture.close_from_client();
      expect(wait([&] { return fixture.closed.load(); }), "the fixture window closes");
    } catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected UIA pattern exception"); }
  });
  Fl::run();
  worker.join();
}

void expand_collapse_and_scroll() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize expansion fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fixture fixture;
  FakePopups::install();
  const HWND hwnd = fl_xid(&fixture.window);
  std::thread worker([&] {
    const CloseOnExit close_on_exit{fixture};
    try {
      Client client;
      if (!client) return;
      Ref<IUIAutomationElement> root;
      if (!ok(client.automation->ElementFromHandle(hwnd, root.put()), "client finds FLTK HWND") ||
          !root)
        return;
      Ref<EventLog> log;
      *log.put() = new ::EventLog;
      Subscription subscription{client.automation.get(), log.get()};
      const PROPERTYID watched[] = {UIA_ExpandCollapseExpandCollapseStatePropertyId,
                                    UIA_ScrollVerticalScrollPercentPropertyId,
                                    UIA_ValueValuePropertyId};
      if (!subscription.add(*root.get(), watched, 3)) return;

      auto choice = find(*client.automation.get(), *root.get(), L"Units");
      auto pane = find(*client.automation.get(), *root.get(), L"Scrollable pane");
      auto button = find(*client.automation.get(), *root.get(), L"Pin overlay");
      if (!choice || !pane || !button) return;

      Ref<IUIAutomationValuePattern> choice_value;
      if (!pattern(*choice.get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern, choice_value))
        return;
      const auto value_is = [&](const std::wstring &expected) {
        BSTR value{};
        const auto read = choice_value->get_CurrentValue(&value);
        const bool matches = SUCCEEDED(read) && value &&
                             std::wstring_view(value, SysStringLen(value)) == expected;
        SysFreeString(value);
        return matches;
      };
      expect(value_is(L"Millimetres"), "the choice publishes its displayed label");
      BOOL readonly{FALSE};
      ok(choice_value->get_CurrentIsReadOnly(&readonly), "read whether the choice value is editable");
      expect(readonly != FALSE, "the non-editable combo publishes a read-only value");
      if (BSTR proposed = SysAllocString(L"Inches")) {
        const HRESULT set = choice_value->SetValue(proposed);
        SysFreeString(proposed);
        expect(FAILED(set), "the combo refuses a text setter");
      } else {
        expect(false, "allocate the proposed combo value");
      }
      expect(value_is(L"Millimetres"), "a failed text setter leaves the selection alone");
      fixture.on_fltk([&] { fixture.units.value(1); });
      expect(wait([&] { return value_is(L"Inches"); }),
             "the published value follows the widget's committed selection");
      expect(wait([&] { return log->property_count(UIA_ValueValuePropertyId) > 0; }),
             "the changed label raises a UIA value event");

      Ref<IUIAutomationExpandCollapsePattern> expand;
      if (!pattern(*choice.get(), UIA_ExpandCollapsePatternId,
                   IID_IUIAutomationExpandCollapsePattern, expand))
        return;
      ExpandCollapseState state = ExpandCollapseState_Expanded;
      ok(expand->get_CurrentExpandCollapseState(&state), "read the expansion state");
      expect(state == ExpandCollapseState_Collapsed, "a closed drop-down is collapsed");
      expect(expand->Expand() == S_OK, "expand the drop-down through the frontend popup");
      expect(wait([&] {
               ExpandCollapseState now = ExpandCollapseState_Collapsed;
               return SUCCEEDED(expand->get_CurrentExpandCollapseState(&now)) &&
                      now == ExpandCollapseState_Expanded;
             }),
             "the open drop-down publishes Expanded");
      expect(FakePopups::opens.load() == 1, "the frontend's own popup was opened");
      expect(FakePopups::picks.load() == 0, "opening the popup chooses nothing");
      expect(wait([&] {
               return log->property_count(UIA_ExpandCollapseExpandCollapseStatePropertyId) > 0;
             }),
             "an expansion change raises a property-changed event");
      expect(expand->Expand() == kInvalidOperation, "expanding an open drop-down is refused");
      expect(expand->Collapse() == S_OK, "collapse the drop-down");
      expect(FakePopups::dismisses.load() == 1, "the popup closed without choosing an item");
      expect(wait([&] {
               ExpandCollapseState now = ExpandCollapseState_Expanded;
               return SUCCEEDED(expand->get_CurrentExpandCollapseState(&now)) &&
                      now == ExpandCollapseState_Collapsed;
             }),
             "the closed drop-down publishes Collapsed");
      expect(expand->Collapse() == kInvalidOperation,
             "collapsing a closed drop-down is refused");
      expect(!has_pattern<IUIAutomationExpandCollapsePattern>(
                 *button.get(), UIA_ExpandCollapsePatternId,
                 IID_IUIAutomationExpandCollapsePattern),
             "a plain button does not publish ExpandCollapse");

      Ref<IUIAutomationScrollPattern> scroll;
      if (!pattern(*pane.get(), UIA_ScrollPatternId, IID_IUIAutomationScrollPattern, scroll))
        return;
      BOOL horizontal{TRUE};
      BOOL vertical{FALSE};
      ok(scroll->get_CurrentHorizontallyScrollable(&horizontal), "read horizontal scrolling");
      ok(scroll->get_CurrentVerticallyScrollable(&vertical), "read vertical scrolling");
      expect(horizontal == VARIANT_FALSE && vertical != FALSE,
             "the pane scrolls only where it overflows");
      double percent{};
      ok(scroll->get_CurrentHorizontalScrollPercent(&percent), "read horizontal percent");
      expect(percent == kNoScroll, "a fixed axis reports NoScroll");
      ok(scroll->get_CurrentVerticalScrollPercent(&percent), "read vertical percent");
      expect(percent >= 0.0 && percent <= 100.0, "the scrolling axis reports a percentage");
      expect(scroll->SetScrollPercent(kNoScroll, 100.0) == S_OK,
             "scroll to the bottom of the pane");
      expect(wait([&] {
               double now = 0.0;
               return SUCCEEDED(scroll->get_CurrentVerticalScrollPercent(&now)) && now > 90.0;
             }),
             "the pane vertically scrolls to the requested percentage");
      int position = 0;
      int last_row = 0;
      fixture.on_fltk([&] {
        const int bar = fixture.pane.scrollbar_size() > 0 ? fixture.pane.scrollbar_size()
                                                         : Fl::scrollbar_size();
        const int view = fixture.pane.h() - Fl::box_dh(fixture.pane.box()) -
                         (fixture.pane.hscrollbar.visible() ? bar : 0);
        position = fixture.pane.yposition();
        last_row = 400 - view; // the pane's child spans 0..400 inside the pane.
      });
      expect(position == last_row,
             "the pane scrolls to its last content row, scrollbar included");
      // The vertical bar narrows this pane's viewport until the content
      // overflows sideways, which only a scrollbar-aware extent reports.
      auto wide = find(*client.automation.get(), *root.get(), L"Wide pane");
      Ref<IUIAutomationScrollPattern> wide_scroll;
      if (wide && pattern(*wide.get(), UIA_ScrollPatternId, IID_IUIAutomationScrollPattern,
                          wide_scroll)) {
        BOOL widens{FALSE};
        ok(wide_scroll->get_CurrentHorizontallyScrollable(&widens),
           "read the wide pane's horizontal axis");
        expect(widens != FALSE,
               "the vertical scrollbar narrows the viewport until the content overflows");
        expect(wide_scroll->SetScrollPercent(kNoScroll, 100.0) == S_OK,
               "scroll the wide pane to its bottom");
        expect(wait([&] {
                 double now = -1.0;
                 return SUCCEEDED(wide_scroll->get_CurrentVerticalScrollPercent(&now)) &&
                        now >= 99.5;
               }),
               "a scrollbar-aware viewport reaches the last content row");
      }
      expect(wait([&] { return log->property_count(UIA_ScrollVerticalScrollPercentPropertyId) > 0; }),
             "a scroll change raises a property-changed event");
      const double before = [&] {
        double now = 0.0;
        ok(scroll->get_CurrentVerticalScrollPercent(&now), "read percentage before stepping");
        return now;
      }();
      expect(scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeDecrement) == S_OK,
             "step the pane up by one large increment");
      expect(wait([&] {
               double now = 0.0;
               return SUCCEEDED(scroll->get_CurrentVerticalScrollPercent(&now)) &&
                      now < before - 5.0;
             }),
             "a large decrement moves the published percentage down");
      expect(scroll->SetScrollPercent(150.0, 50.0) == E_INVALIDARG,
             "an out-of-range percentage is rejected");
      expect(scroll->SetScrollPercent(50.0, 50.0) == E_INVALIDARG,
             "a percentage on a fixed axis is rejected");
      Ref<IUIAutomationScrollPattern> list_scroll;
      auto list = find(*client.automation.get(), *root.get(), L"Long rows");
      if (list && pattern(*list.get(), UIA_ScrollPatternId, IID_IUIAutomationScrollPattern,
                          list_scroll)) {
        BOOL scrolls{FALSE};
        ok(list_scroll->get_CurrentVerticallyScrollable(&scrolls), "read list vertical scrolling");
        expect(scrolls != FALSE, "a long list reports its vertical axis without being drawn");
        expect(list_scroll->SetScrollPercent(kNoScroll, 100.0) == S_OK,
               "scroll the list to its last row");
        expect(wait([&] {
                 double now = -1.0;
                 return SUCCEEDED(list_scroll->get_CurrentVerticalScrollPercent(&now)) &&
                        now >= 99.5;
               }),
               "a list reaches its last row");
        int list_position = 0;
        int list_last = 0;
        fixture.on_fltk([&] {
          const auto &rows = fixture.long_rows;
          const int bar = rows.scrollbar_size() > 0 ? rows.scrollbar_size() : Fl::scrollbar_size();
          const int content_height = (rows.*ListHeight::height)();
          const int content_width = (rows.*ListHeight::width)();
          int view_width = rows.w() - Fl::box_dw(rows.box());
          int view_height = rows.h() - Fl::box_dh(rows.box());
          // Only a scrollbar on the other axis shortens the measured one.
          if (content_height > view_height) view_width -= bar;
          if (content_width > view_width) view_height -= bar;
          list_position = rows.vposition();
          list_last = content_height - view_height;
        });
        expect(list_position == list_last,
               "a list scrolls to its last row, scrollbar included (" +
                   std::to_string(list_position) + " vs " + std::to_string(list_last) + ")");
      }

      fixture.close_from_client();
      expect(wait([&] { return fixture.closed.load(); }), "the fixture window closes");
    } catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected UIA pattern exception"); }
  });
  Fl::run();
  worker.join();
}
} // namespace

// CTest runs every case; passing a case name runs just that one, which keeps a
// single failing case cheap to reproduce.
// D2: an idle client costs at most one walk per refresh interval, and an
// app-side change is published without any client request.
void idle_and_app_side_changes() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize idle fixture STA")) return;
  struct Sta {
    ~Sta() { CoUninitialize(); }
  } sta;
  Fl::lock();
  Fixture fixture;
  const HWND hwnd = fl_xid(&fixture.window);
  // The FLTK thread owns the loop, so it times the waits; the client thread
  // connects, waits for the timing to finish, then makes the app-side change.
  std::atomic<int> stage{0};  // 0 connecting, 1 ready, 2 timed, 3 finished
  std::thread worker([&] {
    const CloseOnExit close_on_exit{fixture};
    try {
      Client client;
      if (!client) return;
      Ref<IUIAutomationElement> root;
      if (!ok(client.automation->ElementFromHandle(hwnd, root.put()), "client finds FLTK HWND") ||
          !root)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      stage = 1;
      while (stage.load() < 2 && !fixture.closed.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // The application moves its own slider from a timeout: no client call and
      // no input, so only the periodic publish can carry it.
      auto slider = find(*client.automation.get(), *root.get(), L"Opacity");
      Ref<IUIAutomationRangeValuePattern> range;
      if (slider &&
          pattern(*slider.get(), UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern,
                  range)) {
        const auto mark = std::chrono::steady_clock::now();
        // FLTK timeouts belong to its own thread, so the client asks that thread
        // to schedule the change rather than calling add_timeout from here.
        Fl::awake(
            [](void *data) {
              Fl::add_timeout(
                  0.05, [](void *slider) { static_cast<Fl_Slider *>(slider)->value(42.5); },
                  data);
            },
            &fixture.opacity);
        const bool landed = wait(
            [&] {
              double now{};
              return SUCCEEDED(range->get_CurrentValue(&now)) && std::abs(now - 42.5) < 0.01;
            },
            1500);
        const auto change = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mark);
        std::cerr << "[activity] the change reached the client in " << change.count()
                  << " ms\n";
        expect(landed, "an app-side change reaches the client");
        expect(change.count() <= 400, "the change arrives within two refresh intervals");
      }
      stage = 3;
      fixture.close_from_client();
      expect(wait([&] { return fixture.closed.load(); }), "the fixture window closes");
    } catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected UIA pattern exception"); }
    stage = 3;
  });
  // Serve the loop until the client is connected.
  while (stage.load() == 0 && !fixture.closed.load()) (void)Fl::wait(0.02);
  expect(UiaClientsAreListening() != FALSE, "the client keeps demand true");
  // Two warm-up waits settle the interval the connection left armed; then an
  // idle loop must sleep through each whole slice instead of waking for us.
  for (int warm = 0; warm < 2; ++warm) (void)Fl::wait(0.5);
  long long shortest = 5000;
  for (int pass = 0; pass < 3; ++pass) {
    const auto before = std::chrono::steady_clock::now();
    (void)Fl::wait(0.5);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - before)
                          .count();
    std::cerr << "[idle] wait " << pass << " took " << took << " ms\n";
    if (took < shortest) shortest = took;
  }
  stage = 2;
  // A wait lasts at least its slice unless something wakes the loop early, and
  // slowness can only lengthen it, so this holds under load too.
  expect(shortest >= 400, "an idle loop is not woken by the accessibility refresh");
  while (!fixture.closed.load()) (void)Fl::wait(0.02);
  worker.join();
}

int main(int argc, char **argv) {
  const std::string_view only = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
  const layerc::test::Case cases[] = {
      LAYERC_TEST_CASE(toggle_and_range_patterns),
      LAYERC_TEST_CASE(selection_and_selection_item),
      LAYERC_TEST_CASE(expand_collapse_and_scroll),
      LAYERC_TEST_CASE(idle_and_app_side_changes),
  };
  std::size_t matched = 0;
  int code = 0;
  for (const auto &test_case : cases) {
    if (!only.empty() && test_case.name != only) continue;
    ++matched;
    code |= layerc::test::run({test_case});
  }
  if (matched == 0) {
    std::cerr << "no case named '" << only << "'\n";
    return EXIT_FAILURE;
  }
  return code;
}
