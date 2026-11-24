#include "layout.hpp"

#include "../accessible_widgets.hpp"
#include "../editor_style.hpp"

#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <utility>

namespace layerc::editor::kit {
namespace {

int measure(const char *text, int &width, const char shortcut) {
  fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
  const auto saved = fl_draw_shortcut;
  fl_draw_shortcut = shortcut;
  int height = 0;
  fl_measure(text, width, height, 0);
  fl_draw_shortcut = saved;
  return height;
}

int check_width(const Fl_Widget &check) {
  return check_label_offset() + text_width(check.label()) + check_ring_margin();
}

int spacing_after(const bool hint) {
  return hint ? metric(Metric::gap_dialog)
              : metric(Metric::row_pitch) - metric(Metric::control_height);
}

class FormHint final : public Fl_Box, public PaintedTextSource {
public:
  using Fl_Box::Fl_Box;
  [[nodiscard]] std::span<const PaintedText> painted_text() const noexcept override {
    return form_hint_painted_text;
  }

private:
  void draw() override {
    const auto ink = fixed_ink(current_editor_tokens(), painted_text().front().ink);
    labelcolor(fl_rgb_color(static_cast<uchar>(ink >> 16U), static_cast<uchar>(ink >> 8U),
                            static_cast<uchar>(ink)));
    draw_box();
    draw_label(x(), y(), w(), h());
  }
};

} // namespace

int text_width(const char *label) {
  int width = 0;
  measure(label, width, 2);
  return width;
}

int wrapped_height(const char *text, int width) {
  width = std::max(1, width);
  return measure(text, width, 0);
}

int button_width(const char *label) {
  return std::max(metric(Metric::button_min_width), text_width(label) + metric(Metric::button_text_pad));
}

int check_label_offset() noexcept { return metric(Metric::checkbox) + metric(Metric::gap_dialog); }
int check_ring_margin() noexcept { return 2 * metric(Metric::focus_width); }

RowLabel::RowLabel(const char *text, Fl_Widget &buddy) : Fl_Box(0, 0, 0, 0), buddy_(&buddy) {
  copy_label(text);
  align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  shortcut_label(1);
  const auto ink = current_editor_tokens().secondary;
  labelcolor(fl_rgb_color(static_cast<uchar>(ink >> 16), static_cast<uchar>(ink >> 8),
                          static_cast<uchar>(ink)));
  accessibility::label_with(buddy, *this);
}

void RowLabel::draw() {
  draw_box();
  draw_label(x(), y(), w(), h());
}

int RowLabel::handle(const int event) {
  if (event == FL_SHORTCUT && test_shortcut())
    if (auto *buddy = buddy_.widget(); buddy && buddy->visible_r() && buddy->active_r())
      return buddy->take_focus();
  return Fl_Box::handle(event);
}

Form::Form() : Fl_Grid(0, 0, 0, 0) {}

RowLabel &Form::field_row(const char *label, Fl_Widget &control,
                          const std::optional<int> logical_width) {
  auto &side = *new RowLabel(label, control);
  add(control);
  insert(side, &control);
  Row row{};
  row.control = &control;
  row.label = &side;
  row.logical_width = logical_width;
  rows_.push_back(std::move(row));
  return side;
}

void Form::check_row(Fl_Check_Button &button) {
  add(button);
  button.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  Row row{};
  row.control = &button;
  rows_.push_back(std::move(row));
}

Fl_Box &Form::hint(Fl_Widget &owner, const char *text,
                  std::initializer_list<std::string> reserve) {
  auto &line = *new FormHint(0, 0, 0, 0);
  line.copy_label(text);
  add(line);
  line.align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  accessibility::describe_with(owner, line);
  Row row{};
  row.control = &owner;
  row.hint = &line;
  row.reserve = reserve;
  for (const auto &candidate : rows_)
    if (candidate.control == &owner && candidate.label) row.label = candidate.label;
  rows_.push_back(std::move(row));
  return line;
}

void Form::gap(const Metric value) {
  Row row{};
  row.gap = value;
  rows_.push_back(std::move(row));
}

int Form::label_column() const {
  int width = 0;
  for (const auto &row : rows_)
    if (row.label && row.control->visible())
      width = std::max(width, text_width(row.label->label()) + metric(Metric::gap_dialog));
  return width;
}

int Form::widest_check() const {
  int width = 0;
  for (const auto &row : rows_)
    if (!row.gap && !row.label && !row.hint && row.control->visible())
      width = std::max(width, check_width(*row.control));
  return width;
}

int Form::minimum_width() const {
  int field = 0;
  for (const auto &row : rows_)
    if (row.label && !row.hint && row.control->visible())
      field = std::max(field, row.logical_width ? scaled(*row.logical_width) : 1);
  return std::max({1, label_column() + field, widest_check() ? check_label_offset() + check_ring_margin() + 1 : 0});
}

std::vector<int> Form::row_heights(const int width, const int column) const {
  const int height = metric(Metric::control_height);
  std::vector<int> heights;
  heights.reserve(rows_.size());
  for (const auto &row : rows_) {
    if (row.gap) {
      heights.push_back(metric(*row.gap));
    } else if (!row.control->visible()) {
      heights.push_back(0);
    } else if (row.hint) {
      const int available = width - (row.label ? column : check_label_offset());
      int result = wrapped_height(row.hint->label(), available);
      for (const auto &text : row.reserve)
        result = std::max(result, wrapped_height(text.c_str(), available));
      heights.push_back(result);
    } else if (row.label || check_width(*row.control) <= width) {
      heights.push_back(height);
    } else {
      int available = std::max(1, width - check_label_offset() - check_ring_margin());
      const int lines = measure(row.control->label(), available, 2);
      heights.push_back(std::max(height, lines + std::max(0, height - fl_height())));
    }
  }
  return heights;
}

int Form::height_for(const int width) const {
  const auto heights = row_heights(width, label_column());
  int height = 0;
  const Row *previous = nullptr;
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const auto &row = rows_[i];
    if (!heights[i]) continue;
    if (!row.gap) {
      if (previous && !row.hint) height += spacing_after(previous->hint != nullptr);
      previous = &row;
    }
    height += heights[i];
  }
  return height;
}

