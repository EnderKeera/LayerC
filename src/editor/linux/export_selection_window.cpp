#include "export_selection_window.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Scroll.H>

namespace layerc::editor {
void ExportSelectionDialog::build(const std::vector<std::string> &entries,
                                  const std::size_t selected) {
  begin();
  auto *note = new Fl_Box(15, 15, 590, 40,
      "Choose the saved profiles to include. Modules and assets travel with each profile.");
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  auto *scroll = new Fl_Scroll(15, 65, 590, 270, "Saved profiles");
  scroll->begin();
  for (std::size_t i{}; i < entries.size(); ++i) {
    auto *check = new Fl_Check_Button(25, 70 + static_cast<int>(i) * 30, 545, 28);
    check->copy_label(entries[i].c_str());
    check->value(i == selected);
    profiles.push_back(check);
  }
  scroll->end();
  auto *all = new Fl_Button(15, 345, 100, 28, "Select all");
  all->callback([](Fl_Widget *, void *self) {
    static_cast<ExportSelectionDialog *>(self)->check_all(true);
  }, this);
  auto *none = new Fl_Button(125, 345, 100, 28, "Select none");
  none->callback([](Fl_Widget *, void *self) {
    static_cast<ExportSelectionDialog *>(self)->check_all(false);
  }, this);
  status_ = new Fl_Box(15, 380, 590, 28);
  status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  auto *cancel = new Fl_Button(375, 415, 110, 30, "Cancel");
  cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  auto *export_button = new Fl_Button(495, 415, 110, 30, "Export");
  export_button->callback([](Fl_Widget *, void *self) {
    static_cast<ExportSelectionDialog *>(self)->apply();
  }, this);
  end();
  set_modal();
}
void ExportSelectionDialog::check_all(const bool checked) {
  for (auto *profile : profiles) profile->value(checked);
  redraw();
}
void ExportSelectionDialog::apply() {
  std::vector<std::size_t> rows;
  for (std::size_t i{}; i < profiles.size(); ++i)
    if (profiles[i]->value()) rows.push_back(i);
  if (rows.empty()) {
    status_->copy_label("Select at least one saved profile.");
    status_->redraw(); return;
  }
  selection = std::move(rows);
  hide();
}
} // namespace layerc::editor
