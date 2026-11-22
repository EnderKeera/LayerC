// F17 diagnostics: the panel-colours layout, the field-specific status text and
// the pending-automation validation box the Linux editor reports.
#include "automation_editor.hpp"
#include "automation_graph.hpp"
#include "editor_app.hpp"
#include "layerc/core/json.hpp"
#include "editor_menus.hpp"
#include "editor_tools.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/core/profile.hpp"
#include "profile_edit.hpp"
#include "linux_editor_test_access.hpp"
#include "editor_test_environment.hpp"
#include "fltk_a11y_snapshot_support.hpp"
#include "a11y/fltk_semantics.hpp"

namespace editor = layerc::editor;

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Round_Button.H>

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace {

using layerc::test::expect;

// A disposable XDG environment with one stored fixture profile, mirroring the
// widget-suite fixture.
class Fixture final {
public:
  Fixture()
      : temp_("layerc-editor-diagnostics", layerc::test::EditorTestScope::widget,
              "fixture control namespace is private") {
    path = layerc::platform::profile_directory() / "fixture.json";
  }
  void store() {
    auto profile = layerc::editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    expect(layerc::platform::write_profile_file(
               path, layerc::core::serialize_profile_json(profile))
               .ok(),
           "store only a private fixture");
  }
  std::filesystem::path path;

private:
  layerc::test::EditorTestEnvironment temp_;
};

void show(layerc::editor::EditorApp &app) {
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  Fl::check();
}

void tab(layerc::editor::EditorApp &app, Fl_Group *group) {
  editor::EditorAppTestAccess::tabs(app)->value(group);
  app.tab_changed();
}

template <typename Widget>
Widget *find_typed(Fl_Group &group, const std::string_view label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (auto *typed = dynamic_cast<Widget *>(child);
        typed && child->label() && label == child->label())
      return typed;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *found = find_typed<Widget>(*nested, label)) return found;
  }
  return nullptr;
}

template <typename Widget>
Widget &automation_control(layerc::editor::EditorApp &app,
                           const std::string_view label) {
  auto *widget = find_typed<Widget>(*editor::EditorAppTestAccess::automation(app)->group(), label);
  expect(widget != nullptr, "the automation page owns the named control: " + std::string(label));
  return *widget;
}

} // namespace

void linux_editor_diagnostic_text_contracts() {
  namespace editor = layerc::editor;
#ifndef _WIN32
  setenv("FLTK_BACKEND", "x11", 1);
#endif
  const auto layout = editor::panel_colors_layout();
  expect(layout.label_width > 0 && 15 + layout.label_width <= layout.input_x - 12,
         "the widest label fits left of the first input");
  expect(layout.button_x >= layout.input_x + 140,
         "the chooser column follows the input column");
  expect(layout.window_width >= layout.button_x + 155 + 15,
         "the window is wide enough for both columns and the trailing margin");
}

void linux_editor_field_error_contracts() {
  namespace editor = layerc::editor;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  tab(app, editor::EditorAppTestAccess::cooldown_tab(app));
  const auto charges = app.session.document().profile().cooldowns.front().max_charges;
  editor::EditorAppTestAccess::charges(app)->value("0");
  app.field_changed(editor::EditorAppTestAccess::charges(app));
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find(
             "Invalid input: Max charges must be between 1 and 99.") !=
                 std::string::npos &&
             app.session.document().profile().cooldowns.front().max_charges ==
                 charges,
         "an out-of-range charge count names the field and keeps the profile");
  app.command(editor::EditorCommand::undo);
  tab(app, editor::EditorAppTestAccess::panel_tab(app));
  const auto anchor = app.session.document().profile().panels.front().anchor_x;
  editor::EditorAppTestAccess::anchor_x(app)->value("2");
  app.field_changed(editor::EditorAppTestAccess::anchor_x(app));
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find(
             "Invalid input: Anchor X must be between 0 and 1.") !=
                 std::string::npos &&
             app.session.document().profile().panels.front().anchor_x == anchor,
         "an out-of-range anchor names the field and keeps the profile");
  editor::EditorAppTestAccess::window(app).hide();
}

