// What a screen reader reads from the editor's FLTK widgets: the tree rows it
// browses, the text it hears, and the announcements the editor raises. Every
// case talks AT-SPI over D-Bus to the editor's own adaptor, the same path a real
// assistive technology takes.
#include "a11y/atspi/adaptor.hpp"
#include "a11y/fltk_semantics.hpp"
#include "a11y/fltk_tree.hpp"
#include "accessible_widgets.hpp"
#include "editor_accessibility.hpp"
#include "editor_app.hpp"
#include "editor_commands.hpp"
#include "editor_popup.hpp"
#include "linux_atspi_test_fixture.hpp"
#include "linux_editor_test_access.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

namespace a11y = layerc::editor::a11y;
namespace atspi = layerc::editor::a11y::atspi;
namespace editor = layerc::editor;
using layerc::test::expect;
using namespace layerc::test::atspi_fixture;

// --- talking to the editor's own connection --------------------------------

// The container interface the row cases read selection through; the harness
// names only the accessible interface.
constexpr const char *kSelectionInterface = "org.a11y.atspi.Selection";
// The reference a container answers for a slot it does not fill.
constexpr const char *kNullPath = "/org/a11y/atspi/null";

// A method call carrying one integer argument.
CallResult with_index(Harness &harness, const std::string &path,
                      const std::string &interface, const std::string &member,
                      const int index) {
  return harness.ask(harness.embed_name(), path, interface, member,
                     [index](sd_bus_message *call) {
                       return sd_bus_message_append(call, "i", index);
                     });
}

// A boolean answer, read the way its reply lays it out.
std::optional<bool> reply_bool(sd_bus_message *message) {
  if (!message) return std::nullopt;
  int value = 0;
  if (sd_bus_message_read(message, "b", &value) < 0) return std::nullopt;
  return value != 0;
}

// The children of a node, as a client reads them.
std::vector<Reference> children(Harness &harness, const std::string &path) {
  return reply_references(
      harness.get(path, kAccessibleInterface, "GetChildren").reply);
}

// The chosen children, as a client reads them: one GetSelectedChild per slot
// until the container reports none.
std::vector<std::string> selected_children(Harness &harness,
                                           const std::string &path) {
  std::vector<std::string> selected;
  for (int index = 0; index < 8; ++index) {
    const auto child = reply_reference(
        with_index(harness, path, kSelectionInterface, "GetSelectedChild", index)
            .reply);
    if (!child || child->path.empty() || child->path == kNullPath) break;
    selected.push_back(child->path);
  }
  return selected;
}

// The node named `name`, breadth first from the application root: a client
// finds nodes by what they say, not by a path it guessed.
std::optional<std::string> named(Harness &harness, const std::string_view name) {
  std::vector<std::string> queue{std::string{atspi::kRootPath}};
  for (std::size_t i = 0; i < queue.size(); ++i) {
    const auto path = queue[i];
    const auto label =
        variant_text(harness.property(path, kAccessibleInterface, "Name").reply);
    if (label && *label == name) return path;
    for (const auto &child : children(harness, path)) queue.push_back(child.path);
  }
  return std::nullopt;
}

std::optional<std::string> text_property(Harness &harness, const std::string &path,
                                         const char *name) {
  return variant_text(harness.property(path, kAccessibleInterface, name).reply);
}

// A session where the harness answers for org.a11y.Bus and the registry, the
// editor's adaptor connects to it, and nothing is left on a real desktop bus.
struct Session {
  layerc::test::ScopedEnvironment backend{"FLTK_BACKEND",
                                          std::filesystem::path("x11")};
  layerc::test::ScopedEnvironment accessibility{"AT_SPI_BUS_ADDRESS",
                                                std::nullopt};

  Session() {
    layerc::test::require_x11();
    require_session_bus();
  }
};

bool enabled(Harness &harness) {
  harness.set_flags(true, false);
  return harness.spin_until([&] { return harness.embeds() > 0; }, ms(3000));
}

