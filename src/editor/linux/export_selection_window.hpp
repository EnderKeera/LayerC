#pragma once

#include "layerc/platform/profile_store.hpp"

#include <FL/Fl_Double_Window.H>
#include <optional>
#include <vector>

class Fl_Box;
class Fl_Check_Button;

namespace layerc::editor {
class ExportSelectionDialog final : public Fl_Double_Window {
public:
  ExportSelectionDialog(const std::vector<platform::StoredProfile> &profiles,
                        std::size_t selected);
  std::vector<Fl_Check_Button *> profiles;
  std::optional<std::vector<std::size_t>> selection;
private:
  Fl_Box *status_{};
  void check_all(bool checked);
  void apply();
};
} // namespace layerc::editor
