#include "diagnostic_text.hpp"
#include "cooldown_groups_window.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"

#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>

#include <algorithm>
#include <array>
#include <string>

namespace layerc::editor {
CooldownGroupsDialog::CooldownGroupsDialog(EditorSession &session)
    : Fl_Double_Window(760, 550, "Cooldown groups — LayerE"),
      session_(session), source_(session.document().profile()),
      staged_(source_.cooldown_groups) {
  begin();
  groups = new Fl_Hold_Browser(15, 30, 220, 350, "Groups");
  groups->format_char(0);
  groups->callback([](Fl_Widget *, void *self) {
    static_cast<CooldownGroupsDialog *>(self)->select();
  }, this);
  const auto input = [](int y, const char *label) {
    return new EditorField<Fl_Input>(390, y, 350, editor_input_height, label);
  };
  id = input(20, "Group ID");
  name = input(58, "Group name");
  mode = new EditorChoice(390, 96, 350, editor_input_height, "Group mode");
  mode->add("Trigger propagation|Shared pool");
  duration = input(134, "Group seconds");
  charges = input(172, "Group charges");
  recovery = new EditorChoice(390, 210, 350, editor_input_height, "Group recovery");
  recovery->add("Single|Sequential|Parallel");
  retrigger = new EditorChoice(390, 248, 350, editor_input_height, "Group retrigger");
  retrigger->add("Ignore|Restart");
  auto *scroll = new Fl_Scroll(270, 305, 470, 110, "Group members");
  scroll->begin();
  for (std::size_t i{}; i < source_.cooldowns.size(); ++i) {
    const auto &cooldown = source_.cooldowns[i];
    auto *check = new Fl_Check_Button(280, 310 + static_cast<int>(i) * 28, 435, 26);
    check->copy_label((cooldown.name + " (" + cooldown.id + ")").c_str());
    members.push_back(check);
  }
  scroll->end();
  auto *add_button = new Fl_Button(15, 395, 105, editor_button_height, "Add group");
  add_button->callback([](Fl_Widget *, void *self) {
    static_cast<CooldownGroupsDialog *>(self)->add();
  }, this);
  auto *remove_button = new Fl_Button(130, 395, 105, editor_button_height, "Remove group");
  remove_button->callback([](Fl_Widget *, void *self) {
    static_cast<CooldownGroupsDialog *>(self)->remove();
  }, this);
  status_ = new Fl_Box(15, 440, 725, 55,
      "Shared pools own timing and charges. Propagation groups use each member's settings.");
  status_->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  auto *cancel = new Fl_Button(510, 505, 110, editor_button_height, "Cancel");
  cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  auto *apply_button = new Fl_Button(630, 505, 110, editor_button_height, "Apply");
  apply_button->callback([](Fl_Widget *, void *self) {
    static_cast<CooldownGroupsDialog *>(self)->apply();
  }, this);
  mode->callback([](Fl_Widget *, void *self) {
    auto &dialog = *static_cast<CooldownGroupsDialog *>(self);
    const bool shared = dialog.mode->value() == 1;
    for (Fl_Widget *field : std::array<Fl_Widget *, 4>{dialog.duration,
         dialog.charges, dialog.recovery, dialog.retrigger}) {
      if (shared) field->activate(); else field->deactivate();
    }
  }, this);
  end();
  set_modal();
  if (!staged_.empty()) selected_ = 0;
  fill_list();
  load();
}

void CooldownGroupsDialog::show_error(const std::string &text) {
  status_->copy_label(text.c_str());
  status_->redraw();
}
void CooldownGroupsDialog::fill_list() {
  groups->clear();
  for (const auto &group : staged_)
    groups->add((group.name + " (" + group.id + ")").c_str());
  groups->value(selected_ + 1);
}
void CooldownGroupsDialog::load() {
  const bool selected = selected_ >= 0 &&
      static_cast<std::size_t>(selected_) < staged_.size();
  for (Fl_Widget *field : std::array<Fl_Widget *, 7>{id, name, mode, duration,
       charges, recovery, retrigger}) {
    if (selected) field->activate(); else field->deactivate();
  }
  for (auto *member : members) {
    member->value(0);
    if (selected) member->activate(); else member->deactivate();
  }
  if (!selected) {
    id->value(""); name->value(""); duration->value(""); charges->value("");
    return;
  }
  const auto &group = staged_[static_cast<std::size_t>(selected_)];
  id->value(group.id.c_str()); name->value(group.name.c_str());
  mode->value(group.mode == core::CooldownGroupMode::shared_pool ? 1 : 0);
  duration->value(format_seconds(group.duration.count() > 0
      ? group.duration : core::Milliseconds(5000)).c_str());
  charges->value(std::to_string(group.max_charges).c_str());
  recovery->value(static_cast<int>(group.recovery));
  retrigger->value(static_cast<int>(group.retrigger));
  for (std::size_t i{}; i < members.size(); ++i)
    members[i]->value(std::ranges::find(group.cooldown_ids, source_.cooldowns[i].id)
                     != group.cooldown_ids.end());
  mode->do_callback();
}
bool CooldownGroupsDialog::read_fields() {
  if (selected_ < 0) return true;
  auto candidate = staged_[static_cast<std::size_t>(selected_)];
  candidate.id = id->value(); candidate.name = name->value();
  candidate.mode = mode->value() == 1 ? core::CooldownGroupMode::shared_pool
                                     : core::CooldownGroupMode::trigger_propagation;
  candidate.cooldown_ids.clear();
  for (std::size_t i{}; i < members.size(); ++i)
    if (members[i]->value()) candidate.cooldown_ids.push_back(source_.cooldowns[i].id);
  if (candidate.mode == core::CooldownGroupMode::shared_pool) {
    const auto seconds = parse_duration_seconds(duration->value());
    const auto count = parse_count(charges->value(), core::min_cooldown_charges,
                                   core::max_cooldown_charges);
    if (!seconds || !count) {
      show_error("Group seconds and charges must be complete values within the profile limits.");
      return false;
    }
    candidate.duration = *seconds; candidate.max_charges = *count;
    candidate.recovery = static_cast<core::RecoveryMode>(recovery->value());
    candidate.retrigger = static_cast<core::RetriggerPolicy>(retrigger->value());
  }
  auto profile = source_;
  profile.cooldown_groups = staged_;
  profile.cooldown_groups[static_cast<std::size_t>(selected_)] = candidate;
  if (const auto error = core::validate_profile(profile)) {
    show_error(cooldown_group_error_text(*error, profile.cooldown_groups));
    return false;
  }
  staged_[static_cast<std::size_t>(selected_)] = std::move(candidate);
  return true;
}
void CooldownGroupsDialog::select() {
  const int next = groups->value() - 1;
  if (!read_fields()) { groups->value(selected_ + 1); return; }
  selected_ = next; fill_list(); load();
}
void CooldownGroupsDialog::add() {
  if (!read_fields()) return;
  if (source_.cooldowns.empty() || staged_.size() >= core::max_cooldown_groups) {
    show_error("A group needs a cooldown; the profile permits at most 64 groups."); return;
  }
  std::size_t suffix{1};
  std::string fresh;
  do { fresh = "group-" + std::to_string(suffix++); }
  while (std::ranges::find(staged_, fresh, &core::CooldownGroup::id) != staged_.end());
  staged_.push_back({.id = fresh, .name = "New group",
      .cooldown_ids = {source_.cooldowns.front().id}, .duration = core::Milliseconds(5000)});
  selected_ = static_cast<int>(staged_.size()) - 1;
  fill_list(); load();
}
void CooldownGroupsDialog::remove() {
  if (selected_ < 0) return;
  staged_.erase(staged_.begin() + selected_);
  selected_ = staged_.empty() ? -1 : std::min(selected_, static_cast<int>(staged_.size()) - 1);
  fill_list(); load();
}
void CooldownGroupsDialog::apply() {
  if (!read_fields()) return;
  if (!session_.has_document() || session_.document().profile().id != source_.id ||
      session_.document().profile().cooldowns != source_.cooldowns ||
      session_.document().profile().cooldown_groups != source_.cooldown_groups) {
    show_error("The profile changed while this dialog was open. Cancel and reopen it."); return;
  }
  session_.end_text_edit();
  auto candidate = session_.document().profile();
  candidate.cooldown_groups = staged_;
  if (const auto error = core::validate_profile(candidate)) {
    show_error(cooldown_group_error_text(*error, candidate.cooldown_groups));
    return;
  }
  if (const auto error = session_.edit([&](auto &profile) {
        profile.cooldown_groups = staged_;
      })) {
    show_error(*error); return;
  }
  applied = true;
  hide();
}
} // namespace layerc::editor
