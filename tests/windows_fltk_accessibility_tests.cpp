#include "a11y/input.hpp"
#include "a11y/uia/bridge.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
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
using layerc::editor::a11y::Input;
using layerc::editor::a11y::Role;
using layerc::editor::a11y::uia::Bridge;

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
struct String {
  BSTR value{};
  String() = default;
  explicit String(const wchar_t *text) : value(SysAllocString(text)) {}
  ~String() { SysFreeString(value); }
  String(const String &) = delete;
  String &operator=(const String &) = delete;
  std::wstring text() const {
    return value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
  }
};
struct Apartment {
  HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
bool ok(HRESULT result, const char *message) {
  if (FAILED(result)) std::cerr << message << ": HRESULT=" << std::hex
                                  << static_cast<unsigned long>(result) << std::dec << '\n';
  return expect(SUCCEEDED(result), message);
}

class Events final : public IUIAutomationEventHandler,
                     public IUIAutomationPropertyChangedEventHandler,
                     public IUIAutomationFocusChangedEventHandler,
                     public IUIAutomationNotificationEventHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAgileObject) ||
        IsEqualIID(iid, IID_IUIAutomationEventHandler))
      *result = static_cast<IUIAutomationEventHandler *>(this);
    else if (IsEqualIID(iid, IID_IUIAutomationPropertyChangedEventHandler))
      *result = static_cast<IUIAutomationPropertyChangedEventHandler *>(this);
    else if (IsEqualIID(iid, IID_IUIAutomationFocusChangedEventHandler))
      *result = static_cast<IUIAutomationFocusChangedEventHandler *>(this);
    else if (IsEqualIID(iid, IID_IUIAutomationNotificationEventHandler))
      *result = static_cast<IUIAutomationNotificationEventHandler *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *, EVENTID id) override {
    if (id == UIA_Text_TextChangedEventId) ++text_changes;
    if (id == UIA_Text_TextSelectionChangedEventId) ++selection_changes;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(
      IUIAutomationElement *, PROPERTYID id, VARIANT) override {
    if (id == UIA_ValueValuePropertyId) ++value_changes;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement *sender) override {
    int process{};
    if (sender && SUCCEEDED(sender->get_CurrentProcessId(&process)) &&
        static_cast<DWORD>(process) == GetCurrentProcessId()) ++focus_changes;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE HandleNotificationEvent(IUIAutomationElement *,
      NotificationKind, NotificationProcessing, BSTR text, BSTR) override {
    try {
      const std::lock_guard lock(mutex);
      notification.assign(text, SysStringLen(text));
      return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
  }
  std::atomic<int> text_changes{}, selection_changes{}, value_changes{}, focus_changes{};
  std::mutex mutex;
  std::wstring notification;
private:
  std::atomic<ULONG> references_{1};
};

struct Fixture {
  // Disposable component fixture: no profile store, daemon or production paths.
  Fl_Double_Window window{640, 240, "LayerE UIA fixture"};
  Input field{130, 32, 350, 28, "Profile name"};
  Input readonly{130, 76, 350, 28, "Read-only value"};
  Fl_Button button{130, 120, 120, 28, "Run fixture"};
  Fl_Box status{130, 164, 350, 28, "Waiting"};
  std::unique_ptr<Bridge> bridge;
  std::atomic<int> invoked{};
  std::atomic<bool> closed{};
  Fixture() {
    window.end();
    field.value("Initial");
    readonly.value("Preserved");
    readonly.readonly(1);
    button.callback([](Fl_Widget *, void *data) {
      auto &self = *static_cast<Fixture *>(data);
      ++self.invoked;
      self.status.copy_label("Fixture status");
      self.bridge->announce("Fixture status");
    }, this);
    window.show();
    Fl::check();
    bridge = std::make_unique<Bridge>(window);
    bridge->bind(field, Role::text_field, "Profile name", "Editable fixture field");
    bridge->bind(readonly, Role::text_field, "Read-only value");
    bridge->bind(button, Role::button, "Run fixture");
    bridge->bind(status, Role::status, "Fixture status");
  }
  void close_from_client() {
    Fl::awake([](void *data) {
      auto &self = *static_cast<Fixture *>(data);
      self.window.hide();
      self.closed.store(true);
    }, this);
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

void text_ranges(IUIAutomation &automation, IUIAutomationElement &field,
                  IUIAutomationTextPattern &text) {
  Ref<IUIAutomationTextRange> document;
  if (!ok(text.get_DocumentRange(document.put()), "document range") || !document) return;
  String content;
  if (!ok(document->GetText(-1, &content.value), "read document text")) return;
  expect(content.text() == L"Caf\u00e9 \u03b2 \U0001f642 ready", "UTF-8/UTF-16 document round-trip");
  Ref<IUIAutomationTextRange> glyph;
  if (!ok(document->Clone(glyph.put()), "clone range") || !glyph) return;
  BOOL same{};
  ok(glyph->Compare(document.get(), &same), "compare ranges");
  expect(same != FALSE, "cloned range has equal endpoints");
  int moved{};
  ok(glyph->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Document, -1, &moved),
      "collapse range to start");
  ok(glyph->ExpandToEnclosingUnit(TextUnit_Character), "expand character");
  ok(glyph->Move(TextUnit_Character, 7, &moved), "move by Unicode characters");
  expect(moved == 7, "non-BMP character is one character unit");
  String emoji;
  ok(glyph->GetText(-1, &emoji.value), "read moved character");
  expect(emoji.text() == L"\U0001f642", "character movement preserves surrogate pair");
  String truncated;
  ok(glyph->GetText(1, &truncated.value), "bounded text read");
  expect(truncated.text().empty(), "maximum length never splits a surrogate pair");
  int distance{};
  ok(glyph->CompareEndpoints(TextPatternRangeEndpoint_Start, document.get(),
                             TextPatternRangeEndpoint_Start, &distance), "compare endpoints");
  expect(distance == 7, "endpoints use UTF-16 units");
  ok(glyph->Select(), "select a text range on the UI thread");
  Ref<IUIAutomationTextRangeArray> selection;
  ok(text.GetSelection(selection.put()), "get selection");
  if (selection) {
    int length{};
    ok(selection->get_Length(&length), "selection length");
    expect(length == 1, "single selection is exposed");
    Ref<IUIAutomationTextRange> selected;
    if (length == 1 && ok(selection->GetElement(0, selected.put()), "selection range") && selected) {
      String value;
      ok(selected->GetText(-1, &value.value), "read selection text");
      expect(value.text() == L"\U0001f642", "client selection reaches FLTK byte offsets");
    }
  }
  Ref<IUIAutomationTextRange> extended;
  if (ok(glyph->Clone(extended.put()), "clone selected character") && extended) {
    ok(extended->MoveEndpointByRange(TextPatternRangeEndpoint_End, document.get(),
                                     TextPatternRangeEndpoint_End), "move endpoint by range");
    String value;
    ok(extended->GetText(-1, &value.value), "read extended range");
    expect(value.text() == L"\U0001f642 ready", "endpoint extension reaches document end");
  }
  Ref<IUIAutomationTextRange> word;
  if (ok(document->Clone(word.put()), "clone for word movement") && word) {
    ok(word->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Document, -1, &moved),
        "collapse word range");
    ok(word->ExpandToEnclosingUnit(TextUnit_Word), "expand word");
    ok(word->Move(TextUnit_Word, 1, &moved), "move by word");
    String value;
    ok(word->GetText(-1, &value.value), "read word");
    expect(value.text() == L"\u03b2 ", "word movement follows logical text");
    ok(word->ExpandToEnclosingUnit(TextUnit_Line), "expand line");
    BOOL equals{};
    ok(word->Compare(document.get(), &equals), "compare single line with document");
    expect(equals != FALSE, "single-line field has one line range");
  }
  String needle(L"READY");
  Ref<IUIAutomationTextRange> found;
  ok(document->FindText(needle.value, TRUE, TRUE, found.put()), "case-insensitive backward search");
  if (expect(static_cast<bool>(found), "matching range exists")) {
    String value;
    ok(found->GetText(-1, &value.value), "read search match");
    expect(value.text() == L"ready", "search returns original case");
  }
  VARIANT attribute{};
  ok(document->GetAttributeValue(UIA_IsReadOnlyAttributeId, &attribute), "text read-only attribute");
  expect(attribute.vt == VT_BOOL && attribute.boolVal == VARIANT_FALSE, "editable text attribute");
  Ref<IUIAutomationTextRange> matching;
  ok(document->FindAttribute(UIA_IsReadOnlyAttributeId, attribute, FALSE, matching.put()),
      "find uniform text attribute");
  VariantClear(&attribute);
  expect(static_cast<bool>(matching), "matching attribute returns a range");
  SAFEARRAY *rectangles{};
  if (ok(document->GetBoundingRectangles(&rectangles), "screen-pixel text rectangles") && rectangles) {
    LONG high{};
    ok(SafeArrayGetUBound(rectangles, 1, &high), "rectangle array bounds");
    expect(high >= 3, "visible text has a nonempty rectangle");
    SafeArrayDestroy(rectangles);
  }
  RECT bounds{};
  ok(field.get_CurrentBoundingRectangle(&bounds), "field bounds");
  Ref<IUIAutomationTextRange> point;
  ok(text.RangeFromPoint({bounds.left + 10, (bounds.top + bounds.bottom) / 2}, point.put()),
      "range from screen point");
  expect(static_cast<bool>(point), "hit test returns a caret range");
  Ref<IUIAutomationTextRangeArray> visible;
  ok(text.GetVisibleRanges(visible.put()), "visible text ranges");
  if (visible) {
    int length{};
    ok(visible->get_Length(&length), "visible range count");
    expect(length > 0, "visible field exposes its text");
  }
  Ref<IUIAutomationElement> enclosing;
  if (ok(document->GetEnclosingElement(enclosing.put()), "enclosing text element") && enclosing) {
    BOOL equal{};
    ok(automation.CompareElements(&field, enclosing.get(), &equal), "compare enclosing element");
    expect(equal != FALSE, "range belongs to its field");
  }
  Ref<IUIAutomationElementArray> children;
  if (ok(document->GetChildren(children.put()), "embedded text children") && children) {
    int length{};
    ok(children->get_Length(&length), "embedded child count");
    expect(length == 0, "plain input has no embedded children");
  }
  ok(document->ScrollIntoView(TRUE), "already-visible range remains visible without selection changes");
}

void client(Fixture &fixture, HWND hwnd) {
  Apartment apartment;
  if (!ok(apartment.result, "initialize client MTA")) return;
  Ref<IUIAutomation> automation;
  if (!ok(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IUIAutomation, reinterpret_cast<void **>(automation.put())),
          "create real IUIAutomation client") || !automation) return;
  Ref<IUIAutomation2> timeouts;
  if (ok(automation->QueryInterface(IID_IUIAutomation2,
                                   reinterpret_cast<void **>(timeouts.put())), "UIA timeout interface")) {
    ok(timeouts->put_ConnectionTimeout(2000), "bound connection timeout");
    ok(timeouts->put_TransactionTimeout(2000), "bound transaction timeout");
  }
  Ref<IUIAutomationElement> root;
  if (!ok(automation->ElementFromHandle(hwnd, root.put()), "client finds FLTK HWND") || !root) return;
  String root_name;
  ok(root->get_CurrentName(&root_name.value), "root accessible name");
  expect(root_name.text() == L"LayerE UIA fixture", "root name is the window title");
  auto field = find(*automation.get(), *root.get(), L"Profile name");
  auto readonly = find(*automation.get(), *root.get(), L"Read-only value");
  auto button = find(*automation.get(), *root.get(), L"Run fixture");
  if (!field || !readonly || !button) return;
  CONTROLTYPEID type{};
  ok(field->get_CurrentControlType(&type), "field role");
  expect(type == UIA_EditControlTypeId, "field is an Edit, not anonymous custom chrome");
  String help;
  ok(field->get_CurrentHelpText(&help.value), "field description");
  expect(help.text() == L"Editable fixture field", "description reaches client");
  Ref<IUIAutomationTreeWalker> walker;
  if (ok(automation->get_RawViewWalker(walker.put()), "raw tree walker") && walker) {
    Ref<IUIAutomationElement> current;
    ok(walker->GetFirstChildElement(root.get(), current.put()), "first child");
    std::vector<std::wstring> names;
    for (int count = 0; current && count < 20; ++count) {
      String name;
      ok(current->get_CurrentName(&name.value), "child name");
      if (name.text() == L"Profile name" || name.text() == L"Read-only value" ||
          name.text() == L"Run fixture" || name.text() == L"Fixture status")
        names.push_back(name.text());
      Ref<IUIAutomationElement> next;
      ok(walker->GetNextSiblingElement(current.get(), next.put()), "next sibling");
      current = std::move(next);
    }
    expect(names == std::vector<std::wstring>{L"Profile name", L"Read-only value",
        L"Run fixture", L"Fixture status"}, "tree keeps reading/focus order");
  }
  Ref<Events> events;
  *events.put() = new Events;
  struct Unsubscribe { IUIAutomation *client; ~Unsubscribe() { client->RemoveAllEventHandlers(); } }
      unsubscribe{automation.get()};
  for (const auto id : {UIA_Text_TextChangedEventId, UIA_Text_TextSelectionChangedEventId})
    ok(automation->AddAutomationEventHandler(id, field.get(), TreeScope_Element, nullptr, events.get()),
        "subscribe text event");
  PROPERTYID properties[]{UIA_ValueValuePropertyId};
  ok(automation->AddPropertyChangedEventHandlerNativeArray(field.get(), TreeScope_Element,
      nullptr, events.get(), properties, 1), "subscribe value changes");
  ok(automation->AddFocusChangedEventHandler(nullptr, events.get()), "subscribe focus changes");
  Ref<IUIAutomation5> notifications;
  if (ok(automation->QueryInterface(IID_IUIAutomation5,
                                    reinterpret_cast<void **>(notifications.put())), "UIA notifications interface"))
    ok(notifications->AddNotificationEventHandler(root.get(), TreeScope_Element, nullptr, events.get()),
        "subscribe status notifications");
  Ref<IUIAutomationValuePattern> value;
  if (!ok(field->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                      reinterpret_cast<void **>(value.put())), "editable Value pattern") || !value) return;
  ok(field->SetFocus(), "focus action reaches UI thread");
  BOOL focused{};
  ok(field->get_CurrentHasKeyboardFocus(&focused), "current focus");
  expect(focused != FALSE, "FLTK focus is reported back to UIA");
  String updated(L"Caf\u00e9 \u03b2 \U0001f642 ready");
  ok(value->SetValue(updated.value), "set real FLTK input through UIA");
  String round_trip;
  ok(value->get_CurrentValue(&round_trip.value), "read changed value");
  expect(round_trip.text() == updated.text(), "value mutation commits before returning");
  Ref<IUIAutomationTextPattern> text;
  if (ok(field->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                   reinterpret_cast<void **>(text.put())), "Text pattern") && text)
    text_ranges(*automation.get(), *field.get(), *text.get());
  Ref<IUIAutomationValuePattern> read_value;
  if (ok(readonly->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                      reinterpret_cast<void **>(read_value.put())), "read-only Value pattern") && read_value) {
    BOOL immutable{};
    ok(read_value->get_CurrentIsReadOnly(&immutable), "read-only state");
    expect(immutable != FALSE, "read-only state is preserved");
    expect(FAILED(read_value->SetValue(updated.value)), "read-only mutation is rejected");
    String preserved;
    ok(read_value->get_CurrentValue(&preserved.value), "read rejected mutation result");
    expect(preserved.text() == L"Preserved", "failed mutation has no effect");
  }
  Ref<IUIAutomationInvokePattern> invoke;
  if (ok(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern,
                                     reinterpret_cast<void **>(invoke.put())), "Invoke pattern") && invoke)
    ok(invoke->Invoke(), "invoke real FLTK callback");
  expect(fixture.invoked.load() == 1, "callback executed once");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    bool notified{};
    { const std::lock_guard lock(events->mutex); notified = events->notification == L"Fixture status"; }
    if (events->text_changes.load() > 0 && events->selection_changes.load() > 0 &&
        events->value_changes.load() > 0 && events->focus_changes.load() > 0 && notified) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  expect(events->text_changes.load() > 0, "client receives text-changed event");
  expect(events->selection_changes.load() > 0, "client receives selection-changed event");
  expect(events->value_changes.load() > 0, "client receives value property event");
  expect(events->focus_changes.load() > 0, "client receives focus event");
  { const std::lock_guard lock(events->mutex);
    expect(events->notification == L"Fixture status", "client receives status notification"); }
  fixture.close_from_client();
  const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!fixture.closed.load() && std::chrono::steady_clock::now() < close_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(fixture.closed.load(), "window closes through owned FLTK loop");
  String unavailable;
  expect(FAILED(field->get_CurrentName(&unavailable.value)), "retained provider disconnects on window close");
}

void real_client_tree_and_text() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fixture fixture;
  const auto error = fixture.bridge->sync();
  if (!expect(!error, error ? error->message : "valid UIA bridge")) return;
  const HWND hwnd = fl_xid(&fixture.window);
  std::thread worker([&] {
    try { client(fixture, hwnd); }
    catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected UIA client exception"); }
    if (!fixture.closed.load()) fixture.close_from_client();
  });
  Fl::run();
  worker.join();
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(real_client_tree_and_text))