// Closing the document must clear the page lists, not leave stale rows behind.

void linux_editor_page_lists_clear_without_document() {
  namespace editor = layerc::editor;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  expect(app.session.has_document() &&
             editor::EditorAppTestAccess::cooldowns(app)->size() > 0 &&
             editor::EditorAppTestAccess::bindings(app)->size() > 0 &&
             editor::EditorAppTestAccess::panels(app)->size() > 0,
         "the fixture fills the cooldown, binding and panel lists");
  app.session.close_document();
  app.refresh();
  expect(!app.session.has_document() &&
             editor::EditorAppTestAccess::cooldowns(app)->size() == 0 &&
             editor::EditorAppTestAccess::bindings(app)->size() == 0 &&
             editor::EditorAppTestAccess::panels(app)->size() == 0,
         "closing the document clears every page list");
  editor::EditorAppTestAccess::window(app).hide();
}

void linux_editor_pending_automation_status_contracts() {
  namespace editor = layerc::editor;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  app.command(editor::EditorCommand::automation_page);
  tab(app, editor::EditorAppTestAccess::automation(app)->group());
  editor::EditorAppTestAccess::automation(app)->refresh();
  auto &json = automation_control<Fl_Multiline_Input>(app, "Advanced automation JSON");
  auto &validation = automation_control<Fl_Output>(app, "Automation validation");
  json.value("{");
  json.do_callback();
  expect(std::string(validation.value()).starts_with(
             "Pending JSON is invalid and not applied: "),
         "an invalid pending buffer describes itself in the validation box");
  const auto pending_text = std::string(validation.value());
  automation_control<Fl_Button>(app, "Validate / Apply JSON").do_callback();
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find("Invalid automation JSON:") !=
                 std::string::npos &&
             std::string(validation.value()) == pending_text,
         "a refused apply keeps the pending-buffer text visible");
  editor::EditorAppTestAccess::automation(app)->refresh();
  const auto &document = app.session.document();
  expect(std::string(validation.value()) ==
             editor::automation_validation_summary(document.automation(),
                                                   document.profile()),
         "discarding the buffer restores the committed summary");
  editor::EditorAppTestAccess::window(app).hide();
}

