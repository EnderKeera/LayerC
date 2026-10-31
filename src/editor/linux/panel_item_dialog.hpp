#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"

#include <FL/Fl_Double_Window.H>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Input;

namespace layerc::editor {

struct PanelItemDraft {
  core::OverlayItem item;
  bool timer_preset{};
  core::Milliseconds interval{};
};

// A local field buffer. Apply invokes one atomic document edit; refusal keeps
// the dialog and every authored field intact. Cancel never edits the document.
class PanelItemDialog final : public Fl_Double_Window {
public:
  using Apply = std::function<std::optional<std::string>(const PanelItemDraft &)>;
  PanelItemDialog(const core::GameProfile &profile,
                  const std::optional<core::AutomationProfile> &automation,
                  std::optional<core::OverlayItem> current, Apply apply);
private:
  void fill_sources(const std::string &selected = {});
  void update_fields();
  void accept();
  void error(const std::string &message);
  core::GameProfile profile_;
  std::optional<core::AutomationProfile> automation_;
  std::optional<core::OverlayItem> current_;
  Apply apply_;
  Fl_Choice *kind_{}, *source_{}, *display_{};
  Fl_Input *label_{}, *maximum_{}, *flash_{}, *interval_{};
  Fl_Check_Button *custom_label_{};
  Fl_Box *message_{};
  std::vector<std::string> source_ids_;
};

} // namespace layerc::editor
