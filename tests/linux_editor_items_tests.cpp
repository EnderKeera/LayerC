// Actual FLTK item controls in private fixtures, not physical-input evidence.
#include "linux_editor_test_access.hpp"
#include "a11y/fltk_semantics.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "platform/services.hpp"
#include "profile_edit.hpp"
#include "editor_test_environment.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Tabs.H>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace test = layerc::test;
using test::expect;

class Fixture final {
public:
  Fixture()
      : temp_("layerc-editor-items", layerc::test::EditorTestScope::widget,
              "item tests use a private control namespace") {}
private:
  layerc::test::EditorTestEnvironment temp_;
};

template <typename T>
T *field(Fl_Group &group, const std::string &label) {
  for (int index = 0; index < group.children(); ++index) {
    auto *child = group.child(index);
    if (auto *value = dynamic_cast<T *>(child);
        value && child->label() && label == child->label()) return value;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *value = field<T>(*nested, label)) return value;
  }
  return nullptr;
}
template <typename T>
T &required(Fl_Group &group, const std::string &label) {
  auto *value = field<T>(group, label);
  if (!expect(value != nullptr, "item control exists: " + label))
    throw std::runtime_error("Missing item control: " + label);
  return *value;
}
void choose(Fl_Group &group, const std::string &label, const int value) {
  auto &choice = required<Fl_Choice>(group, label);
  choice.value(value); choice.do_callback();
}
void click(Fl_Group &group, const std::string &label) {
  required<Fl_Button>(group, label).do_callback();
}

class ModalReply final {
public:
  using Step = std::function<void(Fl_Group &)>;
  explicit ModalReply(std::vector<Step> steps) : steps_(std::move(steps)) {
    Fl::add_timeout(.001, tick, this);
  }
  ~ModalReply() { Fl::remove_timeout(tick, this); }
  [[nodiscard]] bool delivered() const { return position_ == steps_.size(); }
private:
  static void tick(void *data) {
    auto &self = *static_cast<ModalReply *>(data);
    if (auto *modal = Fl::modal()) {
      self.steps_[self.position_++](*modal);
      if (self.position_ == self.steps_.size()) {
        if (modal->shown()) {
          expect(false, "last modal test step must dismiss the item dialog");
          modal->hide();
        }
        return;
      }
    } else if (++self.attempts_ >= 100) return;
    Fl::add_timeout(.001, tick, &self);
  }
  std::vector<Step> steps_;
  std::size_t position_{};
  int attempts_{};
};

void setup(editor::EditorApp &app) {
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  app.command(editor::EditorCommand::new_profile);
  expect(!app.session.edit_items([](auto &profile, auto &automation)
      -> std::optional<std::string> {
    profile.game.executable_name = "items-fixture.exe";
    profile.cooldowns.front().active_duration = core::Milliseconds(2000);
    profile.panels.front().cooldown_ids = {profile.cooldowns.front().id};
    core::AutomationProfile graph;
    graph.profile_id = profile.id;
    graph.variables = {
        {.id = "count", .type = core::AutomationValueType::integer,
         .default_value = core::AutomationValue(std::int64_t{5}), .persistent = false},
        {.id = "flag", .type = core::AutomationValueType::boolean,
         .default_value = core::AutomationValue(true), .persistent = false},
        {.id = "ratio", .type = core::AutomationValueType::number,
         .default_value = core::AutomationValue(1.5), .persistent = false},
        {.id = "title", .type = core::AutomationValueType::string,
         .default_value = core::AutomationValue(std::string("ignored")),
         .persistent = false}};
    core::AutomationAction action;
    action.kind = core::AutomationActionKind::create_timer;
    action.target = "cast";
    action.value = core::expr::literal(
        core::AutomationValue(core::DurationValue{core::Milliseconds(5000)}));
    core::AutomationRule rule;
    rule.id = "start"; rule.event = core::AutomationEventKind::session_started;
    rule.actions.push_back(std::move(action));
    core::AutomationDefinition definition;
    definition.id = "startup"; definition.name = "Startup";
    definition.rules.push_back(std::move(rule));
    graph.automations.push_back(std::move(definition));
    automation = std::move(graph);
    return std::nullopt;
  }), "item fixture installs one validated base/module transaction");
  app.refresh();
  auto *tabs = editor::EditorAppTestAccess::tabs(app);
  tabs->value(editor::EditorAppTestAccess::panel_tab(app));
  app.tab_changed();
  Fl::check();
}
void add(editor::EditorApp &app, const int kind,
         const std::function<void(Fl_Group &)> &configure = {}) {
  ModalReply reply({[&](Fl_Group &dialog) {
    choose(dialog, "Item type", kind);
    if (configure) configure(dialog);
    click(dialog, "Apply item");
  }});
  editor::EditorAppTestAccess::add_item(app)->do_callback();
  expect(reply.delivered(), "the real Add item dialog applied its field buffer");
}
const core::Panel &panel(const editor::EditorApp &app) {
  return app.session.document().profile().panels.front();
}
} // namespace