void diagnostics_summary_raw_and_retry() {
  Fixture fixture;
  namespace runtime = layerc::runtime;
  runtime::RuntimeDiagnostics diagnostics;
  diagnostics.package_version = "test-build";
  diagnostics.helpers.emplace_back().quarantined = true;
  const auto snapshot = runtime::serialize_runtime_diagnostics_json(diagnostics);
  editor::DaemonStatus status;
  int queries{}, retries{};
  editor::DaemonCommandResult result{editor::DaemonCommandState::acknowledged,
                                     "visual runtime retry requested"};
  editor::DiagnosticsWindow window([&] { ++queries; return status; },
                                   [&] { ++retries; return result; });
  window.show();
  Fl_Window *dialog = Fl::first_window();
  const bool shown = dialog && dialog->label() &&
      std::string_view(dialog->label()) == "Runtime diagnostics — LayerE";
  expect(shown, "show the diagnostics tool window");
  if (!dialog || !shown) return;
  auto *summary = find_typed<Fl_Round_Button>(*dialog, "&Summary");
  auto *raw = find_typed<Fl_Round_Button>(*dialog, "Raw &JSON");
  auto *details = find_typed<Fl_Multiline_Output>(*dialog, "Runtime diagnostics");
  auto *retry = find_typed<Fl_Button>(*dialog, "Retry &visual");
  expect(summary && raw && details && retry, "find the single details box and its controls");
  if (!summary || !raw || !details || !retry) return;
  expect(summary->value() && !raw->active_r() && !summary->active_r() && !retry->visible(),
         "without a snapshot Summary is selected and unavailable actions are disabled");
  status.payload_json = snapshot;
  window.refresh();
  expect(!retry->visible(), "a quarantined snapshot without a running daemon cannot be retried");
  status.running = true;
  diagnostics.helpers.front().quarantined = false;
  status.payload_json = runtime::serialize_runtime_diagnostics_json(diagnostics);
  window.refresh();
  expect(!retry->visible(), "a running daemon without quarantine cannot be retried");
  status.payload_json = snapshot;
  window.refresh();
  const std::string summary_text(details->value());
  expect(raw->active_r() && summary->active_r() && summary_text.find("Quarantined") != std::string::npos &&
             static_cast<bool>(retry->visible()) == editor::daemon_retry_supported(),
         "the review state and platform support govern retry visibility");
  auto formatted = layerc::core::json::format(snapshot);
  std::erase(formatted, '\r');
  raw->setonly();
  raw->do_callback();
  expect(details->value() == formatted, "Raw JSON shows formatted text in the details box");
  window.refresh();
  expect(raw->value() && details->value() == formatted, "refresh preserves the details mode");
  status.payload_json = "{invalid";
  window.refresh();
  expect(std::string_view(details->value()) == "{invalid", "invalid JSON is displayed verbatim");
  status.payload_json = snapshot;
  window.refresh();
  summary->setonly();
  summary->do_callback();
  expect(details->value() == summary_text, "Summary restores the original details");
  if (editor::daemon_retry_supported()) {
    const int before = queries;
    retry->do_callback();
    expect(retries == 1 && queries == before + 1 &&
               std::string_view(details->value()).ends_with("Visual retry: visual runtime retry requested"),
           "retry refreshes and shows the acknowledgement");
    result = {editor::DaemonCommandState::refused, "test refusal"};
    retry->do_callback();
    expect(retries == 2 && std::string_view(details->value()).ends_with("Visual retry failed: test refusal"),
           "retry displays the refusal in the refreshed summary");
  }
}

void automation_validate_json_is_a_dry_run() {
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  app.command(editor::EditorCommand::automation_page);
  auto *group = editor::EditorAppTestAccess::automation(app)->group();
  tab(app, group);
  auto *retry = find_typed<Fl_Button>(*group, "Retry");
  expect((retry != nullptr) == editor::daemon_retry_supported() && (!retry || !retry->active()),
         "Retry exists only on a supported platform and requires a selected automation");
  automation_control<Fl_Button>(app, "New automation").do_callback();
  expect(!retry || retry->active(), "selecting an automation in a stored profile enables Retry");
  auto &view = automation_control<Fl_Choice>(app, "Automation view");
  view.value(2);
  view.do_callback();
  auto &json = automation_control<Fl_Multiline_Input>(app, "Advanced automation JSON");
  auto &validation = automation_control<Fl_Output>(app, "Automation validation");
  auto &validate = automation_control<Fl_Button>(app, "Validate JSON");
  const auto original = app.session.document().automation();
  auto edited = original;
  expect(edited && !edited->automations.empty(), "seed one automation for the dry run");
  if (!edited || edited->automations.empty()) return;
  edited->automations.front().name = "Validated, not applied";
  auto text = editor::automation_graph_json(edited);
  std::erase(text, '\r');
  const auto generation = app.session.edit_generation();
  const bool undo = app.session.can_undo(), redo = app.session.can_redo();
  json.value(text.c_str());
  json.do_callback();
  validate.do_callback();
  expect(std::string_view(validation.value()) == "Valid - ready to apply" &&
             app.session.document().automation() == original &&
             generation == app.session.edit_generation() &&
             undo == app.session.can_undo() && redo == app.session.can_redo(),
         "a valid dry run does not change document or history");
  automation_control<Fl_Button>(app, "Validate / Apply JSON").do_callback();
  expect(app.session.document().automation() == edited, "Validate leaves the buffer pending for Apply");
  constexpr auto invalid = "{\n  \"schemaVersion\": x\n}";
  json.value(invalid);
  json.do_callback();
  validate.do_callback();
  const auto parsed = layerc::core::parse_automation_profile_json(invalid, app.session.document().profile());
  const auto *error = std::get_if<layerc::core::AutomationError>(&parsed);
  expect(error != nullptr, "the fixture has a JSON parse error");
  if (!error) return;
  const auto location = editor::locate_json_error(invalid, *error);
  expect(location.begin && location.end, "the parse error has a precise span");
  if (!location.begin || !location.end) return;
  const auto begin = static_cast<int>(*location.begin), end = static_cast<int>(*location.end);
  const auto selected = [&] { return json.mark() == begin && json.insert_position() == end; };
  expect(std::string(validation.value()) == "Pending JSON is invalid and not applied: " + location.message &&
             selected() && app.session.document().automation() == edited,
         "an invalid dry run selects the located error without applying");
  json.insert_position(0);
  const int old_key = Fl::e_keysym;
  Fl::e_keysym = FL_Enter;
  const bool focused = validation.take_focus() != 0;
  const bool handled = validation.handle(FL_KEYDOWN) != 0;
  Fl::e_keysym = old_key;
  expect(focused && handled && selected(), "Enter on validation restores the error selection");
  using enum editor::DaemonCommandState;
  expect(editor::automation_retry_report({acknowledged, "wire text"}) ==
             std::pair{editor::StatusKind::success, std::string("Automation retry requested.")} &&
             editor::automation_retry_report({refused, "refused"}) ==
             std::pair{editor::StatusKind::failure, std::string("Automation retry failed: refused")} &&
             editor::automation_retry_report({daemon_unavailable, "offline"}) ==
             std::pair{editor::StatusKind::failure, std::string("Automation retry failed: offline")},
         "the retry status mapping preserves LayerD failure details");
}

