#include "diagnostics_window.hpp"
#include "appearance_window.hpp"
#include "editor_style.hpp"
#include "daemon_link.hpp"
#include "platform/product_text.hpp"
#include "runtime_diagnostics_view.hpp"
#include "layerc/core/version.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/fl_draw.H>

#include <FL/Fl_Round_Button.H>
#include "layerc/core/json.hpp"

#include <algorithm>
#include <utility>

namespace layerc::editor {
namespace {
constexpr int diagnostics_width = 680, diagnostics_height = 480, margin = 10;
constexpr int button_height = editor_button_height;

class DiagnosticsOutput final : public EditorField<Fl_Multiline_Output> {
public:
  using EditorField<Fl_Multiline_Output>::EditorField;
};

// The window stretches its content; the action row stretches only its spacer.
// Buttons are measured once and stay fixed-width and right-aligned on resize.
class TextDialogShell final : public Fl_Double_Window {
public:
  TextDialogShell(int width, int height, const char *title)
      : Fl_Double_Window(width, height, title) {
    body_ = new Fl_Group(margin, margin, width - 2 * margin,
                          height - 3 * margin - button_height);
    body_->end();
    row_ = new Fl_Group(margin, height - margin - button_height,
                         width - 2 * margin, button_height);
    spacer_ = new Fl_Box(row_->x(), row_->y(), row_->w(), row_->h());
    row_->resizable(spacer_); row_->end();
    end(); resizable(body_);
  }
  Fl_Group &body() { return *body_; }
  Fl_Button &action(const char *label) {
    row_->begin();
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    const int width = std::max(110, static_cast<int>(fl_width(label)) + 2 * margin);
    auto *button = new Fl_Button(0, row_->y(), width, button_height, label);
    row_->end();
    int x = row_->x() + row_->w();
    for (int i = row_->children(); i-- > 1;) {
      auto *child = row_->child(i); x -= child->w();
      child->position(x, row_->y()); x -= margin;
    }
    spacer_->resize(row_->x(), row_->y(), std::max(0, x - row_->x()), row_->h());
    row_->init_sizes();
    return *button;
  }
  void minimum(int content_width, int content_height) {
    const int actions = row_->w() - spacer_->w();
    size_range(std::max(content_width, actions) + 2 * margin,
                content_height + 3 * margin + button_height);
  }
private:
  Fl_Group *body_{}, *row_{};
  Fl_Box *spacer_{};
};

std::string join(const RuntimeDiagnosticsView &view) {
  std::string text;
  for (const auto &line : view.lines) text += (text.empty() ? "" : "\n") + line;
  return text;
}
} // namespace

bool has_diagnostics_text_modes(const Fl_Widget &widget) noexcept {
  return dynamic_cast<const DiagnosticsOutput *>(&widget) != nullptr;
}

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

DiagnosticsWindow::DiagnosticsWindow(std::function<DaemonStatus()> status,
                                     std::function<DaemonCommandResult()> retry)
    : window_(std::make_unique<Fl_Double_Window>(diagnostics_width, diagnostics_height,
                                                 "Runtime diagnostics — LayerE")),
      status_(std::move(status)), retry_(std::move(retry)) {
  window_->begin();
  mode_ = new Fl_Group(margin, margin, diagnostics_width - 2 * margin, editor_input_height);
  summary_mode_ = new Fl_Round_Button(margin, margin, 110, editor_input_height, "&Summary");
  raw_ = new Fl_Round_Button(margin + 120, margin, 110, editor_input_height, "Raw &JSON");
  for (auto *button : {summary_mode_, raw_}) {
    button->type(FL_RADIO_BUTTON);
    button->callback(on_mode, this);
  }
  summary_mode_->setonly();
  mode_->deactivate();
  mode_->end();
  const int top = mode_->y() + mode_->h() + margin;
  text_ = new DiagnosticsOutput(margin, top, diagnostics_width - 2 * margin,
      diagnostics_height - top - 2 * margin - button_height, "Runtime diagnostics");
  text_->align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE);
  text_->labeltype(FL_NO_LABEL);
  const int y = diagnostics_height - margin - button_height;
  auto *refresh_button = new Fl_Button(margin, y, 110, button_height, "&Refresh");
  refresh_button->callback(on_refresh, this);
  retry_button_ = new Fl_Button(margin + 120, y, 110, button_height, "Retry &visual");
  retry_button_->callback(on_retry, this);
  retry_button_->hide();
  copy_ = new Fl_Button(retry_button_->x(), y, 150, button_height, "&Copy raw JSON");
  copy_->callback(on_copy, this);
  auto *close = new Fl_Button(diagnostics_width - margin - 110, y, 110, button_height, "Close");
  primary_button(*close);
  close->callback(on_close, this);
  window_->resizable(text_);
  window_->size_range(530, 320);
  window_->end();
}
DiagnosticsWindow::~DiagnosticsWindow() = default;

void DiagnosticsWindow::show() {
  refresh();
  window_->show();
}
void DiagnosticsWindow::hide() { window_->hide(); }

bool DiagnosticsWindow::shown() const { return window_->shown() != 0; }

void DiagnosticsWindow::refresh() {
  const auto status = status_();
  raw_json_ = status.payload_json;
  auto view = runtime_diagnostics_view(raw_json_, status.running, status.running, true);
  if (!status.running)
    view.lines.push_back(status.summary());
  else if (!status.error.empty())
    view.lines.push_back("Status problem: " + status.error);
  if (!retry_outcome_.empty()) view.lines.push_back(retry_outcome_);
  summary_ = join(view);
  if (raw_json_.empty()) {
    mode_->deactivate();
    copy_->deactivate();
  } else {
    mode_->activate();
    copy_->activate();
  }
  if (daemon_retry_supported() && status.running && view.retry_visual)
    retry_button_->show();
  else
    retry_button_->hide();
  copy_->position(retry_button_->x() + (retry_button_->visible() ? retry_button_->w() + margin : 0),
                  copy_->y());
  window_->init_sizes();
  refresh_details();
}

