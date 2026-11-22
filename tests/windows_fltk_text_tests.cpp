#include "a11y/fltk_tree.hpp"
#include "a11y/input.hpp"
#include "a11y/uia/adaptor.hpp"
#include "a11y/uia/context.hpp"
#include "a11y/uia/text.hpp"
#include "accessible_widgets.hpp"
#include "windows_fltk_uia_text_client.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/platform.H>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using layerc::test::expect;
using layerc::editor::a11y::Bounds;
using layerc::editor::a11y::FltkTree;
using layerc::editor::a11y::Input;
using layerc::editor::a11y::Node;
using layerc::editor::a11y::Role;
using layerc::editor::a11y::Snapshot;
using layerc::editor::a11y::Text;
using layerc::editor::a11y::uia::Adaptor;
using layerc::editor::a11y::uia::Span;
using layerc::editor::accessibility::set_accessible_name;
using layerc::editor::accessibility::set_accessible_role;

using layerc::test::uia_text::Apartment;
using layerc::test::uia_text::Client;
using layerc::test::uia_text::Ref;
using layerc::test::uia_text::String;
using layerc::test::uia_text::document_of;
using layerc::test::uia_text::find;
using layerc::test::uia_text::ok;
using layerc::test::uia_text::range_text;
using layerc::test::uia_text::text_of;
using layerc::test::uia_text::wait_for;

// Text events only: this suite drives text and checks which text event arrives.
class TextEvents final : public IUIAutomationEventHandler {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAgileObject) ||
        IsEqualIID(iid, IID_IUIAutomationEventHandler))
      *result = static_cast<IUIAutomationEventHandler *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *,
                                                   EVENTID id) override {
    if (id == UIA_Text_TextChangedEventId) ++changes;
    if (id == UIA_Text_TextSelectionChangedEventId) ++selections;
    return S_OK;
  }
  std::atomic<int> changes{};
  std::atomic<int> selections{};
private:
  std::atomic<ULONG> references_{1};
};

struct Fixture {
  // Disposable component fixture: no profile store, daemon or production paths.
  Fl_Double_Window window{660, 300, "LayerE UIA text fixture"};
  Input single{20, 30, 320, 28, "Profile name"};
  Input empty_single{380, 30, 200, 28, "Empty profile"};
  Fl_Multiline_Input two_line{20, 80, 320, 56, "Notes"};
  Fl_Multiline_Input one_line{20, 160, 320, 28, "Tall notes"};
  Fl_Multiline_Input empty_multi{380, 80, 200, 56, "Empty notes"};
  Input long_single{20, 220, 200, 28, "Long line"};
  Fl_Multiline_Input lines{380, 160, 200, 80, "Lines"};
  FltkTree tree;
  std::unique_ptr<Adaptor> adaptor;
  std::atomic<bool> closed{};
  Fixture() {
    window.end();
    single.value("Alpha beta gamma");
    empty_single.value("");
    two_line.value("a\n");
    one_line.value("a\nb\n");
    empty_multi.value("");
    long_single.value("Short");
    lines.value("one\ntwo\nthree");
    const struct {
      Fl_Widget *widget;
      Role role;
      const char *name;
    } named[] = {
        {&single, Role::text_field, "Profile name"},
        {&empty_single, Role::text_field, "Empty profile"},
        {&two_line, Role::text_field, "Notes"},
        {&one_line, Role::text_field, "Tall notes"},
        {&empty_multi, Role::text_field, "Empty notes"},
        {&long_single, Role::text_field, "Long line"},
        {&lines, Role::text_field, "Lines"},
    };
    for (const auto &entry : named) {
      set_accessible_role(*entry.widget, entry.role);
      set_accessible_name(*entry.widget, entry.name);
    }
    window.show();
    Fl::check();
    adaptor = std::make_unique<Adaptor>(tree);
  }
  // Widget state belongs to the FLTK thread; the client asks it to do the work.
  void on_fltk(std::function<void()> work) {
    struct Step {
      std::function<void()> work;
      std::atomic<bool> done{};
    };
    auto step = std::make_shared<Step>();
    step->work = std::move(work);
    Fl::awake([](void *data) {
      auto *running = static_cast<Step *>(data);
      running->work();
      running->done.store(true);
    }, step.get());
    wait_for([&] { return step->done.load(); });
  }
  void close_from_client() {
    Fl::awake([](void *data) {
      auto &self = *static_cast<Fixture *>(data);
      self.window.hide();
      self.closed.store(true);
    }, this);
  }
};

