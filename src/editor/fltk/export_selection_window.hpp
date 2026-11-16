#pragma once

#include "layerc/platform/profile_store.hpp"

#include <FL/Fl_Double_Window.H>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Check_Button;

namespace layerc::editor {
class ExportSelectionDialog final : public Fl_Double_Window {
public:
  // Complete and summary scan rows both name the profile they refer to.
  template <typename Rows>
  ExportSelectionDialog(const Rows &entries, const std::size_t selected)
      : Fl_Double_Window(620, 460, "Select profiles to export — LayerE") {
    build(profile_labels(entries), selected);
  }
  std::vector<Fl_Check_Button *> profiles;
  std::optional<std::vector<std::size_t>> selection;
private:
  template <typename Rows>
  static std::vector<std::string> profile_labels(const Rows &entries) {
    std::vector<std::string> labels;
    labels.reserve(entries.size());
    for (const auto &entry : entries)
      labels.push_back(entry.profile.name + " (" + entry.profile.id + ")");
    return labels;
  }
  void build(const std::vector<std::string> &entries, std::size_t selected);
  Fl_Box *status_{};
  void check_all(bool checked);
  void apply();
};
} // namespace layerc::editor
