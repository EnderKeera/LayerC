#include "accessible_widgets.hpp"
#include "editor_commands.hpp"
#include "accessibility_tree.hpp"
#include "a11y/fltk_semantics.hpp"
#include "test_support.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>
#include <filesystem>
#include <memory>
#include <string>
namespace {
namespace a11y = layerc::editor::accessibility;
using layerc::test::expect;
struct Unref { void operator()(AtkObject *p) const { if (p) g_object_unref(p); } };
using Ref = std::unique_ptr<AtkObject, Unref>;
AtkObject *find_widget(AtkObject *object, Fl_Widget *widget) {
  auto &n = a11y::node(object);
  if (n.kind == a11y::Kind::widget && n.widget() == widget) return object;
  for (auto *child : n.children)
    if (auto *found = find_widget(child, widget)) return found;
  return nullptr;
}
bool defunct(AtkObject *object) {
  auto *state = atk_object_ref_state_set(object);
  const bool answer = atk_state_set_contains_state(state, ATK_STATE_DEFUNCT) != 0;
  g_object_unref(state);
  return answer;
}
// What a screen reader hears: the announcement's origin, text and priority.
struct Announcement {
  AtkObject *object{};
  std::string text;
  int live{-1};
  int count{};
};
void on_notification(AtkObject *object, const gchar *text, gint live, void *data) {
  auto &heard = *static_cast<Announcement *>(data);
  heard.object = object;
  heard.text = text != nullptr ? text : "";
  heard.live = live;
  ++heard.count;
}
int description_notifies;
void on_description_notify(GObject *, GParamSpec *, void *) { ++description_notifies; }

// The portable reader must preserve the old bridge's text, including invalid
// file-name bytes and embedded NULs. GLib is only the legacy test oracle.
void portable_utf8_repair_matches_legacy() {
  const auto matches = [](const std::string &text) {
    auto *expected = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
    const bool same = layerc::editor::a11y::valid_utf8(text) == expected;
    g_free(expected);
    return same;
  };
  bool all_match = true;
  for (unsigned first = 0; first < 256 && all_match; ++first) {
    all_match = matches(std::string(1, static_cast<char>(first)));
    for (unsigned second = 0; second < 256 && all_match; ++second) {
      std::string text(2, static_cast<char>(first));
      text[1] = static_cast<char>(second);
      all_match = matches(text);
    }
  }
  expect(all_match, "portable UTF-8 repair preserves every one- and two-byte sequence");
  for (const auto *text : {"", "Save", "\xE2\x82\xAC", "\xF0\x9F\x98\x80",
                           "\xE0\x80\x80", "\xED\xA0\x80", "\xF0\x80\x80\x80",
                           "\xF4\x90\x80\x80", "\xF0\x9F\x98", "\xE2\x82"})
    expect(matches(text), "portable UTF-8 repair preserves valid scalars and repairs invalid scalars");
  expect(matches(std::string("A\0B", 3)), "portable UTF-8 repair replaces embedded NULs");
}
}
// Intentional accessibility-independent fallback: a plain Fl_Hold_Browser keeps owner
// bounds and lazy row lifetime. ContextBrowser geometry: linux_editor_semantic_row_extents.
void linux_editor_accessibility_tree_rows() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  Fl_Window window(440, 320);
  Fl_Tabs tabs(0, 0, 440, 320);
  Fl_Group profile(0, 25, 440, 295, "Profile"); profile.end();
  Fl_Group panels(0, 25, 440, 295, "Panels");
  Fl_Hold_Browser list(10, 60, 400, 200, "Panels");
  for (int i = 1; i <= 5843; ++i) list.add(("Panel " + std::to_string(i)).c_str());
  panels.end(); tabs.end(); window.end();
  tabs.value(&profile);
  list.value(2500);
  window.show(); Fl::check();
  {
    a11y::AccessibilityTree tree;
    std::string fault;
    if (!expect(tree.initialize(fault), "standalone accessibility tree initializes")) return;
    tree.reconcile(true);
    auto *object = find_widget(atk_get_root(), &list);
    if (!expect(object != nullptr, "hidden browser itself remains exported")) return;
    expect(!list.visible_r(), "browser is hidden by its parent tab");
    expect(atk_object_get_n_accessible_children(object) == 0,
           "hidden browser does not eagerly materialize 5843 row objects");
    tabs.value(&panels); tree.sync();
    expect(list.visible_r(), "panel browser is now showing");
    expect(atk_object_get_n_accessible_children(object) == 5843,
           "showing browser exports every row, including offscreen rows");
    Ref row(atk_object_ref_accessible_child(object, 2499));
    Ref last(atk_object_ref_accessible_child(object, 5842));
    if (!expect(row && last, "selected and last row are reachable")) return;
    expect(std::string(atk_object_get_name(row.get())) == "Panel 2500", "row text preserved");
    expect(std::string(atk_object_get_name(last.get())) == "Panel 5843", "offscreen final row preserved");
    expect(atk_object_get_role(row.get()) == ATK_ROLE_LIST_ITEM, "row role preserved");
    expect(atk_object_get_parent(row.get()) == object &&
           atk_object_get_index_in_parent(row.get()) == 2499, "row parent/index preserved");
    expect(atk_selection_get_selection_count(ATK_SELECTION(object)) == 1 &&
           atk_selection_is_child_selected(ATK_SELECTION(object), 2499), "selection exposed");
    tree.sync();
    Ref stable(atk_object_ref_accessible_child(object, 2499));
    expect(stable.get() == row.get(), "unchanged showing rows retain object identity");
    tabs.value(&profile); tree.sync();
    expect(atk_object_get_n_accessible_children(object) == 0, "hidden rows are removed");
    expect(defunct(row.get()) && atk_object_get_parent(row.get()) == nullptr,
           "externally retained hidden row is detached and defunct");
    expect(!atk_action_do_action(ATK_ACTION(row.get()), 0), "defunct row action is rejected");
    expect(list.size() == 5843 && list.value() == 2500,
           "hiding accessibility rows does not mutate the list or its selection");
    tabs.value(&panels); tree.sync();
    Ref restored(atk_object_ref_accessible_child(object, 2499));
    if (!expect(restored != nullptr, "row returns after tab reactivation")) return;
    expect(restored.get() != row.get() && !defunct(restored.get()) && defunct(row.get()),
           "reactivation creates live rows without reviving stale references");
    expect(atk_object_get_n_accessible_children(object) == 5843 &&
           atk_selection_is_child_selected(ATK_SELECTION(object), 2499),
           "all rows and original selection return after reactivation");
    tabs.value(&profile); tree.sync();
    list.clear(); list.add("Replacement"); list.value(1); tree.sync();
    expect(atk_object_get_n_accessible_children(object) == 0, "hidden list rebuild stays lazy");
    tabs.value(&panels); tree.sync();
    Ref replacement(atk_object_ref_accessible_child(object, 0));
    if (!expect(replacement != nullptr, "rebuilt row is reachable")) return;
    expect(atk_object_get_n_accessible_children(object) == 1 &&
           std::string(atk_object_get_name(replacement.get())) == "Replacement" &&
           atk_selection_is_child_selected(ATK_SELECTION(object), 0),
           "hidden rebuild restores correct content and selection");
    expect(defunct(restored.get()), "hidden rebuild cannot retarget an old row");
    list.hide(); tree.sync();
    expect(atk_object_get_n_accessible_children(object) == 0 && defunct(replacement.get()),
           "directly hiding the browser also retires its rows");
    list.show(); tree.sync();
    expect(atk_object_get_n_accessible_children(object) == 1,
           "directly showing the browser recreates its rows");
  }
  window.hide(); Fl::check();
}