// The retained collapsed range at the document end: its start endpoint moves
// onto the document end, so the widget's own caret may be elsewhere.
Ref<IUIAutomationTextRange> end_point(IUIAutomationTextPattern &text) {
  auto document = document_of(text);
  Ref<IUIAutomationTextRange> range;
  if (!document || !ok(document->Clone(range.put()), "clone the document")) return {};
  if (range)
    ok(range->MoveEndpointByRange(TextPatternRangeEndpoint_Start, document.get(),
                                  TextPatternRangeEndpoint_End),
       "collapse the retained range onto the document end");
  return range;
}

int compare(IUIAutomationTextRange &left, IUIAutomationTextRange &right,
            TextPatternRangeEndpoint endpoint) {
  int result{};
  ok(left.CompareEndpoints(endpoint, &right, endpoint, &result), "compare endpoints");
  return result;
}

// A client's view of the widget's own selection, and of its caret offset.
struct Selection {
  Ref<IUIAutomationTextRange> range;
  int start{-1};
  int end{-1};
};

Selection selection_of(IUIAutomationTextPattern &text) {
  Selection found;
  Ref<IUIAutomationTextRangeArray> array;
  if (FAILED(text.GetSelection(array.put())) || !array) return found;
  int length{};
  ok(array->get_Length(&length), "selection count");
  if (!expect(length == 1, "a single-selection field publishes one selection")) return found;
  if (FAILED(array->GetElement(0, found.range.put())) || !found.range) return found;
  auto document = document_of(text);
  if (!document) return found;
  // The document range starts at offset zero, so a comparison against its start
  // reads the absolute offset of either endpoint.
  int start{};
  int end{};
  ok(found.range->CompareEndpoints(TextPatternRangeEndpoint_Start, document.get(),
                                   TextPatternRangeEndpoint_Start, &start),
     "compare the selection start");
  ok(found.range->CompareEndpoints(TextPatternRangeEndpoint_End, document.get(),
                                   TextPatternRangeEndpoint_Start, &end),
     "compare the selection end");
  found.start = start;
  found.end = end;
  return found;
}

bool wait_caret(IUIAutomationTextPattern &text, int offset) {
  const bool reached = wait_for([&] {
    const auto selection = selection_of(text);
    return selection.start == offset && selection.end == offset;
  });
  return reached;
}

// --- synthetic insertion geometry -----------------------------------------

std::shared_ptr<layerc::editor::a11y::uia::Context> publish(HWND hwnd, Text text,
                                                            Bounds bounds) {
  auto context = std::make_shared<layerc::editor::a11y::uia::Context>(hwnd);
  context->root.store(1);
  Snapshot snapshot;
  snapshot.root = 1;
  Node root;
  root.id = 1;
  root.role = Role::window;
  root.children = {2};
  snapshot.nodes.emplace(1, root);
  Node field;
  field.id = 2;
  field.parent = 1;
  field.role = Role::text_field;
  field.name = "Synthetic field";
  field.bounds = bounds;
  field.text = std::move(text);
  snapshot.nodes.emplace(2, field);
  const auto error = context->model.publish(snapshot);
  if (!expect(!error, error ? error->message : "synthetic text snapshot publishes"))
    return {};
  return context;
}

HRESULT reveal(const std::shared_ptr<layerc::editor::a11y::uia::Context> &context,
               Span span) {
  Ref<ITextRangeProvider> range;
  *range.put() = layerc::editor::a11y::uia::make_range(context, 2, span);
  if (!expect(range.get() != nullptr, "synthetic range is available")) return E_FAIL;
  return range->ScrollIntoView(TRUE);
}

std::size_t rectangle_count(const std::shared_ptr<layerc::editor::a11y::uia::Context> &context,
                            Span span) {
  Ref<ITextRangeProvider> range;
  *range.put() = layerc::editor::a11y::uia::make_range(context, 2, span);
  if (!range) return 0;
  SAFEARRAY *boxes{};
  if (FAILED(range->GetBoundingRectangles(&boxes)) || boxes == nullptr) return 0;
  const auto count = static_cast<std::size_t>(boxes->rgsabound[0].cElements);
  SafeArrayDestroy(boxes);
  return count;
}
} // namespace

using namespace layerc::editor::a11y; // NOLINT: fixture types above are qualified.

