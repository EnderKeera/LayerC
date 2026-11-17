#pragma once

#include "a11y/changes.hpp"
#include "a11y/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor::a11y::atspi {

// AT-SPI carries its state set as a 64-bit bitset in two uint32 words; bit
// number N lives in word N / 32 at bit N % 32.
using StateSet = std::array<std::uint32_t, 2>;

// AT-SPI state numbers (AtspiStateType in the protocol header).
enum class State : std::uint32_t {
  active = 1,
  checked = 4,
  collapsed = 5,
  editable = 7,
  enabled = 8,
  expandable = 9,
  expanded = 10,
  focusable = 11,
  focused = 12,
  modal = 16,
  multi_line = 17,
  resizable = 21,
  selectable = 22,
  selected = 23,
  sensitive = 24,
  showing = 25,
  single_line = 26,
  visible = 30,
  indeterminate = 32,
  checkable = 41,
  read_only = 43
};

// AT-SPI role numbers (AtspiRole in the protocol header).
inline constexpr std::uint32_t role_text = 61;
inline constexpr std::uint32_t role_password_text = 40;

[[nodiscard]] std::uint32_t role_code(Role role);
[[nodiscard]] std::string_view role_name(Role role);
// A read-only text field exports "text" and a masked one "password text", as
// the ATK tree this adaptor replaces did.
[[nodiscard]] std::uint32_t role_code(const Node &node);
[[nodiscard]] std::string_view role_name(const Node &node);

[[nodiscard]] StateSet state_bits(const Node &node);
// AT-SPI event minor names a state flip reports, in order. A disabled flip
// reports "enabled" and "sensitive": the negated value, see below.
[[nodiscard]] std::vector<std::string_view> state_names(StateKey key);
// Whether a flip reports the model flag itself, or its negation (disabled).
[[nodiscard]] bool state_value_matches(StateKey key);

// Interfaces a node exposes, as org.a11y.atspi.* names in a stable order.
[[nodiscard]] std::vector<std::string_view> interfaces(const Node &node,
                                                       bool is_root);

// A group whose direct children include a radio button. FLTK radio groups are
// ordinary Fl_Group containers, so the Selection interface follows from the
// children and needs the published snapshot, not one node.
[[nodiscard]] bool radio_group(const Snapshot &snapshot, const Node &node);

// Model offsets are UTF-8 bytes; AT-SPI offsets are characters (code points).
// Both directions reject invalid UTF-8 and offsets that split a character.
[[nodiscard]] std::optional<std::size_t> char_offset(std::string_view value,
                                                     std::size_t byte);
[[nodiscard]] std::optional<std::size_t> byte_offset(std::string_view value,
                                                     std::size_t character);

enum class Granularity { character, word, sentence, line, paragraph };

struct Boundary {
  std::size_t start{}, end{};
  std::string text;
};

// Text boundaries for GetStringAtOffset and GetTextAtOffset, in characters.
// Words are runs of one character class (alphanumeric, underscore, or any
// non-ASCII character, versus everything else); sentences end after '.', '!'
// or '?' followed by whitespace or the end of the text; lines split on '\n';
// paragraphs are lines in this model.
[[nodiscard]] std::optional<Boundary>
boundary_at(std::string_view value, std::size_t character,
            Granularity granularity);

} // namespace layerc::editor::a11y::atspi