// The line that belongs to a control is its description, and a wordless
// button already says its tooltip as its name (DesignSystem.md, Screen
// readers).
void linux_editor_accessibility_descriptions() {
  portable_utf8_repair_matches_legacy();
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  Fl_Window window(360, 200);
  Fl_Button labelled(10, 10, 160, 30, "Save");
  labelled.tooltip("Write the profile and its modules to disk");
  Fl_Button wordless(10, 50, 160, 30);
  wordless.tooltip("Add cooldown");
  window.end();
  window.show();
  Fl::check();
  a11y::AccessibilityTree tree;
  std::string fault;
  if (!expect(tree.initialize(fault), "standalone accessibility tree initializes")) return;
  tree.reconcile(true);
  tree.sync();
  auto *labelled_object = find_widget(atk_get_root(), &labelled);
  auto *wordless_object = find_widget(atk_get_root(), &wordless);
  if (!expect(labelled_object != nullptr && wordless_object != nullptr,
              "both buttons are exported")) return;
  expect(std::string(atk_object_get_name(labelled_object)) == "Save",
         "a labelled button keeps its label as its name");
  expect(std::string(atk_object_get_description(labelled_object)) ==
             "Write the profile and its modules to disk",
         "a labelled button takes the tooltip as its description");
  expect(std::string(atk_object_get_name(wordless_object)) == "Add cooldown",
         "a wordless button takes the tooltip as its name");
  expect(std::string(atk_object_get_description(wordless_object)).empty(),
         "the tooltip is not said twice as the name and the description");
  window.hide();
  Fl::check();
}