namespace {

// --- case: the Text pattern as a real client sees it ----------------------

void text_contract(IUIAutomation &automation, IUIAutomationElement &root,
                   Fixture &fixture) {
  auto field = find(automation, root, L"Profile name");
  auto text = text_of(*field);
  if (!text) return;
  auto document = document_of(*text);
  if (!document) return;
  expect(range_text(*document) == L"Alpha beta gamma",
         "the document range reads the whole text");

  String bounded;
  ok(document->GetText(5, &bounded.value), "read a bounded text run");
  expect(bounded.text() == L"Alpha", "a bounded read stops at the requested length");

  // Move: a moved range becomes exactly one unit wide.
  Ref<IUIAutomationTextRange> unit_range;
  if (ok(document->Clone(unit_range.put()), "clone for unit movement") && unit_range) {
    int moved{};
    ok(unit_range->Move(TextUnit_Word, 1, &moved), "move a range by one word");
    expect(moved == 1, "one word is one move");
    expect(range_text(*unit_range) == L"beta ", "a word move lands on the next word");
    ok(unit_range->Move(TextUnit_Character, -2, &moved), "move back two characters");
    expect(moved == -2, "two characters are two moves");
    expect(range_text(*unit_range) == L"a", "a character move keeps one glyph");
    ok(unit_range->ExpandToEnclosingUnit(TextUnit_Word),
       "expand the range to its enclosing word");
    expect(range_text(*unit_range) == L"Alpha ",
           "the enclosing word covers the whole word");
    ok(unit_range->ExpandToEnclosingUnit(TextUnit_Line),
       "expand the range to its enclosing line");
    expect(range_text(*unit_range) == L"Alpha beta gamma",
           "a single-line field has one line");
    ok(unit_range->Move(TextUnit_Document, -1, &moved), "move to the document start");
    expect(range_text(*unit_range) == L"Alpha beta gamma",
           "the document unit reaches the whole document");
  }

  // MoveEndpointByUnit and CompareEndpoints.
  Ref<IUIAutomationTextRange> edges;
  if (ok(document->Clone(edges.put()), "clone for endpoint movement") && edges) {
    int moved{};
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Word, -1, &moved),
       "move the end endpoint back one word");
    expect(moved == -1, "one endpoint move is one word");
    expect(range_text(*edges) == L"Alpha beta ",
           "the end endpoint lands on the word boundary");
    expect(compare(*edges, *document, TextPatternRangeEndpoint_Start) == 0,
           "both ranges start at the document start");
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, 6,
                                 &moved),
       "move the start endpoint forward six characters");
    expect(range_text(*edges) == L"beta ", "the start endpoint lands on the word");
  }

  // FindText walks the document and answers one exact range.
  String needle(L"beta");
  Ref<IUIAutomationTextRange> found;
  if (ok(document->FindText(needle.value, FALSE, FALSE, found.put()), "find text") && found) {
    expect(range_text(*found) == L"beta", "FindText returns the matched text");
    expect(compare(*found, *document, TextPatternRangeEndpoint_Start) == 6,
           "FindText keeps the match offset");
  }
  String missing(L"delta");
  Ref<IUIAutomationTextRange> absent;
  ok(document->FindText(missing.value, FALSE, FALSE, absent.put()),
     "a missing match is not an error");
  expect(!absent, "a missing match answers no range");

  // Geometric rectangles come from the widget's own glyph geometry.
  SAFEARRAY *boxes{};
  if (ok(document->GetBoundingRectangles(&boxes), "document rectangles") && boxes) {
    expect(boxes->rgsabound[0].cElements >= 4,
           "a visible document publishes at least one rectangle");
    SafeArrayDestroy(boxes);
  }

  // Select / GetSelection round trip, then RemoveFromSelection.
  auto selection_range = end_point(*text);
  Ref<IUIAutomationTextRange> selected;
  if (ok(document->Clone(selected.put()), "clone for selection") && selected) {
    int moved{};
    ok(selected->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, -11,
                                    &moved),
       "select the first word");
    ok(selected->Select(), "select a range");
    const auto selection = selection_of(*text);
    expect(selection.start == 0 && selection.end == 5,
           "the selection round-trips both endpoints");
    if (selection.range) {
      expect(range_text(*selection.range) == L"Alpha", "the selection reads its own text");
      ok(selection.range->RemoveFromSelection(), "remove the selection");
    }
    const bool collapsed = wait_for([&] {
      const auto after = selection_of(*text);
      return after.start == after.end;
    });
    expect(collapsed, "removing the whole selection collapses it");
  }

  // Text events: a committed edit and a selection change both reach the client.
  Ref<TextEvents> events;
  *events.put() = new TextEvents;
  struct Unsubscribe {
    IUIAutomation *automation;
    ~Unsubscribe() { automation->RemoveAllEventHandlers(); }
  } unsubscribe{&automation};
  for (const auto id : {UIA_Text_TextChangedEventId, UIA_Text_TextSelectionChangedEventId})
    ok(automation.AddAutomationEventHandler(id, field.get(), TreeScope_Element, nullptr,
                                            events.get()),
       "subscribe a text event");
  Ref<IUIAutomationValuePattern> value;
  if (ok(field->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                    reinterpret_cast<void **>(value.put())),
         "Value pattern for a text edit") &&
      value) {
    String wider(L"Alpha beta delta");
    ok(value->SetValue(wider.value), "edit the field through UIA");
  }
  fixture.on_fltk([&] { fixture.single.insert_position(5); fixture.single.mark(5); });
  const bool arrived = wait_for([&] {
    return events->changes.load() > 0 && events->selections.load() > 0;
  });
  expect(arrived, "text and selection changes reach the client");
}

