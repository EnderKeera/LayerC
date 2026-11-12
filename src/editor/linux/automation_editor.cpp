#include "automation_editor.hpp"
#include "automation_trace_client.hpp"
#include <exception>
#include "editor_app.hpp"
#include "automation_graph.hpp"
#include "automation_simulator.hpp"

#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace layerc::editor {
namespace core = layerc::core;
namespace {
// The shared JSON writer emits CRLF for Windows edit controls; FLTK would
// render each CR as a visible ^M, and the parser accepts plain LF.
std::string display_lines(std::string text) {
  std::erase(text, '\r');
  return text;
}
void append_choice(Fl_Choice &choice, const std::string_view label) {
  const auto index = choice.add(("__item_" + std::to_string(choice.size())).c_str());
  choice.replace(index, std::string(label).c_str());
}
std::optional<std::filesystem::path> trace_destination(const bool save) {
  Fl_Native_File_Chooser chooser;
  chooser.title(save ? "Save automation trace" : "Load automation trace");
  if (const auto *home = std::getenv("HOME"); home && *home) chooser.directory(home);
  chooser.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
  chooser.filter("LayerC trace\t*.layerctrace");
  if (save) chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  if (chooser.show() || !chooser.filename() || !*chooser.filename()) return std::nullopt;
  return std::filesystem::path(chooser.filename());
}
} // namespace

struct LinuxAutomationEditor::Impl {
  enum class Pending { none, basic, json };
  EditorApp &app;
  Fl_Group *page{}, *basic{}, *advanced{}, *simulation{}, *live_view{}, *properties{}, *action_fields{};
  Fl_Hold_Browser *automations{}, *nodes{};
  Fl_Choice *view{}, *preset{}, *palette{}, *event{}, *action{}, *sim_event{}, *speed{};
  Fl_Input *name{}, *rule_id{}, *source{}, *priority{}, *target{}, *value{};
  Fl_Input *sim_source{}, *sim_fields{}, *breakpoint{}, *assertions{};
  Fl_Check_Button *enabled{}, *repeating{}, *trace_enabled{};
  Fl_Multiline_Input *json{};
  Fl_Output *validation{};
  Fl_Multiline_Output *state{}, *trace_state{};
  Fl_Scroll *state_scroll{};
  Fl_Button *create{}, *remove{}, *add{}, *erase{}, *apply{}, *json_apply{};
  Fl_Button *reset{}, *step{}, *play{}, *event_add{}, *load_trace{}, *save_trace{}, *trace_replay{};
  AutomationSelection selection;
  std::vector<AutomationNodeRow> rows;
  AutomationSimulator simulator;
  LiveTraceLog trace;
  std::unique_ptr<AutomationTraceClient> trace_client;
  bool trace_gap{};
  std::string profile_id;
  std::string simulated_graph; // graph JSON the simulator was last reset with
  std::optional<core::GameProfile> simulated_profile;
  Pending pending{Pending::none};
  int current_view{};
  bool playing{};