void profile_identity_and_browse() {
  Fixture fixture;
  auto profile = editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  const auto path = layerc::platform::profile_directory() / "identity.json";
  expect(layerc::platform::write_profile_file(path, core::serialize_profile_json(profile)).ok(),
         "seed the private profile store");
  editor::EditorApp app;
  app.build();
  using Access = editor::EditorAppTestAccess;
  auto *id = Access::profile_id(app);
  auto *browse = Access::browse_application(app);
  auto *status = Access::status(app);
  if (!expect(id && status && app.session.has_document(), "profile controls and document exist")) return;
  const auto accepted = [&]() -> const core::GameProfile & { return app.session.document().profile(); };
  if constexpr (!editor::windows_editor_keys) {
    expect(id->readonly() && std::string_view(id->label()) == "Profile ID (read-only)" && !browse,
           "Linux keeps read-only Profile ID and no Browse button");
    id->value("local-only");
    app.field_changed(id);
    expect(accepted().id == profile.id && !app.session.can_undo(),
           "Linux's displayed ID cannot edit the profile");
    return;
  }
  expect(!id->readonly() && std::string_view(id->label()) == "Profile ID",
         "Windows shows an editable Profile ID");
  id->value("new"); id->do_callback();
  id->value("new-id"); id->do_callback();
  expect(accepted().id == "new-id" && app.session.can_undo(), "accepted ID typing edits the document");
  app.command(editor::EditorCommand::undo);
  expect(accepted().id == profile.id && id->value() == std::string_view(profile.id) &&
             !app.session.can_undo(), "one Undo restores the ID and field");
  app.command(editor::EditorCommand::redo);
  expect(accepted().id == "new-id" && id->value() == std::string_view("new-id"),
         "Redo restores the ID and field");
  id->value(""); id->do_callback();
  expect(accepted().id == "new-id" && std::string_view(status->value()).starts_with("Invalid input: $.id:"),
         "empty ID is refused with the ID validation status");
  id->value("new-id"); id->do_callback();
  expect(app.save() && app.session.document().path() == path && !app.session.open_path(path) &&
             accepted().id == "new-id", "Save and reopen preserve the new ID using the same file");
  app.refresh();

  constexpr std::string_view tip = "Choose the game's .exe file";
  expect(browse && browse->tooltip() && browse->tooltip() == tip &&
             editor::a11y::description({browse, editor::a11y::TargetKind::widget}) == tip,
         "Browse tooltip and accessible description match Win32");
  auto *executable = Access::executable(app);
  auto *path_field = Access::executable_path(app);
  if (!expect(browse && executable && path_field, "application controls exist")) return;
  std::optional<std::string> chosen;
  Access::set_application_chooser(app, [&] { return chosen; });
  browse->do_callback();
  expect(accepted().game == profile.game && !app.session.has_unsaved_changes(),
         "Cancel changes nothing");
  const auto application = path.parent_path() / "another.exe";
  chosen = layerc::platform::path_to_utf8(application);
  if (!expect(chosen.has_value(), "the private fixture path is representable as UTF-8")) return;
  browse->do_callback();
  expect(accepted().game == profile.game && !app.session.has_unsaved_changes(),
         "a nonexistent selection changes nothing");
  expect(std::ofstream(application).good(), "create a disposable application file");
  browse->do_callback();
  expect(accepted().game.executable_name == "another.exe" && accepted().game.executable_path.empty() &&
             std::string_view(executable->value()) == "another.exe" &&
             std::string_view(status->value()) == "Application set to another.exe.",
         "Browse without exact matching sets only the executable name and reports it");
  app.command(editor::EditorCommand::undo);
  expect(accepted().game == profile.game, "one Undo restores the original executable");
  path_field->value("C:\\Games\\fixture.exe"); path_field->do_callback();
  const auto before_browse = accepted().game;
  browse->do_callback();
  expect(accepted().game.executable_name == "another.exe" && accepted().game.executable_path == *chosen &&
             path_field->value() == std::string_view(*chosen) &&
             path_field->tooltip() && std::string_view(path_field->tooltip()) == "Matches exactly " + *chosen,
         "Browse replaces the exact match and refreshes its description");
  app.command(editor::EditorCommand::undo);
  expect(accepted().game == before_browse, "one Undo restores both application fields");
  app.command(editor::EditorCommand::undo);
  app.command(editor::EditorCommand::new_profile);
  expect(app.session.is_draft() && accepted().game.executable_name.empty(),
         "a newly created draft has no application yet");
  browse->do_callback();
  expect(accepted().game.executable_name == "another.exe" && accepted().game.executable_path.empty(),
         "Browse can supply the first application on a new draft");
  if (!expect(!app.session.edit([](auto &value) { value.game.global = true; }),
              "set global matching on the draft")) return;
  app.refresh();
  browse->do_callback();
  expect(!accepted().game.global, "Browse changes a global profile to application matching");
}