// --- case: ITextProvider2 ------------------------------------------------

void text_pattern2(IUIAutomation &automation, IUIAutomationElement &root,
                   Fixture &fixture) {
  auto field = find(automation, root, L"Profile name");
  Ref<IUIAutomationTextPattern2> text;
  if (!ok(field->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                     reinterpret_cast<void **>(text.put())),
          "TextPattern2 is answered by the text pattern object") ||
      !text)
    return;
  Ref<IUIAutomationTextPattern> base;
  expect(SUCCEEDED(text->QueryInterface(IID_IUIAutomationTextPattern,
                                        reinterpret_cast<void **>(base.put()))) && base,
         "TextPattern2 also serves the base Text pattern");

  fixture.single.take_focus();
  fixture.on_fltk([&] {
    fixture.single.insert_position(6);
    fixture.single.mark(6);
    fixture.single.take_focus();
  });
  auto base_text = text_of(*field);
  if (base_text) expect(wait_caret(*base_text, 6), "the widget caret reaches UIA");
  BOOL active{};
  Ref<IUIAutomationTextRange> caret;
  if (ok(text->GetCaretRange(&active, caret.put()), "read the caret range") && caret) {
    expect(active != FALSE, "a focused field has an active caret range");
    expect(range_text(*caret).empty(), "the caret range is degenerate");
    auto document = document_of(*base_text);
    if (document)
      expect(compare(*caret, *document, TextPatternRangeEndpoint_Start) == 6,
             "the caret range sits at the widget caret");
  }

  auto other = find(automation, root, L"Long line");
  if (ok(other->SetFocus(), "focus another field")) {
    Ref<IUIAutomationTextRange> unfocused;
    BOOL still_active{};
    if (ok(text->GetCaretRange(&still_active, unfocused.put()),
           "read the caret range without focus")) {
      expect(wait_for([&] {
               BOOL reading{};
               Ref<IUIAutomationTextRange> ignored;
               return SUCCEEDED(text->GetCaretRange(&reading, ignored.put())) &&
                      reading == FALSE;
             }),
             "an unfocused field has no active caret range");
    }
  }

  Ref<IUIAutomationTextRange> annotation;
  const HRESULT refused = text->RangeFromAnnotation(field.get(), annotation.put());
  expect(FAILED(refused), "no annotation range is published");
  if (FAILED(refused)) expect(!annotation, "a refused annotation answers no range");
}

// --- case: insertion geometry (synthetic exact snapshots) -----------------