// describe_with reads its hint on every request, and a changed description
// is a property notification for the adaptor's cache.
void linux_editor_accessibility_hint_and_names() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  Fl_Window window(360, 220);
  Fl_Input input(10, 10, 300, 28, "Active (ms)");
  Fl_Box hint(10, 50, 300, 40, "0 = off");
  Fl_Button button(10, 100, 160, 30, "Save");
  window.end();
  window.show();
  Fl::check();
  a11y::AccessibilityTree tree;
  std::string fault;
  if (!expect(tree.initialize(fault), "standalone accessibility tree initializes")) return;
  tree.reconcile(true);
  tree.sync();
  auto *input_object = find_widget(atk_get_root(), &input);
  auto *button_object = find_widget(atk_get_root(), &button);
  if (!expect(input_object != nullptr && button_object != nullptr,
              "the field and the button are exported")) return;
  a11y::describe_with(input, hint);
  tree.sync();
  expect(std::string(atk_object_get_description(input_object)) == "0 = off",
         "describe_with reads the hint line live");
  description_notifies = 0;
  g_signal_connect(input_object, "notify::accessible-description",
                   G_CALLBACK(on_description_notify), nullptr);
  hint.copy_label("0 = off, use the hint");
  tree.sync();
  expect(std::string(atk_object_get_description(input_object)) ==
             "0 = off, use the hint",
         "a changed hint updates the description on the next sync");
  expect(description_notifies == 1,
         "a changed description is notified once, like a changed name");
  g_signal_handlers_disconnect_by_func(
      input_object, reinterpret_cast<gpointer>(on_description_notify), nullptr);
  expect(std::string(atk_object_get_name(button_object)) == "Save",
         "a button without a registry entry keeps its label as its name");
  a11y::set_accessible_name(button, "Save the profile");
  tree.sync();
  expect(std::string(atk_object_get_name(button_object)) == "Save the profile",
         "a registered name wins over the label");
  a11y::set_accessible_name(button, "");
  tree.sync();
  expect(std::string(atk_object_get_name(button_object)) == "Save",
         "an empty name restores the label");
  // A dialog's widgets die and the next dialog lands on the same addresses.
  alignas(Fl_Button) unsigned char storage[sizeof(Fl_Button)]{};
  auto *first = new (storage) Fl_Button(0, 0, 120, 28, "First");
  a11y::set_accessible_name(*first, "First profile action");
  expect(a11y::accessible_name(*first) == "First profile action",
         "the registry stores the name while the widget lives");
  first->~Fl_Button();
  auto *second = new (storage) Fl_Button(0, 0, 120, 28, "Second");
  expect(second == first, "the re-created button reuses the address");
  expect(!a11y::accessible_name(*second),
         "a re-created widget does not inherit a dead widget's name");
  second->~Fl_Button();
  window.hide();
  Fl::check();
}

