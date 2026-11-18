#include "a11y/fltk_tree.hpp"
#include "a11y/input.hpp"
#include "a11y/uia/adaptor.hpp"
#include "a11y/uia/context.hpp"
#include "a11y/uia/text.hpp"
#include "accessible_widgets.hpp"
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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using layerc::test::expect;
using layerc::editor::a11y::FltkTree;
using layerc::editor::a11y::Input;
using layerc::editor::a11y::Role;
using layerc::editor::a11y::uia::Adaptor;
using layerc::editor::accessibility::set_accessible_description;
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
  [[nodiscard]] Ref<IUIAutomationElement> root(const HWND hwnd) {
    Ref<IUIAutomationElement> element;
    if (static_cast<bool>(automation))
      ok(automation->ElementFromHandle(hwnd, element.put()), "client finds FLTK HWND");
    return element;
  }
};

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
      notifications.push_back(notification);
      return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
  }
  [[nodiscard]] std::size_t notification_count() const {
    const std::lock_guard lock(mutex);
    return notifications.size();
  }
  std::atomic<int> text_changes{}, selection_changes{}, value_changes{}, focus_changes{};
  mutable std::mutex mutex;
  std::wstring notification;
  std::vector<std::wstring> notifications;
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
  FltkTree tree;
  std::unique_ptr<Adaptor> adaptor;
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
      self.adaptor->announce(&self.status, "Fixture status");
    }, this);
    set_accessible_role(field, Role::text_field);
    set_accessible_name(field, "Profile name");
    set_accessible_description(field, "Editable fixture field");
    set_accessible_role(readonly, Role::text_field);
    set_accessible_name(readonly, "Read-only value");
    set_accessible_role(button, Role::button);
    set_accessible_name(button, "Run fixture");
    set_accessible_role(status, Role::status);
    set_accessible_name(status, "Fixture status");
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

// M2: sparse visible runs must keep their own document offsets. The model
// accepts runs that omit offscreen text, so the adaptor is tested against a
// published snapshot rather than only the contiguous runs FLTK emits.
void sparse_text_runs(HWND hwnd) {
  using namespace layerc::editor::a11y;
  auto context = std::make_shared<uia::Context>(hwnd);
  context->root.store(1);
  Snapshot tree;
  tree.root = 1;
  Node root;
  root.id = 1;
  root.role = Role::window;
  root.children = {2};
  tree.nodes.emplace(1, root);
  Node field;
  field.id = 2;
  field.parent = 1;
  field.role = Role::text_field;
  field.name = "Sparse runs";
  field.bounds = {0, 0, 200, 20};
  // "\U0001f642xa\u20acyb": runs cover only "a" (bytes 5..6) and "b" (10..11).
  field.text = Text{"\xf0\x9f\x99\x82xa\xe2\x82\xacyb", 11, 11};
  field.text->runs = {{5, 6, {10, 0, 8, 12}}, {10, 11, {60, 0, 8, 12}}};
  field.text->caret_bounds = {70, 0, 1, 12};
  tree.nodes.emplace(2, field);
  const auto error = context->model.publish(tree);
  if (!expect(!error, error ? error->message : "sparse text snapshot publishes")) return;
  const auto info = uia::text_info(context, 2);
  if (!expect(info.has_value(), "sparse text info is available")) return;
  expect(info->value == L"\U0001f642xa\u20acyb", "document text keeps every UTF-16 unit");
  if (!expect(info->glyphs.size() == 2, "only visible runs become glyphs")) return;
  expect(info->glyphs[0].begin == 3 && info->glyphs[0].end == 4,
         "the first run keeps its own UTF-16 offsets");
  expect(info->glyphs[1].begin == 6 && info->glyphs[1].end == 7,
         "the second run keeps its own UTF-16 offsets");
  expect(info->glyphs[0].bounds.left == 10 && info->glyphs[1].bounds.left == 60,
         "glyph bounds stay attached to their own runs");
  Ref<ITextRangeProvider> range;
  *range.put() = uia::make_range(context, 2, {6, 7});
  if (!expect(range.get() != nullptr, "sparse run range is available")) return;
  String text;
  if (ok(range->GetText(-1, &text.value), "read sparse run text"))
    expect(text.text() == L"b", "a range over the second run reads its own glyph");
  SAFEARRAY *boxes{};
  if (ok(range->GetBoundingRectangles(&boxes), "sparse run rectangles") && boxes) {
    double left{};
    LONG index{};
    ok(SafeArrayGetElement(boxes, &index, &left), "first sparse rectangle left");
    expect(left == 60, "the second run keeps its own rectangle");
    SafeArrayDestroy(boxes);
  }
}