void disabled_action_reasons() {
  Fixture fixture;
  editor::EditorApp app;
  setup(app);
  using Access = editor::EditorAppTestAccess;
  const auto reason = [](Fl_Button &button, const std::string_view text) {
    return !button.active() && button.tooltip() && button.tooltip() == text &&
        editor::a11y::description({&button, editor::a11y::TargetKind::widget}) == text;
  };
  const auto shows = [](Fl_Box &line, const std::string_view text) {
    return line.visible() && line.label() && line.label() == text;
  };
  auto *cooldown_remove = Access::remove_cooldown(app);
  auto *cooldown_reason = Access::remove_cooldown_reason(app);
  if (!expect(cooldown_remove && cooldown_reason, "cooldown action and reason exist")) return;
  constexpr std::string_view cooldown = "A profile needs at least one cooldown.";
  expect(reason(*cooldown_remove, cooldown) && shows(*cooldown_reason, cooldown),
         "last cooldown has the visible, tooltip and accessible reason");
  app.command(editor::EditorCommand::add_cooldown);
  expect(cooldown_remove->active() && !cooldown_remove->tooltip() && !cooldown_reason->visible() &&
             editor::a11y::description({cooldown_remove, editor::a11y::TargetKind::widget}).empty(),
         "second cooldown clears every reason and enables Remove");
  constexpr std::string_view last = "A panel needs at least one item.";
  const std::string opaque = "This item kind is kept as it is and can't be edited here.";
  const std::string full = "A panel holds at most " +
      std::to_string(core::max_overlay_items_per_panel) + " items.";
  auto *line = Access::item_reason(app);
  auto *remove = Access::remove_item(app);
  auto *edit = Access::edit_item(app);
  auto *add = Access::add_item(app);
  if (!expect(line && remove && edit && add, "panel item actions and reason exist")) return;
  expect(reason(*remove, last) && shows(*line, last),
         "last panel item cannot be removed");
  const auto populate = [&](const std::size_t count) {
    expect(!app.session.edit_items([&](auto &candidate, auto &) -> std::optional<std::string> {
      using Value = core::json::Value;
      auto &target = candidate.panels.front();
      core::OverlayItem row;
      row.content = core::OpaqueOverlayItem{Value(Value::Object{{"kind", Value(std::string("future"))}})};
      target.items.emplace(count, row);
      target.cooldown_ids.clear();
      return std::nullopt;
    }), "seed the opaque panel rows");
    app.refresh();
  };
  populate(1);
  expect(reason(*edit, opaque) && reason(*remove, last) &&
             shows(*line, opaque + " " + std::string(last)),
         "opaque last item shows Edit and Remove reasons side by side in one line");
  populate(core::max_overlay_items_per_panel);
  expect(reason(*add, full) && reason(*edit, opaque) && remove->active() && shows(*line, full + " " + opaque),
         "full panel shows Add and Edit reasons in order and permits Remove");
  Access::window(app).hide();
}