// --- the portable text repair ---------------------------------------------

// Generated once from GLib's g_utf8_make_valid over these inputs; the vectors
// are the legacy contract and no GLib stays in this suite.
constexpr std::pair<std::string_view, std::string_view> kUtf8Vectors[] = {
    {"", ""},
    {"Save", "Save"},
    {"\xe2\x82\xac", "\xe2\x82\xac"},
    {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"},
    {"\xe0\x80\x80", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
    {"\xed\xa0\x80", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
    {"\xf0\x80\x80\x80", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
    {"\xf4\x90\x80\x80", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
    {"\xf0\x9f\x98", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
    {"\xe2\x82", "\xef\xbf\xbd\xef\xbf\xbd"},
    {"\x80", "\xef\xbf\xbd"},
    {"\xc3", "\xef\xbf\xbd"},
    {"a\xc3\xa9" "b", "a\xc3\xa9" "b"},
    {"\xff\xfe", "\xef\xbf\xbd\xef\xbf\xbd"},
    {"caf\xe9", "caf\xef\xbf\xbd"},
    {"\xe2\x82\xac\xe2", "\xe2\x82\xac\xef\xbf\xbd"},
    {"\xf0\x9f\x98\x80\x80", "\xf0\x9f\x98\x80\xef\xbf\xbd"},
};

// The portable reader must preserve the old bridge's text, including invalid
// file-name bytes and embedded NULs.
void portable_utf8_repair_matches_legacy() {
  bool all_match = true;
  for (const auto &[input, expected] : kUtf8Vectors) {
    if (a11y::valid_utf8(std::string{input}) != expected) {
      all_match = false;
      break;
    }
  }
  expect(all_match, "portable UTF-8 repair matches the recorded legacy text");
  expect(a11y::valid_utf8(std::string("A\0B", 3)) == "A\xef\xbf\xbd" "B",
         "portable UTF-8 repair replaces embedded NULs");
  // Repair is idempotent and its output is always valid: both properties a
  // client relies on when it prints what it received.
  bool stable = true;
  for (const auto &[input, expected] : kUtf8Vectors) {
    const std::string repaired = a11y::valid_utf8(std::string{input});
    stable = stable && a11y::valid_utf8(repaired) == repaired;
  }
  expect(stable, "repair is idempotent over the recorded vectors");
  // Every one- and two-byte sequence still yields valid UTF-8, the property the
  // exhaustive legacy comparison stood for.
  bool valid = true;
  for (unsigned first = 0; first < 256 && valid; ++first) {
    std::string text(1, static_cast<char>(first));
    valid = a11y::valid_utf8(text) == a11y::valid_utf8(a11y::valid_utf8(text));
    for (unsigned second = 0; second < 256 && valid; ++second) {
      text.resize(2);
      text[1] = static_cast<char>(second);
      valid = a11y::valid_utf8(text) == a11y::valid_utf8(a11y::valid_utf8(text));
    }
  }
  expect(valid, "every one- and two-byte sequence repairs to a stable text");
}
} // namespace

// A plain Fl_Hold_Browser keeps owner bounds and lazy row lifetime: a hidden
// list exports no rows, a shown one exports every row, and hiding it retires
// them. ContextBrowser geometry: linux_editor_semantic_row_extents.
void linux_editor_accessibility_tree_rows() {
  Session session;
  Harness harness;
  require_private_bus(harness);
  Fl_Window window(440, 320);
  Fl_Tabs tabs(0, 0, 440, 320);
  Fl_Group profile(0, 25, 440, 295, "Profile");
  profile.end();
  Fl_Group panels(0, 25, 440, 295, "Panels");
  Fl_Hold_Browser list(10, 60, 400, 200, "Panel list");
  for (int i = 1; i <= 5843; ++i) list.add(("Panel " + std::to_string(i)).c_str());
  panels.end();
  tabs.end();
  window.end();
  tabs.value(&profile);
  list.value(2500);
  window.show();
  Fl::check();
  {
    a11y::FltkTree tree;
    atspi::Adaptor adaptor{tree};
    if (!expect(enabled(harness), "a consumer enables the adaptor")) return;
    harness.spin(ms(200));
    const auto owner = named(harness, "Panel list");
    expect(!list.visible_r(), "browser is hidden by its parent tab");
    expect(owner.has_value() && children(harness, *owner).empty(),
           "a hidden browser exports no row objects");
    tabs.value(&panels);
    harness.spin(ms(300));
    expect(list.visible_r(), "panel browser is now showing");
    const auto shown = named(harness, "Panel list");
    if (!expect(shown.has_value(), "showing browser is exported")) return;
    const auto rows = children(harness, *shown);
    expect(rows.size() == 5843,
           "showing browser exports every row, including offscreen rows");
    if (!expect(rows.size() == 5843, "row count is usable")) return;
    const auto row = rows[2499].path;
    const auto last = rows[5842].path;
    expect(text_property(harness, row, "Name") == "Panel 2500",
           "row text preserved");
    expect(text_property(harness, last, "Name") == "Panel 5843",
           "offscreen final row preserved");
    expect(reply_text(harness.get(row, kAccessibleInterface, "GetRoleName").reply) ==
               "list item",
           "row role preserved");
    // As in the legacy ATK test, ask the row for its own parent rather than
    // making a round trip for each of the 5,843 rows to infer parentage.
    const auto parent = reply_variant_reference(
        harness.property(row, kAccessibleInterface, "Parent").reply);
    expect(parent && parent->name == harness.embed_name() && parent->path == *shown,
           "row parent preserved");
    const auto index =
        reply_i32(harness.get(row, kAccessibleInterface, "GetIndexInParent").reply);
    expect(index && *index == 2499, "row index preserved");
    expect(selected_children(harness, *shown) == std::vector<std::string>{row},
           "selection count exposed");
    expect(reply_bool(with_index(harness, *shown, kSelectionInterface,
                                 "IsChildSelected", 2499)
                          .reply) == true,
           "the selected row answers IsChildSelected");
    expect(reply_bool(with_index(harness, *shown, kSelectionInterface,
                                 "IsChildSelected", 0)
                          .reply) == false,
           "an unselected child does not");
    harness.spin(ms(200));
    const auto stable = children(harness, *shown);
    expect(stable.size() == 5843 && stable[2499].path == row,
           "unchanged showing rows keep their object path");
    tabs.value(&profile);
    harness.spin(ms(300));
    const auto hidden_rows = children(harness, *shown);
    expect(hidden_rows.empty(), "hidden rows are removed");
    expect(harness.property(row, kAccessibleInterface, "Parent").error ==
               "org.freedesktop.DBus.Error.UnknownObject",
           "a removed row no longer has a parent");
    expect(harness.get(row, "org.a11y.atspi.Action", "DoAction")
               .error == "org.freedesktop.DBus.Error.UnknownObject",
           "a removed row rejects its action");
    expect(list.size() == 5843 && list.value() == 2500,
           "hiding accessibility rows does not mutate the list or its selection");
    tabs.value(&panels);
    harness.spin(ms(300));
    const auto restored = children(harness, *shown);
    if (!expect(restored.size() == 5843, "row returns after tab reactivation")) return;
    expect(restored[2499].path != row,
           "reactivation creates live rows without reviving stale references");
    expect(selected_children(harness, *shown) ==
               std::vector<std::string>{restored[2499].path},
           "original selection returns after reactivation");
    tabs.value(&profile);
    harness.spin(ms(200));
    list.clear();
    list.add("Replacement");
    list.value(1);
    harness.spin(ms(300));
    expect(children(harness, *shown).empty(), "hidden list rebuild stays lazy");
    tabs.value(&panels);
    harness.spin(ms(300));
    const auto rebuilt = children(harness, *shown);
    if (!expect(rebuilt.size() == 1, "rebuilt row is reachable")) return;
    expect(text_property(harness, rebuilt[0].path, "Name") == "Replacement",
           "hidden rebuild restores correct content");
    expect(harness.property(restored[2499].path, kAccessibleInterface, "Name").error ==
               "org.freedesktop.DBus.Error.UnknownObject",
           "hidden rebuild cannot retarget an old row");
    list.hide();
    harness.spin(ms(300));
    expect(children(harness, *shown).empty(),
           "directly hiding the browser also retires its rows");
    list.show();
    harness.spin(ms(300));
    expect(children(harness, *shown).size() == 1,
           "directly showing the browser recreates its rows");
  }
  window.hide();
  Fl::check();
}

// The line that belongs to a control is its description, and a wordless button
// already says its tooltip as its name (DesignSystem.md, Screen readers).
void linux_editor_accessibility_descriptions() {
  portable_utf8_repair_matches_legacy();
  Session session;
  Harness harness;
  require_private_bus(harness);
  Fl_Window window(360, 200);
  Fl_Button labelled(10, 10, 160, 30, "Save");
  labelled.tooltip("Write the profile and its modules to disk");
  Fl_Button wordless(10, 50, 160, 30);
  wordless.tooltip("Add cooldown");
  window.end();
  window.show();
  Fl::check();
  a11y::FltkTree tree;
  atspi::Adaptor adaptor{tree};
  if (!expect(enabled(harness), "a consumer enables the adaptor")) return;
  harness.spin(ms(200));
  const auto save = named(harness, "Save");
  const auto add = named(harness, "Add cooldown");
  if (!expect(save && add, "both buttons are exported")) return;
  expect(text_property(harness, *save, "Description") ==
             "Write the profile and its modules to disk",
         "a labelled button takes the tooltip as its description");
  expect(text_property(harness, *add, "Description") == std::nullopt ||
             text_property(harness, *add, "Description")->empty(),
         "the tooltip is not said twice as the name and the description");
  window.hide();
  Fl::check();
}

// describe_with reads its hint on every request, and a changed description is
// exactly one property notification for the adaptor's cache.
void linux_editor_accessibility_hint_and_names() {
  Session session;
  Harness harness;
  require_private_bus(harness);
  Fl_Window window(360, 220);
  Fl_Input input(10, 10, 300, 28, "Active (ms)");
  Fl_Box hint(10, 50, 300, 40, "0 = off");
  Fl_Button button(10, 100, 160, 30, "Save");
  window.end();
  window.show();
  Fl::check();
  a11y::FltkTree tree;
  atspi::Adaptor adaptor{tree};
  if (!expect(enabled(harness), "a consumer enables the adaptor")) return;
  layerc::editor::accessibility::describe_with(input, hint);
  harness.spin(ms(300));
  const auto field = named(harness, "Active (ms)");
  const auto save = named(harness, "Save");
  if (!expect(field && save, "the field and the button are exported")) return;
  expect(text_property(harness, *field, "Description") == "0 = off",
         "describe_with reads the hint line live");
  const auto mark = harness.signals().size();
  hint.copy_label("0 = off, use the hint");
  expect(wait_event(harness, mark,
                    EventQuery{"org.a11y.atspi.Event.Object", "PropertyChange",
                               "accessible-description"},
                    ms(2000)),
         "a changed description is notified");
  expect(std::count_if(harness.signals().begin() +
                           static_cast<std::ptrdiff_t>(mark),
                       harness.signals().end(),
                       [](const Signal &signal) {
                         return signal.member == "PropertyChange" &&
                                signal.name == "accessible-description";
                       }) == 1,
         "a changed description is notified once, like a changed name");
  expect(text_property(harness, *field, "Description") == "0 = off, use the hint",
         "a changed hint updates the description");
  expect(text_property(harness, *save, "Name") == "Save",
         "a button without a registry entry keeps its label as its name");
  layerc::editor::accessibility::set_accessible_name(button, "Save the profile");
  harness.spin(ms(300));
  expect(text_property(harness, *save, "Name") == "Save the profile",
         "a registered name wins over the label");
  layerc::editor::accessibility::set_accessible_name(button, "");
  harness.spin(ms(300));
  expect(text_property(harness, *save, "Name") == "Save",
         "an empty name restores the label");
  // A dialog's widgets die and the next dialog lands on the same addresses:
  // neither the registry nor the exported tree may give the new widget the
  // dead one's name.
  alignas(Fl_Button) unsigned char storage[sizeof(Fl_Button)]{};
  window.begin();
  auto *first = new (storage) Fl_Button(10, 140, 160, 30, "First");
  window.end();
  layerc::editor::accessibility::set_accessible_name(*first, "First profile action");
  window.redraw();
  harness.spin(ms(300));
  expect(layerc::editor::accessibility::accessible_name(*first) == "First profile action",
         "the registry stores the name while the widget lives");
  expect(named(harness, "First profile action").has_value(),
         "the live widget is exported under its registered name");
  first->~Fl_Button();
  window.begin();
  auto *second = new (storage) Fl_Button(10, 140, 160, 30, "Second");
  window.end();
  expect(static_cast<void *>(second) == static_cast<void *>(storage),
         "the re-created button reuses the address");
  expect(!layerc::editor::accessibility::accessible_name(*second),
         "a re-created widget does not inherit a dead widget's name");
  window.redraw();
  harness.spin(ms(300));
  expect(named(harness, "Second").has_value() &&
             !named(harness, "First profile action").has_value(),
         "the exported tree names the re-created widget by its own label");
  second->~Fl_Button();
  window.hide();
  Fl::check();
}

// A color button says its color, without the access-key marker or the dialog
// ellipsis (DesignSystem.md, Screen readers).
void linux_editor_accessibility_color_names() {
  const auto name = [](const std::string_view label, const std::string_view color) {
    return layerc::editor::accessibility::color_button_name(label, color);
  };
  expect(name("&Fill...", "#DDBC17") == "Fill, color #DDBC17",
         "a chooser button drops its access key and ellipsis, then adds its color");
  expect(name("Text && &more", "#010203") == "Text & more, color #010203",
         "a doubled ampersand stays one literal ampersand");
  expect(name("Choose accent\xE2\x80\xA6", "#50BAC8") ==
             "Choose accent, color #50BAC8",
         "a single-character ellipsis is dropped too");
}

// Announcements carry their text; a source that is not exported falls back to
// the active window and then the root, and none of it needs a widget action.
void linux_editor_accessibility_announcements() {
  Session session;
  Harness harness;
  require_private_bus(harness);
  Fl_Window window(360, 200);
  Fl_Button button(10, 10, 160, 30, "Save");
  window.end();
  window.show();
  Fl::check();
  a11y::FltkTree tree;
  atspi::Adaptor adaptor{tree};
  if (!expect(enabled(harness), "a consumer enables the adaptor")) return;
  const auto object = named(harness, "Save");
  if (!expect(object.has_value(), "the button is exported")) return;
  // A screen reader subscribes to announcements before it hears one.
  harness.announce_listener("object:announcement");
  harness.spin(ms(100));
  auto mark = harness.signals().size();
  adaptor.announce(&button, "Profile saved.");
  expect(wait_event(harness, mark,
                    EventQuery{"org.a11y.atspi.Event.Object", "Announcement", ""},
                    ms(2000)),
         "an announcement reaches the bus");
  const auto announced =
      find_event(harness, mark,
                 EventQuery{"org.a11y.atspi.Event.Object", "Announcement", ""});
  if (!expect(announced.has_value(), "the announcement is readable")) return;
  expect(announced->path == *object, "an announcement comes from the widget's node");
  expect(announced->text == "Profile saved.", "the announcement text is preserved");
  // A source that is not exported speaks from the active window, as the
  // legacy bridge did, and with no window active from the application.
  const auto top = children(harness, std::string{atspi::kRootPath});
  if (!expect(top.size() == 1, "the application exports its one window")) return;
  const std::string window_path = top.front().path;
  const std::string root_path{atspi::kRootPath};
  Fl_Button unknown(10, 90, 160, 30, "Unknown");
  // The one announcement since `from`, as a client hears it; none or a
  // duplicate fails the case.
  const auto heard = [&](const std::size_t from,
                         const std::string &text) -> std::optional<Signal> {
    (void)wait_event(harness, from,
                     EventQuery{"org.a11y.atspi.Event.Object", "Announcement", ""},
                     ms(2000));
    harness.spin(ms(150));
    std::optional<Signal> only;
    int count = 0;
    for (std::size_t i = from; i < harness.signals().size(); ++i)
      if (harness.signals()[i].member == "Announcement") {
        ++count;
        only = harness.signals()[i];
      }
    if (count != 1 || !only || only->text != text) return std::nullopt;
    return only;
  };
  expect(button.take_focus() != 0, "the window becomes active through its button");
  Fl::check();
  mark = harness.signals().size();
  adaptor.announce(&unknown, "No source widget.");
  auto fallback = heard(mark, "No source widget.");
  expect(fallback && fallback->path == window_path,
         "an unknown widget falls back to the active window");
  mark = harness.signals().size();
  adaptor.announce(nullptr, "No source at all.");
  fallback = heard(mark, "No source at all.");
  expect(fallback && fallback->path == window_path,
         "a null source speaks from the active window");
  Fl::focus(nullptr);
  Fl::check();
  mark = harness.signals().size();
  adaptor.announce(&unknown, "Nothing active.");
  fallback = heard(mark, "Nothing active.");
  expect(fallback && fallback->path == root_path,
         "with no window active, an unknown widget speaks from the root");
  mark = harness.signals().size();
  adaptor.announce(nullptr, "No source, no window.");
  fallback = heard(mark, "No source, no window.");
  expect(fallback && fallback->path == root_path,
         "a null source announces from the root when no window is active");
  window.hide();
  Fl::check();
}

// Sends one key to the popup that holds the grab, as the keyboard would.
void popup_key(const int value) {
  const int previous = Fl::e_keysym;
  Fl::e_keysym = value;
  if (auto *grab = Fl::grab()) grab->handle(FL_KEYBOARD);
  Fl::e_keysym = previous;
}

// Runs a step once the popup's modal loop is up, from inside that loop.
struct DuringPopup {
  explicit DuringPopup(std::function<void()> body) : step(std::move(body)) {
    Fl::add_timeout(0.01, run, this);
  }
  ~DuringPopup() { Fl::remove_timeout(run, this); }
  DuringPopup(const DuringPopup &) = delete;
  DuringPopup &operator=(const DuringPopup &) = delete;
  std::function<void()> step;
  int attempts{};
  bool delivered{};
  static void run(void *data) {
    auto &self = *static_cast<DuringPopup *>(data);
    if (Fl::modal() != nullptr) {
      self.delivered = true;
      self.step();
    } else if (++self.attempts < 100) {
      Fl::repeat_timeout(0.01, run, data);
    }
  }
};

// What a screen reader hears while it walks a popup: each arrow that moves the
// highlight reports the focused state from the row it landed on, once, on the
// bus (the snapshot-only LNX-02 case cannot see delivery).
void linux_editor_accessibility_popup_focus_events() {
  Session session;
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"focus:", "object:"});
  Fl_Window window(460, 300);
  editor::EditorMenuBar bar(0, 0, 460, 28);
  int invoked = 0;
  const auto callback = [](Fl_Widget *, void *data) { ++*static_cast<int *>(data); };
  bar.add("&Help/&First", 0, callback, &invoked);
  bar.add("&Help/Disabled", 0, callback, &invoked, FL_MENU_INACTIVE);
  bar.add("&Help/&Last", 0, callback, &invoked);
  Fl_Button previous(20, 150, 150, 30, "Previous focus");
  window.end();
  window.show();
  previous.take_focus();
  Fl::check();
  a11y::FltkTree tree;
  atspi::Adaptor adaptor{tree};
  if (!expect(enabled(harness), "a consumer enables the adaptor")) return;
  harness.spin(ms(250));
  const auto first = named(harness, "First");
  const auto last = named(harness, "Last");
  if (!expect(first && last, "the popup rows are exported before it opens")) return;
  const auto focused_on = [&](const std::size_t from, const std::string &path) {
    int count = 0;
    for (std::size_t i = from; i < harness.signals().size(); ++i) {
      const auto &signal = harness.signals()[i];
      if (signal.member == "StateChanged" && signal.name == "focused" &&
          signal.detail1 == 1 && signal.path == path)
        ++count;
    }
    return count;
  };
  const auto mark = harness.signals().size();
  {
    DuringPopup probe([&] {
      popup_key(FL_Down);
      expect(harness.spin_until([&] { return focused_on(mark, *first) > 0; }, ms(2000)),
             "the highlighted row reports its focused state on the bus");
      popup_key(FL_Down);
      expect(harness.spin_until([&] { return focused_on(mark, *last) > 0; }, ms(2000)),
             "arrowing past the disabled row reports the last row focused");
      harness.spin(ms(200));
      expect(focused_on(mark, *first) == 1, "the first row's focus is reported once");
      popup_key(FL_Escape);
    });
    editor::track_editor_popup(bar, 0);
    expect(probe.delivered, "the popup probe ran inside the popup");
  }
  expect(!editor::popup_owner() && invoked == 0,
         "Escape closes the popup without invoking a row");
  window.hide();
  Fl::check();
}

// The visible line says the same thing the status reports, and it changes while
// the editor stays open.
void linux_editor_accessibility_status_lines() {
  using editor::AccessibilityStatusState;
  expect(editor::accessibility_status_line({AccessibilityStatusState::active, {}}) ==
             "Accessibility: AT-SPI bridge active",
         "active keeps the wording the GUI checks");
  expect(editor::accessibility_status_line({AccessibilityStatusState::waiting, {}}) ==
             "Accessibility: AT-SPI bridge waiting for a screen reader",
         "waiting names the waiting state");
  expect(editor::accessibility_status_line({AccessibilityStatusState::unavailable,
                                            "no accessibility bus is reachable"}) ==
             "Accessibility unavailable: no accessibility bus is reachable",
         "unavailable names the reason");

  Session session;
  Harness harness;
  require_private_bus(harness);
  // The adaptor reports through a plain function, so the case keeps its log in
  // a static target, as the editor's own wrapper does.
  struct Log {
    atspi::Adaptor *adaptor{};
    std::vector<std::string> lines;
    static editor::AccessibilityStatus neutral_status(const atspi::AdaptorStatus &status) {
      switch (status.state) {
      case atspi::StatusState::on:
        return {editor::AccessibilityStatusState::active, {}};
      case atspi::StatusState::off:
        return {editor::AccessibilityStatusState::waiting, {}};
      case atspi::StatusState::unavailable:
        return {editor::AccessibilityStatusState::unavailable, status.reason};
      }
      return {editor::AccessibilityStatusState::unavailable, status.reason};
    }
    static void record(void *data) {
      auto &self = *static_cast<Log *>(data);
      self.lines.push_back(editor::accessibility_status_line(
          neutral_status(self.adaptor->status())));
    }
  } log;
  {
    a11y::FltkTree tree;
    atspi::Adaptor adaptor{tree};
    log.adaptor = &adaptor;
    adaptor.on_status_change(&Log::record, &log);
    if (!expect(enabled(harness), "a consumer enables the adaptor")) return;
    expect(!log.lines.empty() && log.lines.back() ==
                                     "Accessibility: AT-SPI bridge active",
           "the line turns active when a consumer listens");
    harness.set_flags(false, false);
    expect(harness.spin_until(
               [&] { return adaptor.status().state == atspi::StatusState::off; },
               ms(3000)),
           "the status returns to waiting");
    expect(log.lines.back() ==
               "Accessibility: AT-SPI bridge waiting for a screen reader",
           "the line returns to waiting");
    harness.set_flags(true, false);
    expect(harness.spin_until(
               [&] { return adaptor.status().state == atspi::StatusState::on; },
               ms(3000)),
           "the status turns on again");
  }
}

// The editor's own accessibility object is the one the window uses, and its
// announcements ride the same connection.
void linux_editor_accessibility_in_the_window() {
  Session session;
  Harness harness;
  require_private_bus(harness);
  layerc::test::TempDirectory temp("layerc-editor-accessibility");
  layerc::test::ScopedEnvironment home("HOME", temp.path());
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR", temp.path() / "runtime");
  layerc::test::ScopedEnvironment cache("XDG_CACHE_HOME", temp.path() / "cache");
  layerc::test::ScopedEnvironment state("XDG_STATE_HOME", temp.path() / "state");
  layerc::test::ScopedEnvironment profiles("LAYERC_PROFILE_DIR", std::nullopt);
  layerc::test::ScopedEnvironment store("LAYERC_STORE_ROOT", std::nullopt);
  std::filesystem::create_directories(temp.path() / "runtime");
  expect(::chmod((temp.path() / "runtime").c_str(), 0700) == 0,
         "fixture namespace is private");
  editor::EditorApp app;
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  Fl::check();
  auto *accessibility = editor::EditorAppTestAccess::accessibility(app);
  if (!expect(accessibility != nullptr, "the window owns an accessibility object"))
    return;
  std::string line;
  accessibility->set_status_handler(
      [&](const editor::AccessibilityStatus &status) {
        line = editor::accessibility_status_line(status);
      });
  expect(line == "Accessibility: AT-SPI bridge waiting for a screen reader",
         "a late caller is handed the current waiting line");
  harness.set_flags(true, false);
  expect(harness.spin_until(
             [&] { return accessibility->status().state ==
                       editor::AccessibilityStatusState::active; },
             ms(3000)),
         "the window's own adaptor turns on");
  expect(line == "Accessibility: AT-SPI bridge active",
         "the window's line is the active wording");
  const auto mark = harness.signals().size();
  accessibility->announce(nullptr, "Profile saved.");
  expect(wait_event(harness, mark,
                    EventQuery{"org.a11y.atspi.Event.Object", "Announcement", ""},
                    ms(2000)),
         "the window's announce reaches the bus");
  editor::EditorAppTestAccess::window(app).hide();
  Fl::check();
}

// GTK's file chooser would dlopen GTK and its own accessibility bridge into
// this process, so the editor's accessibility object pins FLTK's chooser for
// its lifetime and restores the option it found.
void linux_editor_accessibility_keeps_fltk_file_chooser() {
  layerc::test::require_x11();
  layerc::test::TempDirectory temp("layerc-editor-chooser");
  layerc::test::ScopedEnvironment home("HOME", temp.path());
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR",
                                          temp.path() / "runtime");
  layerc::test::ScopedEnvironment cache("XDG_CACHE_HOME", temp.path() / "cache");
  layerc::test::ScopedEnvironment state("XDG_STATE_HOME", temp.path() / "state");
  const bool initial = Fl::option(Fl::OPTION_FNFC_USES_GTK);
  Fl::option(Fl::OPTION_FNFC_USES_GTK, true);
  {
    editor::EditorApp app;
    editor::EditorAccessibility accessibility{app};
    expect(!Fl::option(Fl::OPTION_FNFC_USES_GTK),
           "the editor uses FLTK's own file chooser, not GTK's");
  }
  expect(Fl::option(Fl::OPTION_FNFC_USES_GTK),
         "the chooser option is restored when the editor goes away");
  Fl::option(Fl::OPTION_FNFC_USES_GTK, initial);
}
