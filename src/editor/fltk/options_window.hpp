#pragma once

#include "editor_appearance.hpp"
#include "kit/layout.hpp"
#include "platform/editor_options.hpp"

#include <FL/Fl_Double_Window.H>

#include <functional>

class Fl_Box;
class Fl_Button;
class Fl_Check_Button;

namespace layerc::editor {

class EditorApp;
class EditorChoice;
// FLTK-dimmed labels belong to the deferred Disabled / Fill class.
inline constexpr auto &options_painted_text = kit::form_hint_painted_text;

class OptionsWindow final : public Fl_Double_Window {
public:
  using Commit = std::function<std::optional<std::string_view>(const EditorOptions &)>;
  using Applied = std::function<void(const EditorOptions &)>;
  using Failed = std::function<void(std::string_view)>;

  OptionsWindow(const EditorOptions &initial, const EditorAppearance &appearance, int work_width,
                Commit commit, Applied applied, Failed failed);
  void show() override;
  void refresh_capture_warning();

private:
  void enable_apply();
  [[nodiscard]] bool apply();

  Fl_Check_Button *visual_{}, *overlays_{}, *editor_{}, *updates_{};
  EditorChoice *text_size_{};
  Fl_Button *apply_{};
  Fl_Box *capture_hint_{};
  std::string_view capture_warning_;
  Commit commit_;
  Applied applied_;
  Failed failed_;
  std::optional<kit::DialogFrame> frame_;
};

void show_options_dialog(EditorApp &app);

} // namespace layerc::editor