void linux_editor_semantic_row_extents() {
  namespace a11y = layerc::editor::a11y;
  namespace a11y_snapshot = layerc::test::a11y_snapshot;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  editor::EditorAppTestAccess::release_accessibility(app);
  auto &window = editor::EditorAppTestAccess::window(app);
  auto *list = dynamic_cast<editor::ContextBrowser *>(
      editor::EditorAppTestAccess::profiles(app));
  if (!expect(list != nullptr, "production profile list is a ContextBrowser")) return;
  list->clear();
  for (int i = 1; i <= 80; ++i)
    list->add(("Semantic row " + std::to_string(i)).c_str());
  // A framed list below the menu bar: its rows start inside the box, not at y().
  list->box(FL_BORDER_BOX);
  list->topline(1);
  list->redraw();
  Fl::check();
  expect(list->y() > 0 && Fl::box_dy(list->box()) > 0,
         "the production list sits below the window top inside a border");
  const a11y::Target list_target{list, a11y::TargetKind::widget, 0};
  // Every visible row slice hit-tests back to its own row at both edges.
  const auto hits_agree = [&] {
    int seen = 0;
    bool agree = true;
    for (int row = 1; row <= list->size(); ++row) {
      const auto slice = list->row_bounds(row);
      if (!slice) continue;
      ++seen;
      const int middle = slice->x + slice->width / 2;
      agree = agree && list->row_at(middle, slice->y) == row &&
              list->row_at(middle, slice->y + slice->height - 1) == row;
    }
    return agree && seen > 0;
  };
  expect(hits_agree(), "row bounds agree with hit testing at the top");
  const auto at_top = a11y::scroll_of(list_target);
  expect(at_top && at_top->vertical == 0.0 && at_top->vertical_view > 0.0 &&
             at_top->vertical_view < 100.0,
         "an unscrolled long list reports 0 percent and a partial view");
  Fl_Menu_Bar *bar = nullptr;
  std::function<void(Fl_Group &)> locate = [&](Fl_Group &group) {
    for (int i = 0; i < group.children() && !bar; ++i) {
      if (auto *found = dynamic_cast<Fl_Menu_Bar *>(group.child(i))) bar = found;
      else if (auto *nested = dynamic_cast<Fl_Group *>(group.child(i))) locate(*nested);
    }
  };
  locate(window);
  if (!expect(bar != nullptr, "the editor window owns a menu bar")) return;
  {
    a11y::FltkTree tree;
    auto snapshot = tree.snapshot();
    const auto *browser = a11y_snapshot::find(tree, snapshot, *list);
    const auto *menu_object = a11y_snapshot::find(tree, snapshot, *bar);
    if (!expect(browser && menu_object, "browser and menu are exported")) return;
    const auto *first = a11y_snapshot::child(snapshot, *browser, 0);
    const auto *second = a11y_snapshot::child(snapshot, *browser, 1);
    const auto *last = a11y_snapshot::child(snapshot, *browser, 79);
    const auto *item = a11y_snapshot::child(snapshot, *menu_object, 0);
    if (!expect(first && second && last && item, "rows and menu item are reachable")) return;
    const auto first_id = first->id;
    const auto last_id = last->id;
    const auto owner = a11y_snapshot::bounds(browser);
    const auto one = a11y_snapshot::bounds(first);
    const auto two = a11y_snapshot::bounds(second);
    expect(one[2] > 0 && one[3] > 0 && two[2] > 0 && two[3] > 0 &&
               one[1] + one[3] <= two[1] && one != two,
           "semantic rows have positive distinct ordered nonoverlapping extents");
    for (const auto &row : {one, two})
      expect(row[0] >= owner[0] && row[1] >= owner[1] &&
                 row[0] + row[2] <= owner[0] + owner[2] &&
                 row[1] + row[3] <= owner[1] + owner[3],
             "visible row extents stay within the browser viewport");
    const auto title = a11y_snapshot::bounds(item);
    const auto bar_bounds = a11y_snapshot::bounds(menu_object);
    expect(title[2] > 0 && title[2] < bar_bounds[2] && title[3] > 0 &&
               title[0] >= bar_bounds[0] && title[0] + title[2] <= bar_bounds[0] + bar_bounds[2],
           "closed menu-bar titles retain their own header geometry, not the whole bar");
    list->topline(80);
    list->redraw();
    Fl::check();
    snapshot = tree.snapshot();
    const auto *first_now = a11y_snapshot::lookup(snapshot, first_id);
    const auto *last_now = a11y_snapshot::lookup(snapshot, last_id);
    expect(a11y_snapshot::bounds(first_now) == a11y_snapshot::Bounds{} &&
               !a11y_snapshot::showing(first_now),
           "a scrolled-out production row is zero-sized and not showing");
    const auto final = a11y_snapshot::bounds(last_now);
    expect(final[2] > 0 && final[3] > 0 && a11y_snapshot::showing(last_now),
           "scrolling reveals the final row's own geometry");
    expect(hits_agree(), "row bounds agree with hit testing at the end");
    const auto at_end = a11y::scroll_of(list_target);
    expect(at_top && at_end && at_end->vertical_view == at_top->vertical_view,
           "scrolling translates rows without changing content length or view size");
    expect(at_end && at_end->vertical == 100.0, "the final row scrolls the list to 100 percent");
    // Percentages written through the adaptor read back within one pixel.
    for (const double percent : {50.0, 0.0, 100.0, 25.0}) {
      expect(a11y::set_scroll(list_target, -1.0, percent), "a vertical scroll request applies");
      Fl::check();
      const auto read = a11y::scroll_of(list_target);
      expect(read && std::abs(read->vertical - percent) < 0.5,
             "a vertical scroll percentage round-trips");
      expect(read && at_top && read->vertical_view == at_top->vertical_view,
             "a scroll request leaves the visible share unchanged");
      expect(hits_agree(), "row bounds agree with hit testing after a scroll request");
    }
  }
  window.hide();
  Fl::check();
}