void insertion_geometry(HWND hwnd) {
  // An empty document uses its own caret geometry like any other caret.
  {
    Text text;
    text.value = "";
    text.caret_bounds = {5, 5, 1, 10};
    auto context = publish(hwnd, text, {0, 0, 100, 20});
    if (context)
      expect(reveal(context, {0, 0}) == S_OK,
             "an empty document reveals its caret inside the viewport");
  }
  // Even an empty document refuses a caret its viewport does not hold: the
  // early "any empty text is visible" answer is gone.
  {
    Text text;
    text.value = "";
    text.caret_bounds = {5, 5, 1, 10};
    auto context = publish(hwnd, text, {0, 0, 0, 0});
    if (context)
      expect(reveal(context, {0, 0}) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
             "an empty document with an offscreen caret is not revealed");
  }
  // A caret exactly on the viewport's right edge is inside it: zero width.
  {
    Text text;
    text.value = "abc";
    text.caret = 3;
    text.anchor = 3;
    text.runs = {{0, 1, {0, 0, 8, 10}}, {1, 2, {8, 0, 8, 10}}, {2, 3, {16, 0, 8, 10}}};
    text.caret_bounds = {24, 0, 1, 10};
    auto context = publish(hwnd, text, {0, 0, 24, 10});
    if (context)
      expect(reveal(context, {3, 3}) == S_OK,
             "a caret on the viewport's right edge is visible");
  }
  // A multiline newline ends its line: the insertion after it is on the next
  // line, which the newline's own box cannot describe.
  {
    Text text;
    text.value = "a\n";
    text.caret = 0;
    text.anchor = 0;
    text.multiline = true;
    text.runs = {{0, 1, {0, 0, 8, 10}}, {1, 2, {8, 0, 1, 10}}};
    text.caret_bounds = {0, 0, 1, 10};
    auto context = publish(hwnd, text, {0, 0, 100, 20});
    if (context) {
      expect(reveal(context, {2, 2}) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
             "an insertion after a trailing newline is not claimed as revealed");
      expect(rectangle_count(context, {2, 2}) == 0,
             "an insertion after a trailing newline has no rectangle");
    }
    auto offscreen = publish(hwnd, text, {0, 0, 100, 10});
    if (offscreen)
      expect(reveal(offscreen, {2, 2}) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
             "an insertion whose next line is offscreen is not revealed");
  }
  // The same document with the caret itself after the newline: the widget's own
  // caret geometry describes the next line, so it is visible when that line is.
  {
    Text text;
    text.value = "a\n";
    text.caret = 2;
    text.anchor = 2;
    text.multiline = true;
    text.runs = {{0, 1, {0, 0, 8, 10}}, {1, 2, {8, 0, 1, 10}}};
    text.caret_bounds = {0, 10, 1, 10};
    auto context = publish(hwnd, text, {0, 10, 100, 10});
    if (context) {
      expect(reveal(context, {2, 2}) == S_OK,
             "a trailing-newline caret on the visible next line is revealed");
      expect(rectangle_count(context, {2, 2}) == 4,
             "a visible caret publishes exactly one rectangle");
    }
    auto scrolled = publish(hwnd, text, {0, 0, 100, 10});
    if (scrolled)
      expect(reveal(scrolled, {2, 2}) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
             "a trailing-newline caret above the viewport is not revealed");
  }
  // A retained range at the document start of a horizontally scrolled field.
  {
    Text text;
    text.value = "0123456789";
    text.caret = 10;
    text.anchor = 10;
    text.runs = {{0, 1, {-40, 0, 8, 10}}, {1, 2, {-32, 0, 8, 10}}, {9, 10, {24, 0, 8, 10}}};
    text.caret_bounds = {32, 0, 1, 10};
    auto context = publish(hwnd, text, {0, 0, 50, 10});
    if (context)
      expect(reveal(context, {0, 0}) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
             "an insertion left of a horizontally scrolled viewport is refused");
  }
}

// --- case: trailing-newline insertion through a real client ---------------

void trailing_newline(IUIAutomation &automation, IUIAutomationElement &root,
                      Fixture &fixture) {
  // Two lines tall with "a\n": the caret on line 1 while a retained range names
  // the document end, which is on line 2.
  auto notes = find(automation, root, L"Notes");
  auto notes_text = text_of(*notes);
  if (!notes_text) return;
  fixture.on_fltk([&] {
    fixture.two_line.insert_position(0);
    fixture.two_line.mark(0);
  });
  expect(wait_caret(*notes_text, 0), "the notes caret sits on the first line");
  auto retained = end_point(*notes_text);
  if (retained) {
    expect(retained->ScrollIntoView(TRUE) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
           "a retained end range is not revealed while the caret is on line 1");
    fixture.on_fltk([&] { fixture.two_line.insert_position(2); fixture.two_line.mark(2); });
    expect(wait_caret(*notes_text, 2), "the notes caret moves to the end");
    auto caret_end = end_point(*notes_text);
    if (caret_end)
      expect(caret_end->ScrollIntoView(TRUE) == S_OK,
             "the end insertion is revealed when the caret is there");
  }

  // One line tall with "a\nb\n": line 2 and the document end are offscreen.
  auto tall = find(automation, root, L"Tall notes");
  auto tall_text = text_of(*tall);
  if (!tall_text) return;
  fixture.on_fltk([&] {
    fixture.one_line.insert_position(0);
    fixture.one_line.mark(0);
  });
  expect(wait_caret(*tall_text, 0), "the tall notes caret stays on line 1");
  auto hidden = end_point(*tall_text);
  if (hidden) {
    expect(hidden->ScrollIntoView(TRUE) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
           "an end insertion below a one-line viewport is not revealed");
    int before{};
    fixture.on_fltk([&] { before = fixture.one_line.insert_position(); });
    expect(before == 0, "a refused reveal leaves the caret untouched");
  }
  fixture.on_fltk([&] { fixture.one_line.insert_position(4); fixture.one_line.mark(4); });
  expect(wait_caret(*tall_text, 4), "the tall notes caret moves to the last line");
  auto visible_end = end_point(*tall_text);
  if (visible_end)
    expect(visible_end->ScrollIntoView(TRUE) == S_OK,
           "the end insertion is revealed once its line is shown");

  // Empty documents have no glyph to describe but still publish their caret.
  for (const wchar_t *name : {L"Empty profile", L"Empty notes"}) {
    auto empty = find(automation, root, name);
    auto empty_text = text_of(*empty);
    if (!empty_text) continue;
    auto document = document_of(*empty_text);
    if (!document) continue;
    expect(range_text(*document).empty(), "an empty document reads no text");
    Ref<IUIAutomationTextRange> caret;
    if (ok(document->Clone(caret.put()), "clone the empty document") && caret)
      expect(caret->ScrollIntoView(TRUE) == S_OK,
             "an empty document reveals its own caret");
  }

  // A horizontally scrolled field refuses the collapsed range at offset 0.
  auto line = find(automation, root, L"Long line");
  auto line_text = text_of(*line);
  if (!line_text) return;
  Ref<IUIAutomationValuePattern> value;
  if (ok(line->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                   reinterpret_cast<void **>(value.put())),
         "Value pattern for the long line") &&
      value) {
    std::wstring wide;
    for (int count = 0; count < 24; ++count) wide += L"0123456789";
    String widened(wide.c_str());
    ok(value->SetValue(widened.value), "fill the long line");
  }
  fixture.on_fltk([&] {
    fixture.long_single.insert_position(fixture.long_single.size());
    fixture.long_single.mark(fixture.long_single.size());
  });
  auto line_document = document_of(*line_text);
  if (!line_document) return;
  // FLTK scrolls its viewport around the caret when it draws, and the adaptor
  // republishes geometry from its own check callback: poll the hit test.
  RECT bounds{};
  if (!ok(line->get_CurrentBoundingRectangle(&bounds), "field bounds")) return;
  int left_edge{};
  const bool scrolled = wait_for([&] {
    Ref<IUIAutomationTextRange> hit;
    if (FAILED(line_text->RangeFromPoint({bounds.left + 2, (bounds.top + bounds.bottom) / 2},
                                         hit.put())) ||
        !hit)
      return false;
    left_edge = compare(*hit, *line_document, TextPatternRangeEndpoint_Start);
    return left_edge > 0;
  });
  expect(scrolled, "the long line is scrolled away from its start");
  Ref<IUIAutomationTextRange> beginning;
  if (ok(line_document->Clone(beginning.put()), "clone the long line") && beginning) {
    int moved{};
    ok(beginning->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Document, -1,
                                     &moved),
       "collapse the long line to its start");
    expect(beginning->ScrollIntoView(TRUE) == static_cast<HRESULT>(UIA_E_NOTSUPPORTED),
           "an offscreen insertion point is not reported as revealed");
  }
}

