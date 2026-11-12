#include "visual_editor.hpp"

#include "accessible_widgets.hpp"
#include "calibration_window.hpp"
#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "layerc/platform/visual_assets_linux.hpp"

#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "editor_style.hpp"

namespace layerc::editor {
namespace core = layerc::core;
namespace platform = layerc::platform;
struct LinuxVisualEditor::Impl {
  EditorApp &app;
  Fl_Choice *target{};
  Fl_Check_Button *enabled{}, *isolated{};
  Fl_Button *calibrate{}, *remove{};
  Fl_Multiline_Output *summary{}, *hint{};
  std::string selected_id;

  Impl(EditorApp &owner, Fl_Group &page) : app(owner) {
    page.begin();
    target = new EditorChoice(290, 105, 730, editor_input_height, "Visual target cooldown");
    target->align(FL_ALIGN_TOP_LEFT);
    summary = new EditorField<Fl_Multiline_Output>(290, 165, 730, 90, "Visual assignment");
    summary->wrap(1);
    summary->align(FL_ALIGN_TOP_LEFT);
    enabled = new Fl_Check_Button(290, 265, 300, 30, "Visual trigger enabled");
    isolated = new Fl_Check_Button(620, 265, 400, 30, "Isolated visual runtime");
    calibrate = new Fl_Button(290, 310, 350, editor_button_height, "Calibrate visual...");
    remove = new Fl_Button(660, 310, 360, editor_button_height, "Remove visual assignment");
    hint = new EditorField<Fl_Multiline_Output>(290, 370, 730, 75, "Visual availability / preservation");
    hint->wrap(1);
    hint->align(FL_ALIGN_TOP_LEFT); hint->textsize(FL_NORMAL_SIZE);
    for (Fl_Widget *w : std::array<Fl_Widget *, 5>{target, enabled, isolated, calibrate, remove})
      w->callback([](Fl_Widget *widget, void *data) {
        static_cast<Impl *>(data)->handle(widget);
      }, this);
    page.end();
  }
  void refresh() {
    const auto &session = app.session;
    target->clear();
    if (!session.has_document()) return;
    const auto &document = session.document();
    const auto &profile = document.profile();
    if (std::ranges::find(profile.cooldowns, selected_id, &core::CooldownDefinition::id) == profile.cooldowns.end())
      selected_id = profile.cooldowns.empty() ? "" : profile.cooldowns.front().id;
    for (const auto &cooldown : profile.cooldowns) {
      const auto index = target->add("item");
      target->replace(index, cooldown.name.c_str());
      if (cooldown.id == selected_id) target->value(index);
    }
    const auto *trigger = document.visual() ? find_visual_trigger(*document.visual(), selected_id) : nullptr;
    const bool invalid = document.visual_state() == platform::ModuleSidecarState::invalid;
    std::string text = "No visual trigger.";
    if (invalid) text = "Invalid visual settings — preserved; explicit calibration can repair them.\n" + document.visual_error();
    else if (profile.game.global) text = "Unavailable: visual capture is not supported for global profiles.";
    else if (trigger) {
      text = trigger->mode == core::VisualTriggerMode::pixel_only ? "Pixel only" : "Confirm input";
      if (const auto *observation = core::find_observation(*document.visual(), trigger->observation_id)) {
        text += std::holds_alternative<core::PixelRule>(*observation) ? " — Pixel: " : " — Template: ";
        text += std::visit([](const auto &item) { return item.name; }, *observation);
        text += "\nObservation: " + trigger->observation_id;
      }
      if (document.visual_state() == platform::ModuleSidecarState::recovered_backup) text += " (recovered backup)";
    }
    summary->value(text.c_str());
    enabled->value(trigger && trigger->enabled);
    isolated->value(profile.visual_runtime == core::VisualRuntime::isolated);
    if (trigger && !invalid && !profile.game.global) { enabled->activate(); remove->activate(); }
    else { enabled->deactivate(); remove->deactivate(); }
    if (!selected_id.empty() && !profile.game.global && !document.path().empty()) calibrate->activate();
    else calibrate->deactivate();
    if (profile.game.global) isolated->deactivate(); else isolated->activate();
    std::string reason;
    if (selected_id.empty()) reason = "Select or add a cooldown before calibration.\n";
    else if (document.path().empty()) reason = "Save the profile once before calibration.\n";
    if (profile.visual_runtime == core::VisualRuntime::isolated)
      reason += "Unavailable: the isolated visual worker is Windows-only; this setting is retained, never silently weakened.\n";
    else reason += "X11 capture uses Composite/Damage, with a bounded full-frame fallback.\n";
    reason += "Enabled, Remove and runtime edits are drafts until Save. Calibration Apply validates and commits.\n"
              "Imported modules, shared observations and unsupported data are preserved.";
    hint->value(reason.c_str());
    // The availability line explains Calibrate, which it also disables
    // (DesignSystem.md, Screen readers).
    accessibility::describe_with(*calibrate, *hint);
  }
  void changed(const std::optional<std::string> &error) {
    app.refresh();
    // A draft toggle follows every click; a refusal repeats while its cause
    // stays.
    app.report(error ? "Not changed: " + *error : "Unsaved visual changes. Save applies them.",
               error ? StatusVoice::changed : StatusVoice::silent);
  }
  auto unchanged_guard() const {
    const auto &document = app.session.document();
    return [this, cooldown = selected_id, path = document.path(),
            generation = app.session.edit_generation(),
            visual_state = document.visual_state(),
            automation_state = document.automation_state()] {
      const auto &session = app.session;
      return session.has_document() && selected_id == cooldown &&
          session.edit_generation() == generation &&
          session.document().path() == path &&
          session.document().visual_state() == visual_state &&
          session.document().automation_state() == automation_state;
    };
  }
  void handle(Fl_Widget *widget) {
    auto &session = app.session;
    if (!session.has_document()) return;
    if (widget == target) {
      const auto index = target->value();
      const auto &cooldowns = session.document().profile().cooldowns;
      if (index >= 0 && static_cast<std::size_t>(index) < cooldowns.size())
        selected_id = cooldowns[static_cast<std::size_t>(index)].id;
      refresh(); return;
    }
    if (widget == isolated) {
      changed(session.set_visual_runtime(isolated->value() ? core::VisualRuntime::isolated : core::VisualRuntime::inline_process));
      return;
    }
    if (widget == enabled) {
      const bool value = enabled->value() != 0;
      changed(session.edit_visual([&](auto &visual) -> std::optional<std::string> {
        if (!visual) return "No visual assignment.";
        auto *trigger = find_visual_trigger(*visual, selected_id);
        if (!trigger) return "No visual assignment for this cooldown.";
        trigger->enabled = value; return std::nullopt;
      })); return;
    }
    if (widget == remove) {
      const auto cooldown = selected_id;
      const auto unchanged = unchanged_guard();
      fl_message_title("Remove visual assignment");
      if (fl_choice("Remove this cooldown's visual assignment? Shared observations stay in use.",
                    "Cancel", "Remove assignment", nullptr) != 1) return;
      if (!unchanged()) {
        app.report("Not removed: the active document or cooldown changed during confirmation."); return;
      }
      changed(session.edit_visual([&](auto &visual) -> std::optional<std::string> {
        if (!visual || !find_visual_trigger(*visual, cooldown)) return "No visual assignment for this cooldown.";
        erase_visual_assignment(*visual, cooldown);
        if (visual->triggers.empty() && visual->observations.empty()) visual.reset();
        return std::nullopt;
      })); return;
    }
    if (widget != calibrate || !app.apply_fields()) return;
    session.end_text_edit();
    const auto path = session.document().path();
    const auto cooldown = selected_id;
    const auto unchanged = unchanged_guard();
    auto seed = CalibrationModel::open(session.document().profile(), cooldown,
                                       session.document().visual());
    if (!seed) { app.report("Unavailable: the selected cooldown no longer exists."); return; }
    const auto chooser = [&] {
      fl_message_title("Shared observation");
      const auto answer = fl_choice("Other cooldowns share this observation. Update all or clone for this cooldown?",
                                    "Cancel", "Update shared", "Clone for cooldown");
      return answer == 1 ? SharedObservationChoice::update_shared :
             answer == 2 ? SharedObservationChoice::clone : SharedObservationChoice::cancel;
    };
    // Apply prepares the complete typed merge and asset preflight while the
    // dialog keeps its draft, so a refusal never discards the calibration.
    const CalibrationAccept accept = [&](const CalibrationDraft &draft) -> CalibrationAcceptance {
      if (!unchanged())
        return std::string("The active document changed during calibration.");
      auto accepted = session.prepare_calibration(cooldown, draft.visual, draft.images, chooser);
      if (!unchanged())
        return std::string("The active document or cooldown changed during calibration.");
      return accepted;
    };
    auto accepted = run_x11_calibration(path, std::move(*seed), accept,
                                        session.owned_template_assets());
    if (!accepted) { app.report("Calibration cancelled. No calibration data or assets were written."); return; }
    if (!unchanged()) { app.report("Not applied: the active document changed during calibration."); return; }
    // The session owns the prepared template bytes; Save publishes them with
    // the profile and sidecars in one durable transaction.
    if (const auto error = session.apply_calibration(std::move(*accepted))) {
      app.report("Not applied: " + *error); return;
    }
    // A refused durable Save keeps the complete calibration in the document
    // for retry; it never claims reload acknowledgement or publishes assets.
    app.refresh(); (void)app.save();
  }
};
LinuxVisualEditor::LinuxVisualEditor(EditorApp &app)
    : app_(app), page_(new Fl_Group(270, 45, 775, 405, "Visual")) {
  page_->end(); page_->hide();
}
LinuxVisualEditor::~LinuxVisualEditor() = default;
Fl_Group *LinuxVisualEditor::group() const { return page_; }
void LinuxVisualEditor::refresh() {
  if (!app_.session.has_document()) { page_->deactivate(); return; }
  page_->activate();
  if (!impl_) {
    if (!page_->visible()) return;
    auto *previous = Fl_Group::current();
    impl_ = std::make_unique<Impl>(app_, *page_); Fl_Group::current(previous);
  }
  impl_->refresh();
}
} // namespace layerc::editor
