#pragma once

#include "linux_atspi_test_fixture.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Radio_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Value_Slider.H>

#include <utility>

namespace layerc::test::atspi_fixture {

// A window whose widgets cover every interface the bridge publishes: a button
// with an action, editable, read-only, masked and multi-line text, a valuator,
// a list and a radio group. Cases address nodes by their labels.
struct RichFixture {
  Fl_Window *window{};
  Fl_Button *button{};           // "Apply"
  Fl_Input *name{};              // "Name"
  Fl_Secret_Input *passcode{};   // "Passcode"
  Fl_Input *reference{};         // "Reference", read-only
  Fl_Multiline_Input *notes{};   // "Notes"
  Fl_Value_Slider *level{};      // "Level"
  Fl_Group *mode{};              // "Mode", radio group
  Fl_Radio_Button *alpha{};
  Fl_Radio_Button *beta{};
  Fl_Hold_Browser *items{};      // "Items"
  Fl_Box *label{};               // "Static"
  FltkTree tree;

  RichFixture() {
    window = new Fl_Window(40, 40, 520, 420, "LayerC Interfaces");
    button = new Fl_Button(20, 20, 120, 28, "Apply");
    name = new Fl_Input(20, 60, 220, 28, "Name");
    name->value("a\xc3\xa9\xf0\x9f\x98\x80 b\nline2");
    passcode = new Fl_Secret_Input(260, 60, 200, 28, "Passcode");
    passcode->value("hunter2");
    reference = new Fl_Input(20, 100, 220, 28, "Reference");
    reference->value("fixed");
    reference->readonly(1);
    notes = new Fl_Multiline_Input(20, 140, 220, 60, "Notes");
    notes->value("first\nsecond");
    level = new Fl_Value_Slider(20, 220, 220, 26, "Level");
    level->bounds(0.0, 10.0);
    level->step(0.5);
    level->value(4.0);
    mode = new Fl_Group(20, 260, 220, 60, "Mode");
    alpha = new Fl_Radio_Button(30, 265, 90, 24, "Alpha");
    beta = new Fl_Radio_Button(30, 290, 90, 24, "Beta");
    alpha->setonly();
    mode->end();
    mode->box(FL_UP_BOX);
    items = new Fl_Hold_Browser(260, 220, 220, 120, "Items");
    items->add("one");
    items->add("two");
    label = new Fl_Box(20, 340, 220, 24, "Static");
    window->end();
    window->show();
    Fl::wait(0.01);
  }

  ~RichFixture() {
    Fl::delete_widget(window);
    Fl::wait(0.01);
  }

  RichFixture(const RichFixture &) = delete;
  RichFixture &operator=(const RichFixture &) = delete;
};

// --- reply parsers the shared harness does not need ----------------------

inline std::optional<bool> parse_bool(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  int value = 0;
  if (sd_bus_message_read(message, "b", &value) < 0) return std::nullopt;
  return value != 0;
}

inline std::optional<double> parse_double(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  double value = 0.0;
  if (sd_bus_message_read(message, "d", &value) < 0) return std::nullopt;
  return value;
}

inline std::optional<std::pair<std::int32_t, std::int32_t>>
parse_pair(sd_bus_message *message) {
  std::int32_t first = 0;
  std::int32_t second = 0;
  if (message == nullptr || sd_bus_message_read(message, "ii", &first, &second) < 0)
    return std::nullopt;
  return std::pair{first, second};
}

// A rect reply arrives as one (iiii) struct.
inline std::optional<layerc::editor::a11y::Bounds>
parse_rect(sd_bus_message *message) {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  if (message == nullptr ||
      sd_bus_message_read(message, "(iiii)", &x, &y, &width, &height) < 0)
    return std::nullopt;
  return layerc::editor::a11y::Bounds{static_cast<double>(x),
                                      static_cast<double>(y),
                                      static_cast<double>(width),
                                      static_cast<double>(height)};
}

// A (text, start, end) reply, the deprecated text methods' shape.
struct TextSpan {
  std::string text;
  std::int32_t start{};
  std::int32_t end{};
};

inline std::optional<TextSpan> parse_span(sd_bus_message *message) {
  if (message == nullptr) return std::nullopt;
  const char *text = nullptr;
  std::int32_t start = 0;
  std::int32_t end = 0;
  if (sd_bus_message_read(message, "sii", &text, &start, &end) < 0)
    return std::nullopt;
  return TextSpan{text != nullptr ? text : "", start, end};
}

// A (ii) struct reply: a selection range arrives as one struct.
inline std::optional<std::pair<std::int32_t, std::int32_t>>
parse_struct_pair(sd_bus_message *message) {
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "ii") < 0)
    return std::nullopt;
  const auto pair = parse_pair(message);
  sd_bus_message_exit_container(message);
  return pair;
}

// A double held in a property variant.
inline std::optional<double> variant_double(sd_bus_message *message) {
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "d") < 0)
    return std::nullopt;
  double value = 0.0;
  if (sd_bus_message_read(message, "d", &value) < 0) return std::nullopt;
  sd_bus_message_exit_container(message);
  return value;
}

// An int held in a property variant.
inline std::optional<std::int32_t> variant_int(sd_bus_message *message) {
  if (message == nullptr ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "i") < 0)
    return std::nullopt;
  std::int32_t value = 0;
  if (sd_bus_message_read(message, "i", &value) < 0) return std::nullopt;
  sd_bus_message_exit_container(message);
  return value;
}

// --- tree lookups ---------------------------------------------------------

// The object path of the first node whose accessible name matches, walking the
// published tree the way a client does.
[[nodiscard]] std::optional<std::string>
node_path_named(Harness &harness, std::string_view name);

// The published order and paths of one container's children.
[[nodiscard]] std::vector<Reference> children_of(Harness &harness,
                                               const std::string &path);

// Spins until the application has embedded; every case needs it first.
[[nodiscard]] bool embed(Harness &harness, const atspi::Adaptor &adaptor);

} // namespace layerc::test::atspi_fixture