// A color button says its color, without the access-key marker or the dialog
// ellipsis (DesignSystem.md, Screen readers).
void linux_editor_accessibility_color_names() {
  expect(a11y::color_button_name("&Fill...", "#DDBC17") ==
             "Fill, color #DDBC17",
         "a chooser button drops its access key and ellipsis, then adds its color");
  expect(a11y::color_button_name("Text && &more", "#010203") ==
             "Text & more, color #010203",
         "a doubled ampersand stays one literal ampersand");
  expect(a11y::color_button_name("Choose accent\xE2\x80\xA6", "#50BAC8") ==
             "Choose accent, color #50BAC8",
         "a single-character ellipsis is dropped too");
}

// Announcements carry their text and priority; a source that is not exported
// falls back to the active window and then the root.
void linux_editor_accessibility_announcements() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  expect(g_signal_lookup("notification", ATK_TYPE_OBJECT) != 0,
         "ATK 2.62 declares the notification signal an announcement rides on");
  Fl_Window window(360, 200);
  Fl_Button button(10, 10, 160, 30, "Save");
  window.end();
  window.show();
  Fl::check();
  a11y::AccessibilityTree tree;
  std::string fault;
  if (!expect(tree.initialize(fault), "standalone accessibility tree initializes")) return;
  tree.reconcile(true);
  tree.sync();
  auto *object = find_widget(atk_get_root(), &button);
  if (!expect(object != nullptr, "the button is exported")) return;
  Announcement known;
  g_signal_connect(object, "notification", G_CALLBACK(on_notification), &known);
  tree.announce(&button, "Profile saved.");
  expect(known.count == 1 && known.object == object &&
             known.text == "Profile saved." && known.live == ATK_LIVE_POLITE,
         "an announcement comes from the widget's object, with ATK_LIVE_POLITE");
  Announcement root_fallback;
  Announcement window_fallback;
  g_signal_connect(atk_get_root(), "notification", G_CALLBACK(on_notification),
                   &root_fallback);
  auto *window_object = find_widget(atk_get_root(), &window);
  if (window_object != nullptr)
    g_signal_connect(window_object, "notification", G_CALLBACK(on_notification),
                     &window_fallback);
  Fl_Button unknown(10, 90, 160, 30, "Unknown");
  tree.announce(&unknown, "No source widget.");
  expect(known.count == 1 && root_fallback.count + window_fallback.count == 1 &&
             (root_fallback.count == 0 || root_fallback.text == "No source widget.") &&
             (window_fallback.count == 0 ||
              window_fallback.text == "No source widget."),
         "an unknown widget falls back to the active window or the root");
  Announcement null_fallback;
  g_signal_connect(atk_get_root(), "notification", G_CALLBACK(on_notification),
                   &null_fallback);
  tree.announce(nullptr, "No source at all.");
  expect(known.count == 1 && null_fallback.count == 1 &&
             null_fallback.text == "No source at all.",
         "a null source announces from the root when no window is active");
  window.hide();
  Fl::check();
}

// Every shortcut row is "key<TAB>description" from the command table, F1
// included, so the dialog can measure two columns (DesignSystem.md, Keyboard).
void linux_editor_shortcuts_columns() {
  const std::string rows = layerc::editor::shortcut_help();
  expect(rows.find("F1\t") != std::string::npos, "F1 appears in the shortcut table");
  bool tabbed = true;
  std::size_t start = 0;
  while (start < rows.size()) {
    const auto end = rows.find('\n', start);
    const auto line =
        rows.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty())
      tabbed = tabbed && line.find('\t') != std::string::npos &&
               line.find('\t') == line.rfind('\t');
    if (end == std::string::npos) break;
    start = end + 1;
  }
  expect(tabbed, "every shortcut row has exactly one tab and no space-aligned columns");
  // The rows come from the one command table: the file and edit shortcuts are
  // there with their keys, so the dialog never restates a binding.
  expect(rows.find("	Save") != std::string::npos &&
             rows.find("	Undo") != std::string::npos &&
             rows.find("	Redo") != std::string::npos,
         "command-table shortcut rows appear with their keys");
}