// --- case: line units over a three-line field -----------------------------

// "one\ntwo\nthree" has lines starting at 0, 4 and 8; the last has no newline.
void line_moves(IUIAutomation &automation, IUIAutomationElement &root) {
  auto field = find(automation, root, L"Lines");
  if (!field) return;
  auto text = text_of(*field);
  if (!text) return;
  auto document = document_of(*text);
  if (!document) return;
  expect(range_text(*document) == L"one\ntwo\nthree", "the three-line document reads back");

  Ref<IUIAutomationTextRange> line;
  if (ok(document->Clone(line.put()), "clone for line movement") && line) {
    int moved{};
    ok(line->Move(TextUnit_Line, 1, &moved), "move a range by one line");
    expect(moved == 1, "one line is one move");
    expect(range_text(*line) == L"two\n", "a line move lands on the second line");
    ok(line->Move(TextUnit_Line, 5, &moved), "move past the last line");
    expect(moved == 1, "a move stops at the last line");
    expect(range_text(*line) == L"three", "the last line has no newline");
    ok(line->Move(TextUnit_Line, 1, &moved), "move forward from the last line");
    expect(moved == 0, "no line follows the last line");
    expect(range_text(*line) == L"three", "a refused move keeps the last line");
    ok(line->Move(TextUnit_Line, -5, &moved), "move back past the first line");
    expect(moved == -2, "a backward move stops at the first line");
    expect(range_text(*line) == L"one\n", "the first line keeps its newline");
    ok(line->Move(TextUnit_Line, -1, &moved), "move back from the first line");
    expect(moved == 0, "no line precedes the first line");
  }

  Ref<IUIAutomationTextRange> edges;
  if (ok(document->Clone(edges.put()), "clone for line endpoints") && edges) {
    int moved{};
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Line, -1, &moved),
       "move the end back one line");
    expect(moved == -1 && range_text(*edges) == L"one\ntwo\n",
           "the end endpoint lands on the last line's start");
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Line, 1, &moved),
       "move the start forward one line");
    expect(moved == 1 && range_text(*edges) == L"two\n",
           "the start endpoint lands on the second line");
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Line, 5, &moved),
       "move the end past the document end");
    expect(moved == 1 && range_text(*edges) == L"two\nthree",
           "the end endpoint stops at the document end");
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Line, -5, &moved),
       "move the start before the document start");
    expect(moved == -1 && range_text(*edges) == L"one\ntwo\nthree",
           "the start endpoint stops at the document start");
    ok(edges->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Line, 5, &moved),
       "move the start past the end");
    expect(moved == 3 && range_text(*edges).empty(),
           "a start moved past the end collapses the range at the document end");
  }

  // A collapsed range moves between line starts and expands to its line.
  Ref<IUIAutomationTextRange> point;
  if (ok(document->Clone(point.put()), "clone for a collapsed line move") && point &&
      ok(point->MoveEndpointByRange(TextPatternRangeEndpoint_End, document.get(),
                                    TextPatternRangeEndpoint_Start),
         "collapse onto the document start")) {
    int moved{};
    ok(point->Move(TextUnit_Line, 2, &moved), "move a collapsed range by two lines");
    expect(moved == 2 && range_text(*point).empty(),
           "a collapsed range stays collapsed across lines");
    expect(compare(*point, *document, TextPatternRangeEndpoint_Start) == 8,
           "a collapsed range lands on the last line's start");
    ok(point->ExpandToEnclosingUnit(TextUnit_Line), "expand onto the enclosing line");
    expect(range_text(*point) == L"three", "the enclosing last line has no newline");
  }
}