void linux_editor_typed_item_controls() {
  Fixture fixture;
  editor::EditorApp app;
  setup(app);
  const auto original = core::serialize_profile_json(app.session.document().profile());
  expect(!panel(app).items && editor::EditorAppTestAccess::items(app)->size() == 1,
         "viewing legacy rows neither promotes nor allocates IDs");
  app.refresh();
  expect(core::serialize_profile_json(app.session.document().profile()) == original,
         "an unchanged item-list refresh is byte stable");

  add(app, 3, [](Fl_Group &dialog) {
    required<Fl_Input>(dialog, "Counter maximum (optional)").value("8");
    choose(dialog, "Counter display", 1);
    auto &custom = required<Fl_Check_Button>(dialog, "Custom label");
    custom.value(1); custom.do_callback();
    required<Fl_Input>(dialog, "Item label").value("Charges / Δ & pips");
  });
  expect(panel(app).items && panel(app).items->size() == 2 &&
         std::holds_alternative<core::CounterItem>(panel(app).items->back().content),
         "adding a counter promotes the legacy row exactly once");
  const auto legacy_id = panel(app).items->front().id;
  const auto counter_id = panel(app).items->back().id;
  expect(legacy_id != core::legacy_item_id(0) && !counter_id.empty(),
         "persisted typed rows use stable explicit IDs");
  const auto counter = std::get<core::CounterItem>(panel(app).items->back().content);
  expect(counter.variable == "count" && counter.max == 8 &&
         counter.display == core::CounterDisplay::pips &&
         panel(app).items->back().label == "Charges / Δ & pips",
         "counter source, maximum, display and authored label reach the document");
  add(app, 4, [](Fl_Group &dialog) {
    required<Fl_Input>(dialog, "Flash Hz (0 = off)").value("2");
  });
  add(app, 2);
  add(app, 1);
  expect(panel(app).items->size() == 5 &&
         std::get<core::IndicatorItem>((*panel(app).items)[2].content).flash_hz == 2.0 &&
         std::holds_alternative<core::CooldownActiveTimerItem>((*panel(app).items)[3].content) &&
         std::get<core::AutomationTimerItem>((*panel(app).items)[4].content).timer_id == "cast",
         "indicator and both timer sources are editable from the real controls");

  editor::EditorAppTestAccess::items(app)->value(2);
  editor::EditorAppTestAccess::items(app)->do_callback();
  editor::EditorAppTestAccess::item_up(app)->do_callback();
  expect(panel(app).items->front().id == counter_id &&
         (*panel(app).items)[1].id == legacy_id,
         "moving a row preserves all item identities");
  app.command(editor::EditorCommand::undo);
  expect(panel(app).items->front().id == legacy_id &&
         (*panel(app).items)[1].id == counter_id, "one Undo restores the row order");
  app.command(editor::EditorCommand::redo);
  expect(panel(app).items->front().id == counter_id, "one Redo reapplies row order");

  const auto before = *panel(app).items;
  editor::EditorAppTestAccess::items(app)->value(1);
  editor::EditorAppTestAccess::items(app)->do_callback();
  {
    ModalReply cancel({[](Fl_Group &dialog) {
      required<Fl_Input>(dialog, "Counter maximum (optional)").value("7");
      click(dialog, "Cancel");
    }});
    editor::EditorAppTestAccess::edit_item(app)->do_callback();
    expect(cancel.delivered() && *panel(app).items == before,
           "Cancel preserves accepted data and IDs");
  }
  bool refused = false;
  {
    ModalReply invalid({
      [](Fl_Group &dialog) {
        choose(dialog, "Item type", 4);
        required<Fl_Input>(dialog, "Flash Hz (0 = off)").value("5");
        click(dialog, "Apply item");
      },
      [&](Fl_Group &dialog) {
        refused = field<Fl_Box>(dialog, "Flash Hz must be 0 (off), or between 0.5 and 3.") != nullptr;
        click(dialog, "Cancel");
      }});
    editor::EditorAppTestAccess::add_item(app)->do_callback();
    expect(invalid.delivered() && refused && *panel(app).items == before,
           "out-of-range flash leaves the modal open and changes no document data");
  }
  {
    (void)app.session.edit_items([&](auto &profile, auto &) -> std::optional<std::string> {
      std::get<core::IndicatorItem>((*profile.panels[0].items)[2].content).flash_hz = 3.5;
      return std::nullopt;
    });
    app.refresh();
    editor::EditorAppTestAccess::items(app)->value(3);
    editor::EditorAppTestAccess::items(app)->do_callback();
    bool warned = false;
    ModalReply high_flash({[&](Fl_Group &dialog) {
      warned = field<Fl_Box>(dialog, "Flashes at 3 Hz; faster flashing is capped.") != nullptr;
      click(dialog, "Apply item");
    }});
    editor::EditorAppTestAccess::edit_item(app)->do_callback();
    expect(high_flash.delivered() && warned &&
               std::get<core::IndicatorItem>((*panel(app).items)[2].content).flash_hz == 3.5,
           "loaded indicator above 3 Hz shows the cap warning and stays in the profile");
    (void)app.session.edit_items([&](auto &profile, auto &) -> std::optional<std::string> {
      std::get<core::IndicatorItem>((*profile.panels[0].items)[2].content).flash_hz = 2.0;
      return std::nullopt;
    });
    app.refresh();
  }

  const auto automation_before = app.session.document().automation();
  add(app, 5, [](Fl_Group &dialog) {
    required<Fl_Input>(dialog, "Preset seconds").value("1.25");
  });
  expect(panel(app).items->size() == before.size() + 1 &&
         app.session.document().automation()->automations.size() ==
             automation_before->automations.size() + 1,
         "timer preset adds its item and automation in the same edit");
  app.command(editor::EditorCommand::undo);
  expect(*panel(app).items == before &&
         app.session.document().automation() == automation_before,
         "one Undo atomically removes the preset item and its complete automation");
  app.command(editor::EditorCommand::redo);
  expect(panel(app).items->size() == before.size() + 1,
         "one Redo atomically restores the preset");

  const auto profile_before_remove = app.session.document().profile();
  editor::EditorAppTestAccess::items(app)->value(1);
  editor::EditorAppTestAccess::items(app)->do_callback();
  editor::EditorAppTestAccess::remove_item(app)->do_callback();
  expect(panel(app).items->size() == before.size(), "Remove item removes exactly one row");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile() == profile_before_remove,
         "Undo restores removed content and its stable ID");
  // Indicator sources: Boolean, integer and number variables. A new indicator
  // still defaults to the first Boolean source, not the first numeric one.
  // The dialog prefixes each choice with its ordinal ("[1] count").
  const auto source_label = [](Fl_Choice &choice, const int index) {
    const char *text = choice.text(index);
    const std::string label = text == nullptr ? std::string{} : std::string(text);
    const auto bracket = label.find("] ");
    return bracket == std::string::npos ? label : label.substr(bracket + 2);
  };
  const auto find_source = [&](Fl_Choice &choice, const std::string &id) {
    for (int index = 0; index < choice.size(); ++index)
      if (id == source_label(choice, index)) return index;
    return -1;
  };
  {
    const auto items_before = *panel(app).items;
    std::vector<std::string> sources;
    std::string default_source;
    ModalReply inspect({[&](Fl_Group &dialog) {
      choose(dialog, "Item type", 4);
      auto &source = required<Fl_Choice>(dialog, "Source");
      for (int index = 0; index < source.size(); ++index)
        sources.push_back(source_label(source, index));
      if (source.value() >= 0) default_source = source_label(source, source.value());
      click(dialog, "Cancel");
    }});
    editor::EditorAppTestAccess::add_item(app)->do_callback();
    expect(inspect.delivered() && *panel(app).items == items_before,
           "inspecting indicator sources cancels without changing the document");
    expect(std::ranges::find(sources, std::string("count")) != sources.end() &&
               std::ranges::find(sources, std::string("ratio")) != sources.end() &&
               std::ranges::find(sources, std::string("flag")) != sources.end() &&
               std::ranges::find(sources, std::string("title")) == sources.end(),
           "indicator sources include Boolean, integer and number variables, not strings");
    expect(default_source == "flag",
           "a new indicator still defaults to the first Boolean source");
  }
  const auto base_size = panel(app).items->size();
  const auto add_indicator = [&](const std::string &variable) {
    ModalReply reply({[&](Fl_Group &dialog) {
      choose(dialog, "Item type", 4);
      auto &source = required<Fl_Choice>(dialog, "Source");
      const int index = find_source(source, variable);
      expect(index >= 0, "the indicator source is offered by the real dialog");
      source.value(index);
      source.do_callback();
      click(dialog, "Apply item");
    }});
    editor::EditorAppTestAccess::add_item(app)->do_callback();
    expect(reply.delivered(), "the real Add item dialog applied the indicator source");
  };
  add_indicator("count");
  add_indicator("ratio");
  if (!expect(panel(app).items->size() == base_size + 2,
              "two indicator items are appended by the real dialog"))
    return;
  const auto count_item = (*panel(app).items)[base_size];
  const auto ratio_item = (*panel(app).items)[base_size + 1];
  expect(std::holds_alternative<core::IndicatorItem>(count_item.content) &&
             std::get<core::IndicatorItem>(count_item.content).variable == "count" &&
             std::holds_alternative<core::IndicatorItem>(ratio_item.content) &&
             std::get<core::IndicatorItem>(ratio_item.content).variable == "ratio",
         "integer and number variables are accepted indicator sources");
  const auto indicator_preview =
      editor::EditorAppTestAccess::preview_panel(app, panel(app));
  expect(indicator_preview.presentation.items.size() == base_size + 2 &&
             indicator_preview.presentation.items[base_size].active &&
             indicator_preview.presentation.items[base_size + 1].active,
         "count and ratio indicator previews are active from their defaults");
  // Fl_Hold_Browser values are 1-based: row n is the n-1 item.
  editor::EditorAppTestAccess::items(app)->value(static_cast<int>(base_size) + 2);
  editor::EditorAppTestAccess::items(app)->do_callback();
  std::string retained;
  {
    ModalReply cancel({[&](Fl_Group &dialog) {
      auto &source = required<Fl_Choice>(dialog, "Source");
      if (source.value() >= 0) retained = source_label(source, source.value());
      const int index = find_source(source, "count");
      expect(index >= 0, "an existing indicator can switch to another numeric source");
      source.value(index);
      source.do_callback();
      click(dialog, "Cancel");
    }});
    editor::EditorAppTestAccess::edit_item(app)->do_callback();
    expect(cancel.delivered(), "the real Edit item dialog responded to Cancel");
  }
  expect(retained == "ratio",
         "editing an indicator preserves its selected source: " + retained);
  expect((*panel(app).items)[base_size + 1] == ratio_item,
         "Cancel discards an indicator source change");
  editor::EditorAppTestAccess::window(app).hide();
}