// M3: a horizontally scrolled field must refuse an offscreen insertion point
// instead of claiming it was revealed.
void scrolled_input_range(IUIAutomationElement &field,
                          IUIAutomationTextPattern &text) {
  Ref<IUIAutomationValuePattern> value;
  if (!ok(field.GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                    reinterpret_cast<void **>(value.put())),
          "value pattern for wider text") || !value) return;
  std::wstring wide;
  for (int count = 0; count < 24; ++count) wide += L"0123456789";
  String widened(wide.c_str());
  if (!ok(value->SetValue(widened.value), "set text wider than the field")) return;
  // FLTK scrolls its own viewport around the caret when it draws, and the
  // adaptor republishes geometry from its UI-thread check callback, so poll the
  // hit test instead of sleeping for a fixed period.
  RECT bounds{};
  if (!ok(field.get_CurrentBoundingRectangle(&bounds), "field bounds for wider text")) return;
  Ref<IUIAutomationTextRange> document;
  if (!ok(text.get_DocumentRange(document.put()), "document range for wider text") || !document)
    return;
  int start{};
  HRESULT hit_status = E_FAIL;
  HRESULT compare_status = E_FAIL;
  const auto scroll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  for (;;) {
    Ref<IUIAutomationTextRange> hit;
    hit_status = text.RangeFromPoint({bounds.left + 2, (bounds.top + bounds.bottom) / 2},
                                     hit.put());
    if (SUCCEEDED(hit_status) && hit)
      compare_status = hit->CompareEndpoints(TextPatternRangeEndpoint_Start, document.get(),
                                             TextPatternRangeEndpoint_Start, &start);
    if ((SUCCEEDED(compare_status) && start > 0) ||
        std::chrono::steady_clock::now() >= scroll_deadline)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ok(hit_status, "hit test at the scrolled left edge");
  ok(compare_status, "compare the left edge with the document start");
  expect(start > 0, "the caret at the end scrolls the document start offscreen");
  Ref<IUIAutomationTextRange> beginning;
  if (!ok(document->Clone(beginning.put()), "clone the wider document") || !beginning) return;
  int moved{};
  ok(beginning->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Document, -1, &moved),
     "collapse the wider range to the document start");
  expect(beginning->ScrollIntoView(TRUE) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
         "an offscreen insertion point is not reported as revealed");
  Ref<IUIAutomationTextRangeArray> selection;
  if (!ok(text.GetSelection(selection.put()), "selection after a refused reveal") || !selection)
    return;
  int length{};
  ok(selection->get_Length(&length), "selection count after a refused reveal");
  if (!expect(length == 1, "one selection remains after a refused reveal") || length != 1) return;
  Ref<IUIAutomationTextRange> caret;
  if (!ok(selection->GetElement(0, caret.put()), "caret range after a refused reveal") || !caret)
    return;
  int at_end{};
  ok(caret->CompareEndpoints(TextPatternRangeEndpoint_End, document.get(),
                              TextPatternRangeEndpoint_End, &at_end),
     "compare the caret with the document end");
  expect(at_end == 0, "a refused reveal leaves the caret untouched");
  expect(caret->ScrollIntoView(TRUE) == S_OK, "the visible caret range is revealed");
}

// H1: one Tab through FLTK's own keyboard dispatch, executed on the UI thread.
struct TabStep {
  Fl_Window *window{};
  bool backward{};
  std::shared_ptr<std::atomic<int>> handled;
};