  Impl(EditorApp &owner, Fl_Group &group) : app(owner), page(&group) {
    page->begin();
    view = choice(285, 95, 170, "Automation view");
    for (const auto label : automation_view_labels) append_choice(*view, label);
    append_choice(*view, "Live trace");
    view->value(0);
    preset = choice(475, 95, 260, "Starter preset");
    for (const auto label : automation_preset_labels) append_choice(*preset, label);
    preset->value(0);
    create = button(750, 95, 120, "New automation");
    remove = button(880, 95, 150, "Remove automation");

    basic = new Fl_Scroll(270, 130, 775, 265);
    basic->begin();
    automations = browser(285, 165, 215, 200, "Automations");
    name = input(520, 165, 325, "Automation name");
    enabled = check(860, 165, 170, "Automation enabled");
    palette = choice(520, 230, 325, "Node palette");
    for (const auto &entry : automation_palette) append_choice(*palette, entry.label);
    palette->value(0);
    add = button(860, 230, 75, "Add node");
    erase = button(945, 230, 85, "Delete node");
    nodes = browser(520, 290, 510, 110, "Rules and actions");
    properties = new Fl_Group(520, 425, 510, 280);
    properties->begin();
    rule_id = input(520, 450, 245, "Rule id");
    event = choice(785, 450, 245, "Event");
    for (const auto kind : core::automation_event_kinds) append_choice(*event, event_label(kind));
    source = input(520, 515, 245, "Source filter");
    priority = input(785, 515, 245, "Priority");
    action_fields = new Fl_Group(520, 545, 510, 160);
    action_fields->begin();
    action = choice(520, 580, 245, "Action kind");
    for (const auto &spec : core::automation_action_specs) append_choice(*action, spec.display_name);
    target = input(785, 580, 245, "Action target");
    value = input(520, 645, 325, "Typed JSON value (expressions kept)");
    repeating = check(860, 645, 170, "Repeating timer");
    action_fields->end();
    properties->end();
    apply = button(520, 725, 245, "Apply properties");
    basic->end();

    advanced = new Fl_Group(270, 130, 775, 335);
    advanced->begin();
    json = new EditorField<Fl_Multiline_Input>(285, 165, 745, 185, "Advanced automation JSON");
    json->align(FL_ALIGN_TOP_LEFT);
    json->textfont(FL_COURIER);
    json->textsize(FL_NORMAL_SIZE);
    json->when(FL_WHEN_CHANGED);
    bind(*json);
    json_apply = button(285, 365, 220, "Validate / Apply JSON");
    advanced->end();

    simulation = new Fl_Group(270, 130, 775, 335);
    simulation->begin();
    sim_event = choice(285, 165, 225, "Synthetic event");
    for (const auto kind : core::automation_event_kinds) append_choice(*sim_event, event_label(kind));
    sim_event->value(0);
    sim_source = input(530, 165, 190, "Event source", false);
    sim_fields = input(740, 165, 290, "Fields: name=JSON; ...", false);
    event_add = button(285, 215, 135, "Add event");
    reset = button(430, 215, 100, "Reset");
    step = button(540, 215, 100, "Step");
    play = button(650, 215, 100, "Play");
    speed = choice(770, 215, 110, "Speed");
    for (const auto label : simulator_speed_labels) append_choice(*speed, label);
    speed->value(1);
    breakpoint = input(285, 275, 225, "Breakpoint rule id", false);
    assertions = input(530, 275, 500, "Assertions: variable=JSON; ...", false);
    state_scroll = new Fl_Scroll(279, 309, 757, 92);
    state_scroll->type(Fl_Scroll::VERTICAL);
    state_scroll->begin();
    state = new EditorField<Fl_Multiline_Output>(285, 330, 725, 65, "Simulator state");
    state->wrap(1);
    state->align(FL_ALIGN_TOP_LEFT);
    state->textfont(FL_COURIER);
    state->textsize(FL_NORMAL_SIZE);
    state_scroll->end();
    simulation->end();
    live_view = new Fl_Group(270, 130, 775, 270);
    live_view->begin();
    trace_enabled = check(285, 165, 650, "Record authenticated LayerD trace (saved profile only)");
    trace_replay = button(285, 215, 350, "Replay collected trace in simulator");
    trace_state = new EditorField<Fl_Multiline_Output>(285, 275, 745, 100, "Live trace status");
    trace_state->wrap(1);
    trace_state->align(FL_ALIGN_TOP_LEFT);
    trace_state->value("Stopped. No LayerD subscription.");
    live_view->end();
    page->end();
    // Trace files work in every view and never access a running daemon.
    page->begin();
    load_trace = button(285, 415, 160, "Load trace...");
    save_trace = button(460, 415, 160, "Save trace...");
    validation = new EditorField<Fl_Output>(645, 415, 385, editor_input_height, "Automation validation");
    validation->align(FL_ALIGN_TOP_LEFT);
    page->end();
    show_view();
  }
  ~Impl() { suspend(); }
  void bind(Fl_Widget &widget) {
    widget.callback([](Fl_Widget *w, void *data) { static_cast<Impl *>(data)->handle(w); }, this);
  }
  Fl_Button *button(int x, int y, int width, const char *label) {
    auto *widget = new Fl_Button(x, y, width, editor_button_height, label);
    bind(*widget);
    return widget;
  }
  Fl_Input *input(int x, int y, int width, const char *label, bool edit = true) {
    auto *widget = new EditorField<Fl_Input>(x, y, width, editor_input_height, label);
    widget->align(FL_ALIGN_TOP_LEFT);
    if (edit) { widget->when(FL_WHEN_CHANGED); bind(*widget); }
    return widget;
  }
  Fl_Choice *choice(int x, int y, int width, const char *label) {
    auto *widget = new EditorChoice(x, y, width, editor_input_height, label);
    widget->align(FL_ALIGN_TOP_LEFT);
    bind(*widget);
    return widget;
  }
  Fl_Check_Button *check(int x, int y, int width, const char *label) {
    auto *widget = new Fl_Check_Button(x, y, width, 28, label);
    bind(*widget);
    return widget;
  }
  Fl_Hold_Browser *browser(int x, int y, int width, int height, const char *label) {
    auto *widget = new Fl_Hold_Browser(x, y, width, height, label);
    widget->align(FL_ALIGN_TOP_LEFT);
    widget->format_char(0);
    bind(*widget);
    return widget;
  }
  void show_view() {
    basic->hide(); simulation->hide(); advanced->hide(); live_view->hide();
    if (current_view == 0) basic->show();
    if (current_view == 1) simulation->show();
    if (current_view == 2) advanced->show();
    if (current_view == 3) live_view->show();
    page->redraw();
  }
  void stop() {
    playing = false;
    Fl::remove_timeout(tick, this);
    play->copy_label("Play");
  }
  void stop_live(const std::string &status = "Stopped. No LayerD subscription.") {
    Fl::remove_timeout(trace_tick, this);
    trace_enabled->value(0);
    trace_client.reset(); // owned, joined worker; unsubscribes or lease expires
    trace_state->value(status.c_str());
    trace_state->redraw();
  }
  void suspend() { stop(); stop_live(); }
  static void trace_tick(void *data) {
    auto &self = *static_cast<Impl *>(data);
    if (!self.page->visible_r()) { self.stop_live(); return; }
    if (!self.trace_client) return;
    if (auto update = self.trace_client->take_update()) {
      if (!update->reply) {
        self.stop_live("Unavailable: " + update->error);
        self.message(StatusKind::failure, "Automation trace unavailable: " + update->error);
        return;
      }
      const auto &reply = *update->reply;
      if (!reply.packet.empty() &&
          std::holds_alternative<core::AutomationError>(core::parse_layerc_trace(reply.packet))) {
        self.stop_live("Unavailable: LayerD returned an invalid trace packet.");
        self.message(StatusKind::failure, "Automation trace packet was refused.");
        return;
      }
      if (reply.reset) { self.trace.clear(); self.trace_gap = false; }
      self.trace_gap = self.trace_gap || reply.gap;
      if (!reply.packet.empty()) (void)self.trace.append_packet(reply.packet);
      const auto status = "Recording saved LayerD profile. Roots: " +
          std::to_string(self.trace.events().size()) +
          (self.trace_gap ? "\nOlder trace events were lost from the bounded LayerD buffer." : "") +
          "\nTrace files may contain user-authored strings or maps.";
      self.trace_state->value(status.c_str());
      self.trace_state->redraw();
    }
    self.trace_client->request_poll();
    Fl::repeat_timeout(0.2, trace_tick, &self);
  }
  void set_pending(Pending next) {
    stop();
    pending = next;
    app.module_fields_changed(next != Pending::none);
    if (next != Pending::none) show_pending_validation();
  }
  // The validation box describes the pending buffer while one exists; refresh()
  // restores the committed summary when the buffer is applied or discarded.
  void show_pending_validation() {
    if (pending == Pending::json) {
      const auto text = std::string(json->value());
      auto parsed = core::parse_automation_profile_json(text, app.session.document().profile());
      if (const auto *failure = std::get_if<core::AutomationError>(&parsed)) {
        validation->value(("Pending JSON is invalid and not applied: " +
                           locate_json_error(text, *failure).message)
                              .c_str());
        return;
      }
      validation
          ->value(("Pending JSON: " +
                   automation_validation_summary(
                       std::optional<core::AutomationProfile>(
                           std::get<core::AutomationProfile>(parsed)),
                       app.session.document().profile()))
                      .c_str());
      return;
    }
    validation->value("Pending changes are not applied yet.");
  }
  void message(const StatusKind kind, const std::string &text,
               const StatusVoice voice = StatusVoice::announced) {
    app.report(kind, text, voice);
  }
  void describe() {
    app.redraw_preview();
    const auto text = simulator.describe(assertions->value());
    state->value(text.c_str());
    fl_font(state->textfont(), state->textsize());
    int width = 725 - 12;  // fl_measure wraps to a non-zero preset width
    int height{};
    fl_measure(text.c_str(), width, height, 0);
    state_scroll->scroll_to(0, 0);
    state->resize(state_scroll->x() + 6, state_scroll->y() + 21, 725,
                  std::max(65, height + 12));
    state_scroll->redraw();
  }
  // An absent module edits and simulates as an empty graph for this profile,
  // so the template carries the base profile ID the parser requires.
  [[nodiscard]] std::string current_graph_json() const {
    const auto &document = app.session.document();
    const auto &graph = document.automation();
    if (graph) return automation_graph_json(graph);
    // Assigned after construction: the aggregate has members without default
    // member initializers, so a brace form warns under -Wmissing-field-initializers.
    core::AutomationProfile empty;
    empty.profile_id = document.profile().id;
    return automation_graph_json(std::optional<core::AutomationProfile>(std::move(empty)));
  }
  void rewind() {
    stop();
    const auto result = simulator.reset(current_graph_json(), app.session.document().profile());
    if (result.error) message(StatusKind::failure, "Simulator unavailable: " + *result.error);
    else if (result.seeded) trace.record(*result.seeded);
    describe();
  }
  void refresh() {
    stop();
    if (pending != Pending::none) app.module_fields_changed(false);
    pending = Pending::none;
    if (!app.session.has_document()) {
      stop_live();
      profile_id.clear();
      simulated_graph.clear();
      simulated_profile.reset();
      trace.clear();
      simulator.clear();
      json->value(""); state->value(""); validation->value("");
      page->deactivate();
      return;
    }
    page->activate();
    const auto &document = app.session.document();
    if (profile_id != document.profile().id) {
      stop_live();
      profile_id = document.profile().id;
      selection = {};
      trace.clear();
      simulator.clear();
      simulated_graph.clear();
      simulated_profile.reset();
    }
    const auto &graph = document.automation();
    selection = clamp_selection(graph, selection);
    if (selection.automation < 0 && graph && !graph->automations.empty())
      selection = {0, -1, -1};
    automations->clear(); nodes->clear(); rows.clear();
    name->value(""); enabled->value(0);
    if (graph) {
      for (const auto &item : graph->automations)
        automations->add(automation_label(item).c_str());
      if (selection.automation >= 0) {
        const auto &item = graph->automations[static_cast<std::size_t>(selection.automation)];
        name->value(item.name.c_str()); enabled->value(item.enabled);
        rows = automation_node_rows(item);
        for (const auto &row : rows) nodes->add(row.label.c_str());
      }
    }
    automations->value(selection.automation + 1);
    nodes->value(0);
    for (std::size_t i = 0; i < rows.size(); ++i)
      if (rows[i].rule == selection.rule && rows[i].action == selection.action)
        nodes->value(static_cast<int>(i + 1));
    if (selection.automation < 0) {
      name->deactivate(); enabled->deactivate(); remove->deactivate(); add->deactivate();
    } else {
      name->activate(); enabled->activate(); remove->activate(); add->activate();
    }
    if (selection.rule < 0) { properties->deactivate(); erase->deactivate(); }
    else { properties->activate(); erase->activate(); }
    if (selection.action < 0) action_fields->deactivate();
    else action_fields->activate();
    const auto fields = graph ? automation_node_fields(*graph, selection) : AutomationNodeFields{};
    rule_id->value(fields.rule_id.c_str()); event->value(fields.event_index);
    source->value(fields.source_filter.c_str()); priority->value(fields.priority.c_str());
    action->value(fields.action_kind_index); target->value(fields.target.c_str());
    value->value(fields.value.c_str()); repeating->value(fields.repeating);
    auto graph_json = current_graph_json();
    json->value(display_lines(graph_json).c_str());
    validation->value(automation_validation_summary(graph, document.profile()).c_str());
    if (document.automation_state() == platform::ModuleSidecarState::invalid) {
      basic->deactivate(); advanced->deactivate(); create->deactivate(); remove->deactivate();
      validation->value("Unreadable automation module preserved; editing blocked.");
    } else { basic->activate(); advanced->activate(); create->activate(); }
    // Refreshing unchanged data preserves progress; graph or base edits rewind.
    if (graph_json != simulated_graph || !simulated_profile ||
        *simulated_profile != document.profile()) {
      simulated_graph = std::move(graph_json);
      simulated_profile = document.profile();
      rewind();
    }
    show_view();
  }
  std::optional<std::string> apply_pending() {
    if (pending == Pending::none) return std::nullopt;
    auto &session = app.session;
    std::optional<std::string> error;
    if (pending == Pending::json) {
      const auto source_text = std::string(json->value());
      auto parsed = core::parse_automation_profile_json(source_text, session.document().profile());
      if (const auto *failure = std::get_if<core::AutomationError>(&parsed)) {
        const auto location = locate_json_error(source_text, *failure);
        show_pending_validation();
        if (location.begin && location.end) {
          const auto bound = static_cast<std::size_t>(std::numeric_limits<int>::max());
          json->insert_position(static_cast<int>(std::min(*location.end, bound)),
                         static_cast<int>(std::min(*location.begin, bound)));
          json->take_focus();
        }
        return "Invalid automation JSON: " + location.message;
      }
      error = session.edit_automation([&](auto &graph) -> std::optional<std::string> {
        graph = std::move(std::get<core::AutomationProfile>(parsed));
        return std::nullopt;
      });
    } else {
      const AutomationNodeFields fields{
          .rule_id = rule_id->value(), .event_index = event->value(),
          .source_filter = source->value(), .priority = priority->value(),
          .action_kind_index = action->value(), .target = target->value(),
          .value = value->value(), .repeating = repeating->value() != 0,
          .advanced_value = std::string_view(value->value()) == advanced_expression_text};
      error = session.edit_automation([&](auto &graph) -> std::optional<std::string> {
        if (!graph || selection.automation < 0 ||
            static_cast<std::size_t>(selection.automation) >= graph->automations.size())
          return "Select an automation first.";
        auto &item = graph->automations[static_cast<std::size_t>(selection.automation)];
        item.name = name->value(); item.enabled = enabled->value() != 0;
        if (selection.rule >= 0)
          return apply_automation_node_fields(*graph, selection, fields, fields.advanced_value);
        return std::nullopt;
      });
    }
    if (error) {
      validation->value(("Pending changes are not applied: " + *error).c_str());
      return "Not applied: " + *error;
    }
    pending = Pending::none;
    app.module_fields_changed(false);
    refresh();
    message(StatusKind::success, "Automation applied to the document. Save publishes it.");
    return std::nullopt;
  }
  bool edit_graph(const std::function<AutomationEditResult(std::optional<core::AutomationProfile> &)> &edit,
                  const bool repair = false) {
    if (!app.apply_fields()) return false;
    auto next = selection;
    const auto error = app.session.edit_automation([&](auto &graph) -> std::optional<std::string> {
      const auto result = edit(graph);
      if (const auto *failure = std::get_if<std::string>(&result)) return *failure;
      next = std::get<AutomationSelection>(result);
      return std::nullopt;
    }, false, repair);
    if (error) { message(StatusKind::failure, "Not changed: " + *error); return false; }
    selection = next;
    refresh();
    message(StatusKind::information, "Unsaved automation changes. Save applies them.", StatusVoice::silent);
    return true;
  }
  static void tick(void *data) {
    auto &self = *static_cast<Impl *>(data);
    if (!self.playing || !self.app.shown() || !self.page->visible_r()) {
      self.stop(); return;
    }
    const bool more = self.simulator.step(self.breakpoint->value());
    self.describe();
    if (!more) { self.stop(); return; }
    Fl::repeat_timeout(static_cast<double>(simulator_interval(self.speed->value()).count()) / 1000.0,
                       tick, &self);
  }
  void trace_file(bool save) {
    suspend();
    if (save && trace.empty()) { message(StatusKind::failure, "No trace to save."); return; }
    fl_message_title("Save simulator trace");
    if (save && fl_choice("Trace files may contain user-authored strings or maps. Save?",
                          "Cancel", "Save", nullptr) != 1) return;
    const auto destination = trace_destination(save);
    if (!destination) return;
    if (save) {
      const bool saved = write_trace_file(*destination, trace.export_text());
      message(saved ? StatusKind::success : StatusKind::failure,
              saved ? "Trace saved." : "Trace write failed.");
      return;
    }
    auto loaded = read_trace_file(*destination);
    if (const auto *error = std::get_if<TraceLoadError>(&loaded)) {
      message(StatusKind::failure, error->message); return;
    }
    auto &record = std::get<LoadedTrace>(loaded);
    simulator.load(record.events);
    trace.assign(std::move(record.events), std::move(record.text));
    rewind();
    message(StatusKind::success, "Trace loaded as simulator roots.");
  }
  void handle(Fl_Widget *widget) {
    if (!app.session.has_document()) return;
    if (widget == view) {
      const auto next = view->value();
      if (!app.apply_fields()) { view->value(current_view); return; }
      stop(); if (next != 3) stop_live();
      current_view = next; show_view(); return;
    }
    if (widget == trace_enabled) {
      if (!trace_enabled->value()) { stop_live(); return; }
      try {
        // Like Windows: a recording starts a fresh log, never mixed with
        // seeded or synthetic simulator roots.
        trace.clear();
        trace_client = std::make_unique<AutomationTraceClient>(profile_id);
        trace_gap = false;
        trace_state->value("Connecting to authenticated local LayerD...");
        Fl::add_timeout(0.0, trace_tick, this);
      } catch (const std::exception &error) {
        stop_live(std::string("Unavailable: ") + error.what());
      }
      return;
    }
    if (widget == trace_replay) {
      if (trace.empty()) { message(StatusKind::failure, "No LayerD trace roots to replay."); return; }
      stop_live();
      simulator.load(trace.events());
      current_view = 1; view->value(1); show_view(); rewind();
      message(StatusKind::success, "LayerD trace copied to simulator. Local replay does not modify LayerD.");
      return;
    }
    if (widget == preset || widget == palette || widget == sim_event) return;
    if (widget == speed) { if (playing) { stop(); play->do_callback(); } return; }
    if (widget == create) {
      (void)edit_graph([&](auto &graph) {
        return add_automation_preset(app.session.document().profile(), graph,
                                     static_cast<AutomationPreset>(preset->value()));
      });
      return;
    }
    if (widget == remove || widget == erase) {
      fl_message_title("Remove automation");
      if (fl_choice(widget == remove ? "Remove this automation?" : "Remove this rule or action?",
                    "Cancel", "Remove", nullptr) != 1) return;
      (void)edit_graph([&](auto &graph) -> AutomationEditResult {
        return widget == remove ? remove_automation(graph, selection)
                                : delete_automation_node(graph, selection);
      }, true);
      return;
    }
    if (widget == add) {
      const auto *entry = automation_palette_entry(palette->value());
      if (entry) (void)edit_graph([&](auto &graph) {
        return add_palette_node(app.session.document().profile(), graph, selection, *entry);
      });
      return;
    }
    if (widget == automations || widget == nodes) {
      const int chosen = static_cast<Fl_Hold_Browser *>(widget)->value() - 1;
      const auto previous = selection;
      if (!app.apply_fields()) {
        automations->value(previous.automation + 1);
        for (std::size_t i = 0; i < rows.size(); ++i)
          if (rows[i].rule == previous.rule && rows[i].action == previous.action)
            nodes->value(static_cast<int>(i + 1));
        return;
      }
      if (widget == automations) selection = {chosen, -1, -1};
      else if (chosen >= 0 && static_cast<std::size_t>(chosen) < rows.size()) {
        selection.rule = rows[static_cast<std::size_t>(chosen)].rule;
        selection.action = rows[static_cast<std::size_t>(chosen)].action;
      }
      refresh(); return;
    }
    if (widget == apply || widget == json_apply) {
      if (const auto error = apply_pending()) message(StatusKind::failure, *error);
      return;
    }
    if (widget == load_trace || widget == save_trace) { trace_file(widget == save_trace); return; }
    if (widget == reset) { rewind(); return; }
    if (widget == step) { stop(); (void)simulator.step(breakpoint->value()); describe(); return; }
    if (widget == event_add) {
      const int index = sim_event->value();
      if (index < 0 || static_cast<std::size_t>(index) >= core::automation_event_kinds.size()) return;
      const auto result = simulator.add_event(core::automation_event_kinds[static_cast<std::size_t>(index)],
                                               sim_source->value(), sim_fields->value());
      if (const auto *error = std::get_if<std::string>(&result)) message(StatusKind::failure, *error);
      else { trace.record(std::get<core::AutomationEvent>(result)); message(StatusKind::success, "Synthetic event added."); }
      describe(); return;
    }
    if (widget == play) {
      if (playing) { stop(); return; }
      if (!simulator.compiled() || simulator.position() >= simulator.events().size()) {
        message(StatusKind::information, "Reset the simulator or add an event before playback."); return;
      }
      playing = true; play->copy_label("Pause");
      Fl::add_timeout(static_cast<double>(simulator_interval(speed->value()).count()) / 1000.0, tick, this);
      return;
    }
    set_pending(widget == json ? Pending::json : Pending::basic);
  }
};

