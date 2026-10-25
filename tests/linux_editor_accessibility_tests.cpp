#include "accessibility_tree.hpp"
#include "test_support.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
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