// --- case: selection kind and embedded children ---------------------------

void selection_kind_and_children(IUIAutomation &automation, IUIAutomationElement &root) {
  auto field = find(automation, root, L"Lines");
  if (!field) return;
  auto text = text_of(*field);
  if (!text) return;
  SupportedTextSelection kind{SupportedTextSelection_None};
  if (ok(text->get_SupportedTextSelection(&kind), "read the supported selection"))
    expect(kind == SupportedTextSelection_Single,
           "a field supports one contiguous selection");
  // A plain input embeds no child objects, so no element maps to a range in it.
  auto other = find(automation, root, L"Profile name");
  for (IUIAutomationElement *element : {field.get(), other.get()}) {
    if (element == nullptr) continue;
    Ref<IUIAutomationTextRange> range;
    const HRESULT refused = text->RangeFromChild(element, range.put());
    expect(FAILED(refused) && !range, "no element maps to an embedded child range");
  }
}

// --- case: the invisible system caret follows the focused field ------------

struct SystemCaret {
  HWND owner{};
  RECT screen{};
};

// What Windows itself reports for the FLTK thread's caret, in screen pixels.
SystemCaret system_caret_of(const DWORD thread) {
  GUITHREADINFO info{};
  info.cbSize = sizeof(info);
  if (!GetGUIThreadInfo(thread, &info) || info.hwndCaret == nullptr) return {};
  POINT corners[2] = {{info.rcCaret.left, info.rcCaret.top},
                      {info.rcCaret.right, info.rcCaret.bottom}};
  if (!ClientToScreen(info.hwndCaret, &corners[0]) ||
      !ClientToScreen(info.hwndCaret, &corners[1]))
    return {};
  return {info.hwndCaret, {corners[0].x, corners[0].y, corners[1].x, corners[1].y}};
}

// The active caret range's insertion rectangle, as the client reads it.
std::optional<RECT> caret_box(IUIAutomationTextPattern2 &text) {
  BOOL active{};
  Ref<IUIAutomationTextRange> caret;
  if (FAILED(text.GetCaretRange(&active, caret.put())) || !caret || active == FALSE)
    return std::nullopt;
  SAFEARRAY *boxes{};
  if (FAILED(caret->GetBoundingRectangles(&boxes)) || boxes == nullptr) return std::nullopt;
  std::optional<RECT> found;
  double *values{};
  if (boxes->rgsabound[0].cElements >= 4 &&
      SUCCEEDED(SafeArrayAccessData(boxes, reinterpret_cast<void **>(&values)))) {
    found = RECT{std::lround(values[0]), std::lround(values[1]),
                 std::lround(values[0] + values[2]), std::lround(values[1] + values[3])};
    SafeArrayUnaccessData(boxes);
  }
  SafeArrayDestroy(boxes);
  return found;
}

// Polls until the system caret is on `owner` at the caret range's position.
bool caret_follows(IUIAutomationTextPattern2 &text, const DWORD thread, const HWND owner,
                   RECT *seen, const std::optional<LONG> below = std::nullopt) {
  return wait_for([&] {
    const auto box = caret_box(text);
    if (!box) return false;
    const auto caret = system_caret_of(thread);
    const auto height = caret.screen.bottom - caret.screen.top;
    // A caret still on the previous line is a publish that has not caught up.
    if (below && caret.screen.top <= *below) return false;
    if (caret.owner != owner || std::abs(caret.screen.left - box->left) > 1 ||
        std::abs(caret.screen.top - box->top) > 1 || height <= 0 ||
        std::abs(height - (box->bottom - box->top)) > 1)
      return false;
    if (seen != nullptr) *seen = caret.screen;
    return true;
  });
}

