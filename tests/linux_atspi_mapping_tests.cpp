#include "a11y/atspi/mapping.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace layerc::editor::a11y;
using namespace layerc::editor::a11y::atspi;
using layerc::test::expect;

bool has(const StateSet &bits, const State state) {
  const auto number = static_cast<std::size_t>(state);
  return (bits[number / 32] & (1U << (number % 32))) != 0U;
}

bool exposes(const std::vector<std::string_view> &names,
             const std::string_view name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

void every_role_has_a_code_and_name() {
  struct Expected {
    Role role;
    std::uint32_t code;
    std::string_view name;
  };
  const Expected expected[] = {
      {Role::application, 75, "application"},
      {Role::window, 23, "frame"},
      {Role::dialog, 16, "dialog"},
      {Role::button, 43, "push button"},
      {Role::check_box, 7, "check box"},
      {Role::radio_button, 44, "radio button"},
      {Role::text_field, 79, "entry"},
      {Role::number_field, 52, "spin button"},
      {Role::drop_down, 11, "combo box"},
      {Role::list, 31, "list"},
      {Role::list_row, 32, "list item"},
      {Role::menu_bar, 34, "menu bar"},
      {Role::menu, 33, "menu"},
      {Role::menu_item, 35, "menu item"},
      {Role::check_menu_item, 8, "check menu item"},
      {Role::radio_menu_item, 45, "radio menu item"},
      {Role::tab_list, 38, "page tab list"},
      {Role::tab, 37, "page tab"},
      {Role::scroll_bar, 48, "scroll bar"},
      {Role::slider, 51, "slider"},
      {Role::scroll_pane, 49, "scroll pane"},
      {Role::canvas, 6, "canvas"},
      {Role::label, 29, "label"},
      {Role::status, 54, "status bar"},
      {Role::image, 27, "image"},
      {Role::group, 39, "panel"}};
  expect(std::size(expected) == 26, "the table covers every Role");
  for (const auto &entry : expected) {
    expect(role_code(entry.role) == entry.code,
           "role code matches for " + std::string(entry.name));
    expect(role_name(entry.role) == entry.name,
           "role name matches at code " + std::to_string(entry.code));
  }
}

void text_fields_refine_their_role() {
  Node field;
  field.role = Role::text_field;
  expect(role_code(field) == 79 && role_name(field) == "entry",
         "an editable text field is an entry");
  field.states.read_only = true;
  expect(role_code(field) == role_text && role_name(field) == "text",
         "a read-only text field is text");
  field.states.password = true;
  expect(role_code(field) == role_password_text &&
             role_name(field) == "password text",
         "a masked text field is password text");
  field.role = Role::button;
  expect(role_code(field) == 43 && role_name(field) == "push button",
         "other roles ignore text flags");
}

void state_bits_follow_the_model() {
  Node node;
  auto bits = state_bits(node);
  expect(has(bits, State::enabled) && has(bits, State::sensitive),
         "enabled and sensitive default on");
  expect(has(bits, State::showing) && has(bits, State::visible),
         "showing and visible default on");
  expect(!has(bits, State::focused) && !has(bits, State::checked),
         "unset flags stay clear");
  expect(!has(bits, State::single_line) && !has(bits, State::multi_line),
         "line states need text");

  node.states.disabled = true;
  bits = state_bits(node);
  expect(!has(bits, State::enabled) && !has(bits, State::sensitive),
         "a disabled node drops enabled and sensitive");

  node = Node{};
  node.states.checkable = true;
  node.states.checked = true;
  node.states.mixed = true;
  node.states.focusable = true;
  node.states.focused = true;
  node.states.selectable = true;
  node.states.selected = true;
  node.states.modal = true;
  node.states.active = true;
  node.states.resizable = true;
  bits = state_bits(node);
  expect(has(bits, State::checkable) && has(bits, State::checked),
         "checkable and checked follow the model");
  expect(has(bits, State::indeterminate), "mixed is indeterminate");
  expect(has(bits, State::focusable) && has(bits, State::focused),
         "focus states follow the model");
  expect(has(bits, State::selectable) && has(bits, State::selected),
         "selection states follow the model");
  expect(has(bits, State::modal) && has(bits, State::active) &&
             has(bits, State::resizable),
         "window states follow the model");

  node = Node{};
  node.states.expanded = true;
  bits = state_bits(node);
  expect(has(bits, State::expanded) && has(bits, State::expandable) &&
             !has(bits, State::collapsed),
         "an expanded node is expandable");
  node.states.expanded = false;
  node.states.collapsed = true;
  bits = state_bits(node);
  expect(has(bits, State::collapsed) && has(bits, State::expandable) &&
             !has(bits, State::expanded),
         "a collapsed node is expandable");
}

void text_states_follow_the_model() {
  Node node;
  node.text = Text{"typed", 0, 0, {}, {}};
  node.states.editable = true;
  auto bits = state_bits(node);
  expect(has(bits, State::editable) && has(bits, State::single_line) &&
             !has(bits, State::multi_line),
         "an editable field is editable single line");
  node.states.read_only = true;
  bits = state_bits(node);
  expect(has(bits, State::read_only) && !has(bits, State::editable),
         "a read-only field is not editable");
  node.states.read_only = false;
  node.states.multi_line = true;
  bits = state_bits(node);
  expect(has(bits, State::multi_line) && !has(bits, State::single_line),
         "a multi-line field reports multi line");
  node.states.disabled = true;
  bits = state_bits(node);
  expect(has(bits, State::editable), "disabling keeps the editable state");
}

void state_event_names_match_at_spi() {
  expect(state_names(StateKey::focused) ==
             std::vector<std::string_view>{"focused"},
         "focus reports the focused name");
  expect(state_names(StateKey::checked) ==
             std::vector<std::string_view>{"checked"},
         "checks report the checked name");
  expect(state_names(StateKey::mixed) ==
             std::vector<std::string_view>{"indeterminate"},
         "mixed reports indeterminate");
  expect(state_names(StateKey::disabled) ==
             std::vector<std::string_view>{"enabled", "sensitive"},
         "disabling reports both enabled names");
  expect(!state_value_matches(StateKey::disabled),
         "disabling reports the negated value");
  expect(state_value_matches(StateKey::checked),
         "other flags report the model value");
  expect(state_names(StateKey::password).empty(),
         "masking changes the role, not a state");
}

void interfaces_follow_the_node() {
  Node root;
  root.role = Role::application;
  expect(interfaces(root, true) ==
             std::vector<std::string_view>{"org.a11y.atspi.Accessible",
                                           "org.a11y.atspi.Application"},
         "the root is the application");

  Node group;
  expect(interfaces(group, false) ==
             std::vector<std::string_view>{"org.a11y.atspi.Accessible"},
         "a bounds-less group has no component");

  Node list;
  list.role = Role::list;
  list.bounds = {0.0, 0.0, 10.0, 10.0};
  list.actions = {"select"};
  list.text = Text{"row", 0, 0, {}, {}};
  list.range = Range{};
  list.states.editable = true;
  expect(interfaces(list, false) ==
             std::vector<std::string_view>{"org.a11y.atspi.Accessible",
                                           "org.a11y.atspi.Component",
                                           "org.a11y.atspi.Action",
                                           "org.a11y.atspi.Text",
                                           "org.a11y.atspi.EditableText",
                                           "org.a11y.atspi.Value",
                                           "org.a11y.atspi.Selection"},
         "a full node exposes every matching interface");

  list.states.read_only = true;
  expect(!exposes(interfaces(list, false), "org.a11y.atspi.EditableText"),
         "a read-only node exposes no editable text");
  list.states.read_only = false;
  list.states.disabled = true;
  expect(!exposes(interfaces(list, false), "org.a11y.atspi.EditableText"),
         "a disabled node exposes no editable text");

  Node tab_list;
  tab_list.role = Role::tab_list;
  tab_list.bounds = {0.0, 0.0, 10.0, 10.0};
  expect(exposes(interfaces(tab_list, false), "org.a11y.atspi.Selection"),
         "a page tab list exposes selection");
  Node plain_group;
  plain_group.bounds = {0.0, 0.0, 10.0, 10.0};
  expect(!exposes(interfaces(plain_group, false), "org.a11y.atspi.Selection"),
         "a plain group exposes no selection");
}

void utf8_offsets_translate_both_ways() {
  const std::string value = "a\xe2\x82\xac"
                            "b"; // a, euro sign, b
  expect(char_offset(value, 0) == 0 && char_offset(value, 1) == 1,
         "leading bytes map to character offsets");
  expect(!char_offset(value, 2) && !char_offset(value, 3),
         "offsets inside a character are rejected");
  expect(char_offset(value, 4) == 2 && char_offset(value, 5) == 3,
         "the end offset is the character count");
  expect(!char_offset(value, 6), "a byte offset past the end is rejected");
  expect(byte_offset(value, 0) == 0 && byte_offset(value, 1) == 1 &&
             byte_offset(value, 2) == 4 && byte_offset(value, 3) == 5,
         "character offsets map back to byte offsets");
  expect(!byte_offset(value, 4), "a character offset past the end is rejected");
  const std::string invalid = "\xc3";
  expect(!char_offset(invalid, 1) && !byte_offset(invalid, 0),
         "invalid UTF-8 is rejected");
}

void boundaries_follow_reading_units() {
  const std::string text = "alpha beta. gamma\nsecond line";
  auto boundary = boundary_at(text, 6, Granularity::character);
  expect(boundary && boundary->start == 6 && boundary->end == 7 &&
             boundary->text == "b",
         "character granularity returns one character");
  boundary = boundary_at(text, 5, Granularity::character);
  expect(boundary && boundary->text == " ", "spaces are characters too");
  boundary = boundary_at(text, 6, Granularity::word);
  expect(boundary && boundary->start == 6 && boundary->end == 10 &&
             boundary->text == "beta",
         "word granularity returns the whole word");
  boundary = boundary_at(text, 5, Granularity::word);
  expect(boundary && boundary->text == " ", "separator runs are words");
  boundary = boundary_at(text, 0, Granularity::sentence);
  expect(boundary && boundary->start == 0 && boundary->end == 11 &&
             boundary->text == "alpha beta.",
         "a sentence ends after its terminator");
  boundary = boundary_at(text, 12, Granularity::sentence);
  expect(boundary && boundary->start == 12 && boundary->end == 29,
         "the trailing sentence keeps the newline");
  boundary = boundary_at(text, 12, Granularity::line);
  expect(boundary && boundary->start == 0 && boundary->end == 17 &&
             boundary->text == "alpha beta. gamma",
         "lines split on newlines");
  boundary = boundary_at(text, 18, Granularity::line);
  expect(boundary && boundary->text == "second line",
         "the last line runs to the end");
  boundary = boundary_at(text, 18, Granularity::paragraph);
  expect(boundary && boundary->text == "second line",
         "paragraphs are lines in this model");
  boundary = boundary_at(text, text.size(), Granularity::character);
  expect(boundary && boundary->start == 29 && boundary->end == 29 &&
             boundary->text.empty(),
         "the end offset is an empty boundary");
  expect(!boundary_at(text, 30, Granularity::line),
         "an offset past the end is rejected");

  const std::string multibyte = "a\xe2\x82\xac"
                                "b";
  boundary = boundary_at(multibyte, 1, Granularity::character);
  expect(boundary && boundary->start == 1 && boundary->end == 2 &&
             boundary->text == "\xe2\x82\xac",
         "a character boundary holds the whole code point");
  boundary = boundary_at(multibyte, 1, Granularity::word);
  expect(boundary && boundary->start == 0 && boundary->end == 3,
         "non-ASCII characters join the word");
  expect(!boundary_at("\xc3", 0, Granularity::word),
         "invalid UTF-8 is rejected");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(every_role_has_a_code_and_name),
                 LAYERC_TEST_CASE(text_fields_refine_their_role),
                 LAYERC_TEST_CASE(state_bits_follow_the_model),
                 LAYERC_TEST_CASE(text_states_follow_the_model),
                 LAYERC_TEST_CASE(state_event_names_match_at_spi),
                 LAYERC_TEST_CASE(interfaces_follow_the_node),
                 LAYERC_TEST_CASE(utf8_offsets_translate_both_ways),
                 LAYERC_TEST_CASE(boundaries_follow_reading_units))