LinuxAutomationEditor::LinuxAutomationEditor(EditorApp &app)
    : app_(app), page_(new Fl_Group(270, 45, 775, 405, "Automation")) {
  page_->end();
  page_->hide();
}
LinuxAutomationEditor::~LinuxAutomationEditor() = default;
Fl_Group *LinuxAutomationEditor::group() const { return page_; }
void LinuxAutomationEditor::refresh() {
  if (!impl_) {
    if (!app_.session.has_document()) { page_->deactivate(); return; }
    page_->activate();
    if (!page_->visible()) return; // no advanced widgets or graph copies until requested
    auto *previous = Fl_Group::current();
    impl_ = std::make_unique<Impl>(app_, *page_);
    Fl_Group::current(previous);
  }
  impl_->refresh();
}
std::optional<std::string> LinuxAutomationEditor::apply_pending() {
  return impl_ ? impl_->apply_pending() : std::nullopt;
}
void LinuxAutomationEditor::stop() { if (impl_) impl_->suspend(); }
std::optional<SimulatorPanelPreview>
LinuxAutomationEditor::panel_preview(const std::string_view panel_id) const {
  if (!impl_ || impl_->current_view != 1 || !app_.session.has_document() ||
      !impl_->simulated_profile ||
      *impl_->simulated_profile != app_.session.document().profile()) return std::nullopt;
  return impl_->simulator.panel_preview(panel_id);
}
} // namespace layerc::editor
