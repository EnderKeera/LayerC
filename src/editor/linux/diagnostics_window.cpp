#include "diagnostics_window.hpp"
#include "daemon_link.hpp"
#include "runtime_diagnostics_view.hpp"
#include "layerc/core/version.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/fl_draw.H>

#include <algorithm>

namespace layerc::editor {
namespace {
constexpr int width = 680, height = 480, margin = 10, button_height = 30;

std::string join(const RuntimeDiagnosticsView &view) {
  std::string text;
  for (const auto &line : view.lines) text += (text.empty() ? "" : "\n") + line;
  return text;
}
} // namespace

void fit_text_view(Fl_Window &window, Fl_Multiline_Output &text) {
  fl_font(text.textfont(), text.textsize());
  // Fl_Input_ draws inside its box frame with a small text margin.
  int width = text.w() - Fl::box_dw(text.box()) - 6, height = 0;
  if (width <= 0) return;
  fl_measure(text.value(), width, height, 0);
  const int needed = height + Fl::box_dh(text.box()) + fl_height();
  if (needed <= text.h()) return;
  int x = 0, y = 0, w = 0, h = 0;
  Fl::screen_work_area(x, y, w, h, window.x(), window.y());
  const int limit = std::max(window.h(), h - 40); // room for decorations
  window.size(window.w(), std::min(window.h() + needed - text.h(), limit));
}

DiagnosticsWindow::DiagnosticsWindow()
    : window_(std::make_unique<Fl_Double_Window>(width, height, "Runtime Diagnostics — LayerE")) {
  window_->begin();
  text_ = new Fl_Multiline_Output(margin, margin, width - 2 * margin,
                                  height - 3 * margin - button_height, "Runtime diagnostics");
  text_->wrap(1);
  text_->align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE);
  text_->labeltype(FL_NO_LABEL);
  text_->textfont(FL_COURIER);
  const int y = height - margin - button_height;
  auto *refresh_button = new Fl_Button(margin, y, 110, button_height, "&Refresh");
  refresh_button->callback(on_refresh, this);
  auto *copy = new Fl_Button(margin + 120, y, 150, button_height, "&Copy raw JSON");
  copy->callback(on_copy, this);
  auto *close = new Fl_Button(width - margin - 110, y, 110, button_height, "Close");
  close->callback(on_close, this);
  window_->resizable(text_);
  window_->size_range(480, 320);
  window_->end();
}
DiagnosticsWindow::~DiagnosticsWindow() = default;

void DiagnosticsWindow::show() {
  refresh();
  window_->show();
}
bool DiagnosticsWindow::shown() const { return window_->shown() != 0; }

void DiagnosticsWindow::refresh() {
  // The authenticated control socket proves the peer, so a reply is both
  // connected and authenticated.
  const auto status = query_daemon_status();
  raw_json_ = status.payload_json;
  auto view = runtime_diagnostics_view(raw_json_, status.running, status.running, true);
  if (!status.running)
    view.lines.push_back(status.summary());
  else if (!status.error.empty())
    view.lines.push_back("Status problem: " + status.error);
  text_->value(join(view).c_str());
  fit_text_view(*window_, *text_);
  text_->redraw();
}

void DiagnosticsWindow::on_refresh(Fl_Widget *, void *data) {
  static_cast<DiagnosticsWindow *>(data)->refresh();
}
void DiagnosticsWindow::on_copy(Fl_Widget *, void *data) {
  const auto &json = static_cast<DiagnosticsWindow *>(data)->raw_json_;
  if (!json.empty()) Fl::copy(json.c_str(), static_cast<int>(json.size()), 1);
}
void DiagnosticsWindow::on_close(Fl_Widget *, void *data) {
  static_cast<DiagnosticsWindow *>(data)->window_->hide();
}

void show_text_dialog(const char *title, const std::string &text, const std::string &copy_text) {
  Fl_Double_Window dialog(560, 400, title);
  auto *output = new Fl_Multiline_Output(margin, margin, 560 - 2 * margin,
                                         400 - 3 * margin - button_height, title);
  output->wrap(1);
  output->labeltype(FL_NO_LABEL);
  output->value(text.c_str());
  const int y = 400 - margin - button_height;
  if (!copy_text.empty()) {
    auto *copy = new Fl_Button(margin, y, 140, button_height, "&Copy details");
    copy->callback([](Fl_Widget *, void *data) {
      const auto &details = *static_cast<const std::string *>(data);
      Fl::copy(details.c_str(), static_cast<int>(details.size()), 1);
    }, const_cast<std::string *>(&copy_text));
  }
  auto *close = new Fl_Button(560 - margin - 110, y, 110, button_height, "Close");
  close->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  dialog.resizable(output);
  dialog.end();
  fit_text_view(dialog, *output);
  dialog.set_modal();
  dialog.show();
  close->take_focus();
  while (dialog.shown()) Fl::wait();
}

void show_about_dialog(const std::string &daemon_summary, const std::string &accessibility) {
#ifdef NDEBUG
  constexpr const char *build = "release";
#else
  constexpr const char *build = "debug";
#endif
  const std::string version(layerc::core::product_version);
  const std::string text =
      "LayerE " + version + " — Linux X11 editor\n\n"
      "Creates and edits cooldown overlay profiles. LayerC reads input passively: "
      "it never blocks, changes or sends input, and the editor never accesses the network.\n\n"
      "Source and releases: github.com/EnderKeera/LayerC";
  const std::string details = "LayerE " + version + " (" + build + " build), FLTK " +
      std::to_string(Fl::api_version()) + ", display scale " +
      std::to_string(static_cast<int>(Fl::screen_scale(0) * 100.0F + 0.5F)) + " %\n" +
      accessibility + "\n" + daemon_summary;
  show_text_dialog("About LayerE", text, details);
}

} // namespace layerc::editor