// H1: the fixture holds the foreground while traversal is measured, so the
// desktop focus state the client reads describes the fixture. A locked or busy
// desktop keeps another window active; `activate_now` reports that instead of
// pretending the fixture was observed.
bool activate(Fl_Window &window) {
  auto *hwnd = fl_xid(&window);
  if (hwnd == nullptr) return false;
  const HWND root = GetAncestor(hwnd, GA_ROOT);
  const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  const DWORD current = GetCurrentThreadId();
  const bool attached = foreground != 0 && foreground != current;
  if (attached) AttachThreadInput(current, foreground, TRUE);
  SetForegroundWindow(root);
  ::SetFocus(hwnd);
  if (attached) AttachThreadInput(current, foreground, FALSE);
  return GetForegroundWindow() == root;
}

void press_tab(void *data) {
  std::unique_ptr<TabStep> step(static_cast<TabStep *>(data));
  activate(*step->window);
  Fl::e_keysym = FL_Tab;
  Fl::e_state = step->backward ? FL_SHIFT : 0;
  Fl::e_length = 0;
  step->handled->store(Fl::handle(FL_KEYBOARD, step->window));
}

// H1: activation must happen on the thread that owns the window, so the
// provider's own ::GetFocus() check observes the fixture while traversal runs.
struct ActivateStep {
  Fl_Window *window{};
  std::shared_ptr<std::atomic<int>> done;
};

void activate_ui(void *data) {
  std::unique_ptr<ActivateStep> step(static_cast<ActivateStep *>(data));
  step->done->store(activate(*step->window) ? 1 : 0);
}