Ref<IUIAutomationTextPattern2> text2_of(IUIAutomationElement *element) {
  Ref<IUIAutomationTextPattern2> text;
  if (element != nullptr)
    ok(element->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                    reinterpret_cast<void **>(text.put())),
       "TextPattern2 for the caret field");
  return text;
}

void system_caret(IUIAutomation &automation, IUIAutomationElement &root, Fixture &fixture) {
  const HWND hwnd = fl_xid(&fixture.window);
  const DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
  auto field = find(automation, root, L"Lines");
  auto text = text2_of(field.get());
  if (!text) return;

  fixture.on_fltk([&] {
    fixture.lines.insert_position(5);
    fixture.lines.mark(5);
    fixture.lines.take_focus();
  });
  RECT first{};
  expect(caret_follows(*text, thread, hwnd, &first),
         "the system caret sits at the focused field's caret");
  fixture.on_fltk([&] {
    fixture.lines.insert_position(10);
    fixture.lines.mark(10);
  });
  expect(caret_follows(*text, thread, hwnd, nullptr, first.top),
         "the system caret follows the caret onto a later line");

  fixture.on_fltk([] { Fl::focus(nullptr); });
  expect(wait_for([&] {
           BOOL active{};
           Ref<IUIAutomationTextRange> ignored;
           (void)text->GetCaretRange(&active, ignored.put());
           return system_caret_of(thread).owner == nullptr;
         }),
         "the system caret is destroyed when no field has focus");

  // A second top-level window owns the caret while its field has focus, and
  // closing that window leaves no caret on it.
  std::unique_ptr<Fl_Double_Window> extra;
  Input *extra_field{};
  HWND extra_handle{};
  fixture.on_fltk([&] {
    extra = std::make_unique<Fl_Double_Window>(360, 90, "LayerE UIA caret window");
    extra_field = new Input(20, 30, 300, 28, "Caret field");
    extra->end();
    set_accessible_role(*extra_field, Role::text_field);
    set_accessible_name(*extra_field, "Caret field");
    extra_field->value("caret");
    extra->show();
    extra_handle = fl_xid(extra.get());
  });
  Ref<IUIAutomationElement> extra_root;
  if (expect(extra_handle != nullptr, "the second caret window has a handle") &&
      ok(automation.ElementFromHandle(extra_handle, extra_root.put()),
         "client finds the second window") &&
      extra_root) {
    auto extra_element = find(automation, *extra_root, L"Caret field");
    auto extra_text = text2_of(extra_element.get());
    fixture.on_fltk([&] {
      extra_field->insert_position(2);
      extra_field->mark(2);
      extra_field->take_focus();
    });
    if (extra_text)
      expect(caret_follows(*extra_text, thread, extra_handle, nullptr),
             "the system caret moves to the focused field's own window");
    fixture.on_fltk([&] { extra->hide(); });
    expect(wait_for([&] { return system_caret_of(thread).owner != extra_handle; }),
           "closing the caret's window leaves no caret on it");
  }
  fixture.on_fltk([&] { extra.reset(); });
}
} // namespace

int main(int argc, char **argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  const Apartment apartment{COINIT_APARTMENTTHREADED};
  if (!ok(apartment.result, "initialize text fixture STA")) return 1;
  Fl::lock();
  Fixture fixture;
  std::thread worker([&] {
    Client client;
    if (client) {
      auto root = client.root(fl_xid(&fixture.window));
      if (root) {
        const auto run = [&](const char *name, auto &&case_body) {
          if (!only.empty() && only != name) return;
          std::cerr << "CASE " << name << '\n';
          const int before = layerc::test::failures;
          case_body();
          if (layerc::test::failures != before) std::cerr << "CASE FAILED: " << name << '\n';
        };
        run("text_contract", [&] { text_contract(*client.automation.get(), *root.get(), fixture); });
        run("text_pattern2", [&] { text_pattern2(*client.automation.get(), *root.get(), fixture); });
        run("insertion_geometry", [&] { insertion_geometry(fl_xid(&fixture.window)); });
        run("trailing_newline", [&] { trailing_newline(*client.automation.get(), *root.get(), fixture); });
        run("line_moves", [&] { line_moves(*client.automation.get(), *root.get()); });
        run("selection_kind_and_children",
            [&] { selection_kind_and_children(*client.automation.get(), *root.get()); });
        // Last: it opens and closes a second top-level window.
        run("system_caret", [&] { system_caret(*client.automation.get(), *root.get(), fixture); });
      }
    }
    fixture.close_from_client();
  });
  Fl::run();
  worker.join();
  return layerc::test::failures == 0 ? 0 : 1;
}