void Form::finish() {
  if (Fl_Group::current() == this) end();
  finished_ = true;
  resize(x(), y(), w(), h());
}

void Form::resize(const int x, const int y, const int w, const int h) {
  Fl_Widget::resize(x, y, w, h);
  if (!finished_ || rows_.empty()) return;
  const int width = std::max(w, minimum_width());
  const int column = label_column();
  const auto heights = row_heights(width, column);
  layout(static_cast<int>(rows_.size()), 2, 0, 0);
  col_width(0, column);
  col_width(1, width - column);
  int previous = -1;
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const auto &row = rows_[i];
    const int index = static_cast<int>(i);
    const int height = heights[i];
    row_height(index, height);
    row_gap(index, 0);
    if (row.gap) continue;

    row.control->labelsize(FL_NORMAL_SIZE);
    if (row.hint) {
      if (height) row.hint->show(); else row.hint->hide();
      row.hint->labelsize(FL_NORMAL_SIZE);
      const int indent = row.label ? column : check_label_offset();
      widget(row.hint, index, 0, 1, 2, FL_GRID_RIGHT)->minimum_size(std::max(0, width - indent), height);
    } else if (row.label) {
      if (height) row.label->show(); else row.label->hide();
      row.label->labelsize(FL_NORMAL_SIZE);
      widget(row.label, index, 0)->minimum_size(std::max(0, column - metric(Metric::gap_dialog)), height);
      const int preferred = row.logical_width ? scaled(*row.logical_width) : metric(Metric::field_cap);
      const auto *choice = dynamic_cast<const Fl_Choice *>(row.control);
      const int control_height = choice ? metric(Metric::combo_height) : height;
      if (auto *menu = dynamic_cast<Fl_Choice *>(row.control)) menu->textsize(FL_NORMAL_SIZE);
      if (auto *input = dynamic_cast<Fl_Input_ *>(row.control)) input->textsize(FL_NORMAL_SIZE);
      widget(row.control, index, 1, FL_GRID_LEFT)->minimum_size(std::min(width - column, preferred), control_height);
    } else {
      widget(row.control, index, 0, 1, 2)->minimum_size(width, height);
    }

    if (height) {
      if (previous >= 0 && !row.hint)
        row_gap(previous, spacing_after(rows_[static_cast<std::size_t>(previous)].hint != nullptr));
      previous = index;
    }
  }
  Fl_Grid::layout();
}

