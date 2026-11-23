#pragma once

#include "daemon_link.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Fl_Button;
class Fl_Double_Window;
class Fl_Group;
class Fl_Multiline_Output;
class Fl_Round_Button;
class Fl_Widget;
class Fl_Window;

namespace layerc::editor {

// Modeless Runtime Diagnostics view of the daemon's status snapshot. It is
// refreshed only on open and on request (no timers or threads) and its text
// comes from the shared runtime_diagnostics_view formatter.
class DiagnosticsWindow final {
public:
  explicit DiagnosticsWindow(std::function<DaemonStatus()> status = query_daemon_status,
                             std::function<DaemonCommandResult()> retry = request_visual_retry);
  ~DiagnosticsWindow();
  DiagnosticsWindow(const DiagnosticsWindow &) = delete;
  DiagnosticsWindow &operator=(const DiagnosticsWindow &) = delete;
  void show();
  void hide();
  void refresh();
  [[nodiscard]] bool shown() const;
  [[nodiscard]] const std::string &raw_json() const { return raw_json_; }

private:
  static void on_refresh(Fl_Widget *, void *);
  static void on_retry(Fl_Widget *, void *);
  static void on_mode(Fl_Widget *, void *);
  static void on_copy(Fl_Widget *, void *);
  static void on_close(Fl_Widget *, void *);
  void refresh_details();

  std::unique_ptr<Fl_Double_Window> window_;
  Fl_Group *mode_{};
  Fl_Round_Button *summary_mode_{}, *raw_{};
  Fl_Multiline_Output *text_{};
  Fl_Button *retry_button_{}, *copy_{};
  std::function<DaemonStatus()> status_;
  std::function<DaemonCommandResult()> retry_;
  std::string raw_json_, summary_, retry_outcome_;
};

// The mode-switching diagnostics output can paint both status and verbose ink.
[[nodiscard]] bool has_diagnostics_text_modes(const Fl_Widget &widget) noexcept;

// Modal read-only text with Close and, when copy_text is not empty, a
// "Copy details" button that places copy_text on the clipboard.
// Grows `window` (whose resizable() is `text`) until `text` shows its whole
// wrapped value, never beyond the work area of the window's screen.
void fit_text_view(Fl_Window &window, Fl_Multiline_Output &text);
void show_text_dialog(const char *title, const std::string &text,
                      const std::string &copy_text = {});

// Keyboard shortcuts in two measured columns (DesignSystem.md, Keyboard):
// rows are "key<TAB>description", the key column is the widest key plus its
// gutter, and the description column the widest description. Modal, read-only.
void show_shortcuts_dialog(std::string_view rows);

// About LayerE, with a "Copy details" button for version and runtime state.
void show_about_dialog(const std::string &daemon_summary, const std::string &accessibility);

} // namespace layerc::editor