void DiagnosticsWindow::refresh_details() {
  const bool raw = raw_->value() && !raw_json_.empty();
  text_->textfont(raw ? FL_COURIER : FL_HELVETICA);
  text_->wrap(!raw);
  if (raw) {
    auto formatted = core::json::format(raw_json_);
    std::erase(formatted, '\r');
    text_->value(formatted.c_str());
  } else {
    text_->value(summary_.c_str());
    fit_text_view(*window_, *text_);
  }
  apply_editor_window_appearance(*window_);
}

void DiagnosticsWindow::on_refresh(Fl_Widget *, void *data) {
  auto &self = *static_cast<DiagnosticsWindow *>(data);
  self.retry_outcome_.clear();
  self.refresh();
}

void DiagnosticsWindow::on_retry(Fl_Widget *, void *data) {
  auto &self = *static_cast<DiagnosticsWindow *>(data);
  const auto result = self.retry_();
  self.retry_outcome_ = (result.state == DaemonCommandState::acknowledged
                             ? "Visual retry: " : "Visual retry failed: ") + result.text;
  self.summary_mode_->setonly();
  self.refresh();
}

void DiagnosticsWindow::on_mode(Fl_Widget *, void *data) {
  static_cast<DiagnosticsWindow *>(data)->refresh_details();
}
void DiagnosticsWindow::on_copy(Fl_Widget *, void *data) {
  const auto &json = static_cast<DiagnosticsWindow *>(data)->raw_json_;
  if (!json.empty()) Fl::copy(json.c_str(), static_cast<int>(json.size()), 1);
}
void DiagnosticsWindow::on_close(Fl_Widget *, void *data) {
  static_cast<DiagnosticsWindow *>(data)->window_->hide();
}

void show_text_dialog(const char *title, const std::string &text, const std::string &copy_text) {
  TextDialogShell dialog(560, 400, title);
  auto &body = dialog.body(); body.begin();
  auto *output = new EditorField<Fl_Multiline_Output>(body.x(), body.y(), body.w(), body.h(), title);
  output->wrap(1);
  output->labeltype(FL_NO_LABEL);
  output->value(text.c_str());
  body.resizable(output); body.end();
  if (!copy_text.empty()) {
    auto *copy = &dialog.action("&Copy details");
    copy->callback([](Fl_Widget *, void *data) {
      const auto &details = *static_cast<const std::string *>(data);
      Fl::copy(details.c_str(), static_cast<int>(details.size()), 1);
    }, const_cast<std::string *>(&copy_text));
  }
  auto *close = &dialog.action("Close");
  primary_button(*close);
  close->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  fl_font(output->textfont(), output->textsize());
  dialog.minimum(static_cast<int>(fl_width("LayerE — Runtime diagnostics")) + 2 * margin,
                   8 * fl_height());
  fit_text_view(dialog, *output);
  apply_editor_window_appearance(dialog);
  dialog.set_modal();
  dialog.show();
  close->take_focus();
  while (dialog.shown()) Fl::wait();
}

void show_shortcuts_dialog(const std::string_view rows) {
  // Measure in the body font the browser draws with, never the default.
  fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
  // Both columns come from the rendered text, so no row is padded with
  // spaces and no width is guessed (DesignSystem.md, Keyboard).
  int key_width = 0;
  int description_width = 0;
  std::vector<std::pair<std::string, std::string>> pairs;
  std::size_t start = 0;
  while (start <= rows.size()) {
    const auto end = rows.find('\n', start);
    const std::string_view line = rows.substr(start, end == std::string_view::npos
                                                          ? std::string_view::npos
                                                          : end - start);
    const auto tab = line.find('\t');
    std::string key(line.substr(0, tab));
    std::string description(tab == std::string_view::npos ? std::string_view{}
                                                          : line.substr(tab + 1));
    key_width = std::max(key_width, static_cast<int>(fl_width(key.c_str())));
    description_width =
        std::max(description_width, static_cast<int>(fl_width(description.c_str())));
    pairs.emplace_back(std::move(key), std::move(description));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  const int keys = key_width + 24;
  const int descriptions = std::max(320, description_width + 24);
  int screen_x{}, screen_y{}, screen_width{}, screen_height{};
  Fl::screen_xywh(screen_x, screen_y, screen_width, screen_height, 0);
  const int width = std::min(keys + descriptions + 2 * margin, screen_width);
  const int visible = std::min(static_cast<int>(pairs.size()), 22);
  const int height = std::min(2 * margin + visible * 20 + editor_button_height + margin,
                              screen_height);
  TextDialogShell dialog(width, height, "Keyboard shortcuts — LayerE");
  auto &body = dialog.body(); body.begin();
  auto *browser = new Fl_Browser(body.x(), body.y(), body.w(), body.h(), "Keyboard shortcuts");
  browser->labeltype(FL_NO_LABEL);
  browser->type(FL_HOLD_BROWSER);
  browser->textfont(FL_HELVETICA);
  browser->textsize(FL_NORMAL_SIZE);
  const int columns[]{keys, 0};
  browser->column_widths(columns);
  for (const auto &[key, description] : pairs)
    browser->add((key + "\t" + description).c_str());
  body.resizable(browser); body.end();
  auto *close = &dialog.action("Close");
  primary_button(*close);
  close->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  dialog.minimum(keys + descriptions, 6 * (fl_height() + 4));
  apply_editor_window_appearance(dialog);
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
      "LayerE " + version + " — " + std::string(editor_product_line()) + "\n\n"
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