// Returns whether the fixture owns the foreground: only then does the desktop
// focus state read by the client describe this fixture.
bool activate_now(Fl_Window &window) {
  auto done = std::make_shared<std::atomic<int>>(-1);
  Fl::awake(activate_ui, new ActivateStep{&window, done});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (done->load() == -1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return done->load() == 1;
}

// H1: the fixture's own keyboard focus, read through this provider's published
// state. A locked or otherwise busy desktop keeps another application in front,
// and then the desktop focus element names that application instead.
std::wstring provider_focus(IUIAutomationElement &element, IUIAutomationTreeWalker &walker) {
  BOOL keyboard_focus{};
  if (SUCCEEDED(element.get_CurrentHasKeyboardFocus(&keyboard_focus)) &&
      keyboard_focus != FALSE) {
    String name;
    if (SUCCEEDED(element.get_CurrentName(&name.value))) return name.text();
  }
  Ref<IUIAutomationElement> child;
  if (FAILED(walker.GetFirstChildElement(&element, child.put())) || !child) return {};
  for (;;) {
    std::wstring found = provider_focus(*child.get(), walker);
    if (!found.empty()) return found;
    Ref<IUIAutomationElement> sibling;
    if (FAILED(walker.GetNextSiblingElement(child.get(), sibling.put())) || !sibling) return {};
    child = std::move(sibling);
  }
}

struct FocusReading {
  std::wstring name;
  bool from_desktop{};
};

// H1: the focused element as a client reads it, preferring the desktop state
// and falling back to the provider's published focus when another application
// owns the foreground.
FocusReading focus_reading(IUIAutomation &automation, Fl_Window &window) {
  Ref<IUIAutomationElement> fixture;
  Ref<IUIAutomationTreeWalker> walker;
  const bool walkable =
      SUCCEEDED(automation.ElementFromHandle(fl_xid(&window), fixture.put())) && fixture &&
      SUCCEEDED(automation.get_RawViewWalker(walker.put())) && walker;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (;;) {
    Ref<IUIAutomationElement> focused;
    if (SUCCEEDED(automation.GetFocusedElement(focused.put())) && focused) {
      String id;
      // Only this provider's elements describe the fixture: whatever another
      // application on the desktop has focused is not an answer.
      if (SUCCEEDED(focused->get_CurrentAutomationId(&id.value)) &&
          id.text().rfind(L"layerc-", 0) == 0) {
        String name;
        if (SUCCEEDED(focused->get_CurrentName(&name.value)) && !name.text().empty())
          return {name.text(), true};
      }
    }
    if (walkable) {
      std::wstring name = provider_focus(*fixture.get(), *walker.get());
      if (!name.empty()) return {name, false};
    }
    if (std::chrono::steady_clock::now() >= deadline) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

FocusReading tab_once(IUIAutomation &automation, Fl_Window &window, bool backward,
                      const std::wstring &previous) {
  auto handled = std::make_shared<std::atomic<int>>(-1);
  Fl::awake(press_tab, new TabStep{&window, backward, handled});
  const auto dispatch = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (handled->load() == -1 && std::chrono::steady_clock::now() < dispatch)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!expect(handled->load() > 0, backward ? "Shift+Tab reaches FLTK group navigation"
                                            : "Tab reaches FLTK group navigation"))
    return {};
  // The republished focus and UI Automation's own bookkeeping both settle on
  // the UI thread's message loop; poll until the client sees the new element.
  const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  FocusReading reading = focus_reading(automation, window);
  while (reading.name == previous && std::chrono::steady_clock::now() < settle) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    reading = focus_reading(automation, window);
  }
  return reading;
}

// Hosted GitHub runners have an unlocked interactive desktop, so the desktop
// focus path must work there. A locked interactive desktop cannot grant any
// process the foreground, so the strict path is only required on hosted CI.
bool hosted_ci() {
  wchar_t value[8]{};
  const DWORD length = GetEnvironmentVariableW(L"GITHUB_ACTIONS", value, 8);
  return length > 0 && length < 8 && std::wstring_view(value, length) == L"true";
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
  sparse_text_runs(hwnd);
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
        L"Run fixture", L"Fixture status"}, "tree keeps reading order");
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
                                   reinterpret_cast<void **>(text.put())), "Text pattern") && text) {
    text_ranges(*automation.get(), *field.get(), *text.get());
    scrolled_input_range(*field.get(), *text.get());
  }
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
  // Invoke returns without running the callback: it runs from FLTK's loop, so
  // the client waits for the observed effect instead of assuming it completed.
  const auto invoked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (fixture.invoked.load() == 0 && std::chrono::steady_clock::now() < invoked_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
  // M1: the same announcement repeated is a separate occurrence.
  if (invoke) {
    const auto delivered = events->notification_count();
    ok(invoke->Invoke(), "invoke the announcing action a second time");
    const auto repeat_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (events->notification_count() < delivered + 1 &&
           std::chrono::steady_clock::now() < repeat_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::lock_guard lock(events->mutex);
    expect(events->notifications.size() > delivered &&
           events->notifications.back() == L"Fixture status",
           "a repeated identical announcement still reaches the client");
  }
  // H1: forward and backward traversal through FLTK's real keyboard dispatch.
  {
    Ref<IUIAutomationTreeWalker> tab_walker;
    std::vector<std::wstring> focusable;
    if (ok(automation->get_RawViewWalker(tab_walker.put()), "raw tree walker for focus order") &&
        tab_walker) {
      Ref<IUIAutomationElement> current;
      ok(tab_walker->GetFirstChildElement(root.get(), current.put()),
         "first child for focus order");
      for (int count = 0; current && count < 20; ++count) {
        String name;
        ok(current->get_CurrentName(&name.value), "child name");
        // Only the fixture's own controls belong to its reading order; the
        // window's non-client decorations are not freestanding controls.
        const bool fixture_control = name.text() == L"Profile name" ||
                                     name.text() == L"Read-only value" ||
                                     name.text() == L"Run fixture" ||
                                     name.text() == L"Fixture status";
        if (fixture_control) {
          BOOL keyboard_focusable{};
          ok(current->get_CurrentIsKeyboardFocusable(&keyboard_focusable),
             "child keyboard focusable");
          if (keyboard_focusable != FALSE) focusable.push_back(name.text());
        }
        Ref<IUIAutomationElement> next;
        ok(tab_walker->GetNextSiblingElement(current.get(), next.put()),
           "next sibling for focus order");
        current = std::move(next);
      }
    }
    const std::vector<std::wstring> fixture_order{L"Profile name", L"Read-only value",
                                                   L"Run fixture"};
    expect(focusable == fixture_order, "keyboard-focusable controls match the fixture");
    if (expect(focusable.size() == 3, "the fixture exposes three keyboard-focusable controls")) {
      ok(field->SetFocus(), "restore the fixture field before traversal");
      const bool desktop_focus = activate_now(fixture.window);
      if (!desktop_focus)
        std::cerr << "note: another window owns the foreground (a locked or busy desktop), so "
                     "focus is read from this provider's HasKeyboardFocus state\n";
      FocusReading reading = focus_reading(*automation.get(), fixture.window);
      bool desktop_reads = desktop_focus && reading.from_desktop;
      expect(reading.name == focusable[0], "traversal starts at the first focusable control");
      std::vector<std::wstring> forward;
      for (std::size_t step = 0; step < focusable.size(); ++step) {
        reading = tab_once(*automation.get(), fixture.window, false, reading.name);
        desktop_reads = desktop_reads && reading.from_desktop;
        forward.push_back(reading.name);
      }
      expect(forward == std::vector<std::wstring>{focusable[1], focusable[2], focusable[0]},
             "Tab visits focusable controls in reading order and wraps");
      std::vector<std::wstring> backward;
      for (std::size_t step = 0; step < focusable.size(); ++step) {
        reading = tab_once(*automation.get(), fixture.window, true, reading.name);
        desktop_reads = desktop_reads && reading.from_desktop;
        backward.push_back(reading.name);
      }
      expect(backward == std::vector<std::wstring>{focusable[2], focusable[1], focusable[0]},
             "Shift+Tab visits them in reverse reading order and wraps");
      // A fallback read only counts against the desktop when the fixture owned
      // the foreground for the whole traversal.
      expect(!desktop_focus || desktop_reads,
             "UI Automation reports the fixture's focused element while the fixture owns the "
             "foreground");
      std::cerr << "note: traversal focus read through "
                << (desktop_reads ? "GetFocusedElement" : "the provider fallback")
                << (desktop_focus ? " with the fixture in the foreground"
                                  : " while another window owned the foreground")
                << '\n';
      if (hosted_ci()) {
        // Hosted CI has an unlocked session, so the provider fallback must not
        // stand in for the desktop path there.
        expect(desktop_focus, "hosted CI grants the fixture the desktop foreground");
        expect(desktop_reads,
               "hosted CI reads every traversal step through GetFocusedElement");
      }
      for (const auto &name : forward)
        expect(name != L"Fixture status", "a non-focusable status never receives focus");
      for (const auto &name : backward)
        expect(name != L"Fixture status", "a non-focusable status never receives focus");
    }
  }
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
  const auto error = fixture.adaptor->sync();
  if (!expect(!error, error ? error->message : "valid UIA adaptor")) return;
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

// D1: no client, no walk. The adaptor hosts the window but exports nothing
// until a client asks for the root object, and the explicit sync() above is the
// only walk allowed before that.
void demand_drives_walking() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize demand fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fl_Double_Window window(260, 140, "LayerE UIA demand");
  Fl_Button button{40, 60, 180, 28, "Demand button"};
  window.end();
  window.show();
  Fl::check();
  FltkTree tree;
  Adaptor adaptor(tree);
  const HWND hwnd = fl_xid(&window);
  std::thread worker([&] {
    try {
      // The FLTK loop runs with no client attached for longer than the refresh
      // interval; walking here would be work nobody asked for.
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      expect(adaptor.walks_for_testing() == 0,
             "no tree walk happens while no client asks for the tree");
      expect(adaptor.hosted_windows_for_testing() == 1,
             "the shown window is hosted before any client asks");
      Client client;
      if (!client) return;
      auto root = client.root(hwnd);
      expect(static_cast<bool>(root), "the client reaches the window");
      expect(adaptor.walks_for_testing() > 0,
             "asking for the root object walks the tree");
      Fl::awake([](void *data) { static_cast<Fl_Window *>(data)->hide(); }, &window);
    } catch (...) { expect(false, "unexpected demand exception"); }
  });
  Fl::run();
  worker.join();
}

// D1: every shown top-level window is its own fragment root: the dialog's
// controls appear in the dialog's tree only, and hiding it retires that host
// while the main window keeps working.
void second_window_hosting() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (!ok(apartment, "initialize dialog fixture STA")) return;
  struct Sta { ~Sta() { CoUninitialize(); } } sta;
  Fl::lock();
  Fixture fixture;
  Fl_Double_Window dialog(260, 140, "LayerE UIA dialog");
  Fl_Button dialog_button{40, 60, 180, 28, "Dialog action"};
  dialog.end();
  set_accessible_role(dialog_button, Role::button);
  dialog.show();
  Fl::check();
  const auto error = fixture.adaptor->sync();
  if (!expect(!error, error ? error->message : "valid UIA adaptor")) return;
  expect(fixture.adaptor->hosted_windows_for_testing() == 2,
         "both shown windows are hosted");
  const HWND main_hwnd = fl_xid(&fixture.window);
  const HWND dialog_hwnd = fl_xid(&dialog);
  std::thread worker([&] {
    try {
      Client client;
      if (!client) return;
      auto main_root = client.root(main_hwnd);
      auto dialog_root = client.root(dialog_hwnd);
      if (!expect(static_cast<bool>(main_root) && static_cast<bool>(dialog_root),
                  "the client reaches both windows"))
        return;
      String name;
      ok(dialog_root->get_CurrentName(&name.value), "dialog root name");
      expect(name.text() == L"LayerE UIA dialog", "the dialog is its own root window");
      auto field = find(*client.automation.get(), *main_root.get(), L"Profile name");
      auto dialog_action = find(*client.automation.get(), *dialog_root.get(), L"Dialog action");
      if (!field || !dialog_action) return;
      // The dialog's button must not appear in the main window's subtree.
      VARIANT value{};
      value.vt = VT_BSTR;
      value.bstrVal = SysAllocString(L"Dialog action");
      Ref<IUIAutomationCondition> condition;
      ok(client.automation->CreatePropertyCondition(UIA_NamePropertyId, value, condition.put()),
         "build dialog-name condition");
      VariantClear(&value);
      Ref<IUIAutomationElement> leaked;
      if (condition)
        ok(main_root->FindFirst(TreeScope_Descendants, condition.get(), leaked.put()),
           "search the main window for the dialog control");
      expect(!leaked, "one window's controls stay out of another window's tree");
      // Hiding the dialog drops its host, and its retained providers disconnect.
      Fl::awake([](void *data) { static_cast<Fl_Window *>(data)->hide(); }, &dialog);
      const auto hide_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (fixture.adaptor->hosted_windows_for_testing() != 1 &&
             std::chrono::steady_clock::now() < hide_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      expect(fixture.adaptor->hosted_windows_for_testing() == 1,
             "hiding the dialog drops its host");
      String retired;
      expect(FAILED(dialog_action->get_CurrentName(&retired.value)),
             "a closed dialog's retained provider disconnects");
      String still_there;
      ok(field->get_CurrentName(&still_there.value), "the main window keeps working");
      expect(still_there.text() == L"Profile name",
             "the main window's elements survive the dialog closing");
      fixture.close_from_client();
      const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!fixture.closed.load() && std::chrono::steady_clock::now() < close_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } catch (const std::exception &exception) { expect(false, exception.what()); }
    catch (...) { expect(false, "unexpected dialog exception"); }
    if (!fixture.closed.load()) fixture.close_from_client();
  });
  Fl::run();
  worker.join();
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(real_client_tree_and_text),
                 LAYERC_TEST_CASE(demand_drives_walking),
                 LAYERC_TEST_CASE(second_window_hosting))
