#pragma once

#include "../editor_appearance.hpp"
#include "metrics.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Flex.H>
#include <FL/Fl_Grid.H>

#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

class Fl_Button;
class Fl_Check_Button;
class Fl_Window;

namespace layerc::editor::kit {

// Layout and the FLTK measurements it uses belong to the UI thread.
[[nodiscard]] int text_width(const char *label);
[[nodiscard]] int wrapped_height(const char *text, int width);
[[nodiscard]] int button_width(const char *label);
[[nodiscard]] int check_label_offset() noexcept;
[[nodiscard]] int check_ring_margin() noexcept;

class RowLabel final : public Fl_Box {
public:
  RowLabel(const char *text, Fl_Widget &buddy);
  int handle(int event) override;

private:
  void draw() override;
  Fl_Widget_Tracker buddy_;
};

inline constexpr std::array form_hint_painted_text{
    PaintedText{FixedInk::subtext, PaintedGround::beneath, PaintedPurpose::options_hint, false}};

class Form final : public Fl_Grid {
public:
  Form();
  RowLabel &field_row(const char *label, Fl_Widget &control,
                      std::optional<int> logical_width = std::nullopt);
  void check_row(Fl_Check_Button &button);
  Fl_Box &hint(Fl_Widget &owner, const char *text,
               std::initializer_list<std::string> reserve = {});
  void gap(Metric metric);

  [[nodiscard]] int label_column() const;
  [[nodiscard]] int widest_check() const;
  [[nodiscard]] int minimum_width() const;
  [[nodiscard]] int height_for(int width) const;
  void finish();
  void resize(int x, int y, int w, int h) override;

private:
  struct Row {
    Fl_Widget *control{};
    RowLabel *label{};
    Fl_Box *hint{};
    std::optional<int> logical_width;
    std::optional<Metric> gap;
    std::vector<std::string> reserve;
  };

  [[nodiscard]] std::vector<int> row_heights(int width, int column) const;
  std::vector<Row> rows_;
  bool finished_{};
};

class ButtonRow final : public Fl_Flex {
public:
  ButtonRow();
  void leading(Fl_Button &button);
  void action(Fl_Button &button);
  [[nodiscard]] int width() const;
  void resize(int x, int y, int w, int h) override;

private:
  Fl_Box *spacer_{};
};

enum class DialogSizing { fixed, resizable };

class DialogFrame final {
public:
  DialogFrame(Fl_Window &window, Form &form, ButtonRow &buttons,
              int preferred_logical_width, int work_width, DialogSizing sizing);
  ~DialogFrame();
  DialogFrame(const DialogFrame &) = delete;
  DialogFrame &operator=(const DialogFrame &) = delete;

  void refit(int requested_client_width = 0);
  void place();
  [[nodiscard]] int minimum_width() const;

private:
  Fl_Window &window_;
  Form &form_;
  ButtonRow &buttons_;
  int preferred_logical_width_;
  int work_width_;
  DialogSizing sizing_;
  MetricsSubscription subscription_{};
  bool fitting_{};
};

} // namespace layerc::editor::kit