ButtonRow::ButtonRow() : Fl_Flex(0, 0, 0, 0, Fl_Flex::HORIZONTAL) {
  spacer_ = new Fl_Box(0, 0, 0, 0);
}

void ButtonRow::leading(Fl_Button &button) { insert(button, spacer_); }
void ButtonRow::action(Fl_Button &button) { add(button); }

int ButtonRow::width() const {
  int total = 0, count = 0;
  for (int i = 0; i < children(); ++i)
    if (auto *item = child(i); item != spacer_ && item->visible()) {
      total += button_width(item->label());
      ++count;
    }
  return total + std::max(0, count - 1) * metric(Metric::gap_dialog);
}

void ButtonRow::resize(const int x, const int y, const int w, const int h) {
  const int space = metric(Metric::gap_dialog);
  if (w - width() >= space) spacer_->show(); else spacer_->hide();
  gap(space);
  for (int i = 0; i < children(); ++i)
    if (auto *item = child(i); item != spacer_) {
      item->labelsize(FL_NORMAL_SIZE);
      fixed(item, button_width(item->label()));
    }
  Fl_Flex::resize(x, y, w, h);
}

DialogFrame::DialogFrame(Fl_Window &window, Form &form, ButtonRow &buttons,
                         const int preferred_logical_width, const int work_width,
                         const DialogSizing sizing)
    : window_(window), form_(form), buttons_(buttons),
      preferred_logical_width_(preferred_logical_width), work_width_(work_width), sizing_(sizing) {
  refit();
  form_.finish();
  subscription_ = subscribe_metrics_changed([this] { refit(); });
}

DialogFrame::~DialogFrame() { unsubscribe_metrics_changed(subscription_); }

int DialogFrame::minimum_width() const {
  return std::max(form_.minimum_width(), buttons_.width()) + 2 * metric(Metric::margin);
}

void DialogFrame::refit(const int requested_client_width) {
  if (fitting_) return;
  fitting_ = true;
  const int margin = metric(Metric::margin);
  const int minimum = minimum_width();
  int x = 0, y = 0, work_w = 0, work_h = 0;
  Fl::screen_work_area(x, y, work_w, work_h, window_.screen_num());
  const int client_cap = work_w - (window_.decorated_w() - window_.w());
  const int maximum = std::max(minimum, work_width_ > 0 ? std::min(client_cap, work_width_) : client_cap);
  const int natural = std::max(scaled(preferred_logical_width_), form_.widest_check() + 2 * margin);
  const int width = std::clamp(requested_client_width > 0 ? requested_client_width : natural, minimum, maximum);
  const int content = width - 2 * margin;
  const int form_height = form_.height_for(content);
  const int buttons_top = margin + form_height + metric(Metric::gap_main);
  const int button_height = metric(Metric::control_height);

  const int height = buttons_top + button_height + margin;
  if (sizing_ == DialogSizing::fixed)
    window_.size_range(width, height, width, height);
  else
    window_.size_range(minimum, 2 * margin + form_.height_for(maximum - 2 * margin) +
                       metric(Metric::gap_main) + button_height);
  window_.size(width, height);
  form_.resize(margin, margin, content, form_height);
  buttons_.resize(margin, buttons_top, content, button_height);
  place();
  fitting_ = false;
}

void DialogFrame::place() {
  int x = 0, y = 0, w = 0, h = 0;
  Fl::screen_work_area(x, y, w, h, window_.screen_num());
  const int edge = (window_.decorated_w() - window_.w()) / 2;
  const int top = window_.decorated_h() - window_.h() - edge;
  const int left = x + edge;
  const int upper = y + top;
  const int fitted_x = std::clamp(window_.x(), left, std::max(left, x + w - window_.w() - edge));
  const int fitted_y = std::clamp(window_.y(), upper, std::max(upper, y + h - window_.h() - edge));
  if (window_.x() != fitted_x || window_.y() != fitted_y) window_.position(fitted_x, fitted_y);
}

} // namespace layerc::editor::kit