void linux_editor_item_preview_and_active_duration() {
  Fixture fixture;
  editor::EditorApp app;
  setup(app);
  auto *tabs = editor::EditorAppTestAccess::tabs(app);
  tabs->value(editor::EditorAppTestAccess::cooldown_tab(app)); app.tab_changed();
  auto *duration = editor::EditorAppTestAccess::active_duration(app);
  duration->value("1.25"); app.field_changed(duration);
  expect(app.session.document().profile().cooldowns.front().active_duration ==
             core::Milliseconds(1250), "active duration edits milliseconds without automation");
  duration->value("-1"); app.field_changed(duration);
  expect(app.session.document().profile().cooldowns.front().active_duration ==
             core::Milliseconds(1250), "invalid duration leaves accepted timing unchanged");
  app.command(editor::EditorCommand::undo); // explicitly discards the invalid field buffer
  tabs->value(editor::EditorAppTestAccess::panel_tab(app)); app.tab_changed();
  add(app, 3, [](Fl_Group &dialog) {
    required<Fl_Input>(dialog, "Counter maximum (optional)").value("8");
    choose(dialog, "Counter display", 1);
  });
  add(app, 4, [](Fl_Group &dialog) {
    required<Fl_Input>(dialog, "Flash Hz (0 = off)").value("2");
  });
  add(app, 1);
  const auto ready = editor::EditorAppTestAccess::preview_panel(app, panel(app));
  expect(ready.presentation.items.size() == 4 &&
         ready.presentation.items[1].filled_pips == 5 &&
         ready.presentation.items[2].active &&
         !ready.presentation.items[3].active,
         "local preview uses typed core defaults, including pips and the active indicator");

  tabs->value(editor::EditorAppTestAccess::automation(app)->group()); app.tab_changed();
  auto &automation_page = *editor::EditorAppTestAccess::automation(app)->group();
  choose(automation_page, "Automation view", 1);
  click(automation_page, "Step");
  const auto simulated = editor::EditorAppTestAccess::preview_panel(app, panel(app));
  expect(simulated.presentation.items.size() == 4 &&
         simulated.presentation.items[3].active &&
         simulated.presentation.items[3].remaining_time == core::Milliseconds(5000),
         "the actual Linux simulator supplies the live timer presentation to preview");
  editor::EditorAppTestAccess::window(app).hide();
}
