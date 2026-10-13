#pragma once

#include <memory>
#include <string>

class Fl_Double_Window;
class Fl_Multiline_Output;
class Fl_Widget;

namespace layerc::editor {

// Modeless Runtime Diagnostics view of the daemon's status snapshot. It is
// refreshed only on open and on request (no timers or threads) and its text
// comes from the shared runtime_diagnostics_view formatter.
class DiagnosticsWindow final {
public:
  DiagnosticsWindow();
  ~DiagnosticsWindow();
  DiagnosticsWindow(const DiagnosticsWindow &) = delete;
  DiagnosticsWindow &operator=(const DiagnosticsWindow &) = delete;
  void show();
  void refresh();
  [[nodiscard]] bool shown() const;
  [[nodiscard]] const std::string &raw_json() const { return raw_json_; }

private:
  static void on_refresh(Fl_Widget *, void *);
  static void on_copy(Fl_Widget *, void *);
  static void on_close(Fl_Widget *, void *);
  std::unique_ptr<Fl_Double_Window> window_;
  Fl_Multiline_Output *text_{};
  std::string raw_json_;
};

// Modal read-only text with Close and, when copy_text is not empty, a
// "Copy details" button that places copy_text on the clipboard.
void show_text_dialog(const char *title, const std::string &text,
                      const std::string &copy_text = {});

// About LayerE, with a "Copy details" button for version and runtime state.
void show_about_dialog(const std::string &daemon_summary, const std::string &accessibility);

} // namespace layerc::editor
