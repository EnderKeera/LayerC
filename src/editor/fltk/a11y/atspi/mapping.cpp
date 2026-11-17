#include "a11y/atspi/mapping.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <utility>

namespace layerc::editor::a11y::atspi {
namespace {

struct RoleEntry {
  std::uint32_t code;
  std::string_view name;
};

// Indexed by Role. Numbers and names match the ATK tree this adaptor replaces;
// AT-SPI role names are the atk-bridge spellings, so a by-name client mapping
// keeps working.
constexpr RoleEntry roles[] = {
    {75, "application"},      {23, "frame"},
    {16, "dialog"},           {43, "push button"},
    {7, "check box"},         {44, "radio button"},
    {79, "entry"},            {52, "spin button"},
    {11, "combo box"},        {31, "list"},
    {32, "list item"},        {34, "menu bar"},
    {33, "menu"},             {35, "menu item"},
    {8, "check menu item"},   {45, "radio menu item"},
    {38, "page tab list"},    {37, "page tab"},
    {48, "scroll bar"},       {51, "slider"},
    {49, "scroll pane"},      {6, "canvas"},
    {29, "label"},            {54, "status bar"},
    {27, "image"},            {39, "panel"}};
constexpr std::size_t role_count = 26;
static_assert(std::size(roles) == role_count, "one entry per Role");
static_assert(static_cast<std::size_t>(Role::group) == role_count - 1,
              "Role order decides the table order");

constexpr RoleEntry role_entry(const Role role) {
  return roles[static_cast<std::size_t>(role)];
}

void add(StateSet &bits, const State state, const bool present) {
  if (!present) return;
  const auto number = static_cast<std::size_t>(state);
  bits[number / 32] |= 1U << (number % 32);
}

bool is_space(const char byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

bool is_word(const char byte) {
  const auto value = static_cast<unsigned char>(byte);
  if (value >= 0x80U) return true;
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_terminator(const char byte) {
  return byte == '.' || byte == '!' || byte == '?';
}

using Starts = std::vector<std::size_t>;

std::optional<Starts> starts_of(const std::string_view value) {
  if (!layerc::core::is_valid_utf8(value)) return std::nullopt;
  Starts starts;
  starts.reserve(value.size() + 1);
  for (std::size_t index = 0; index < value.size(); ++index) {
    if ((static_cast<unsigned char>(value[index]) & 0xc0U) != 0x80U)
      starts.push_back(index);
  }
  starts.push_back(value.size()); // Sentinel: one past the last character.
  return starts;
}

using Span = std::pair<std::size_t, std::size_t>;

std::vector<Span> spans(const std::string_view value, const Starts &starts,
                        const Granularity granularity) {
  const std::size_t count = starts.size() - 1;
  std::vector<Span> spans;
  if (granularity == Granularity::character) {
    for (std::size_t index = 0; index < count; ++index)
      spans.emplace_back(index, index + 1);
    return spans;
  }
  if (granularity == Granularity::word) {
    std::size_t begin = 0;
    while (begin < count) {
      std::size_t end = begin + 1;
      while (end < count && is_word(value[starts[end]]) ==
                                is_word(value[starts[begin]]))
        ++end;
      spans.emplace_back(begin, end);
      begin = end;
    }
    return spans;
  }
  if (granularity == Granularity::sentence) {
    std::size_t begin = 0;
    std::size_t index = 0;
    while (index < count) {
      const bool ends =
          is_terminator(value[starts[index]]) &&
          (index + 1 == count || is_space(value[starts[index + 1]]));
      if (!ends) {
        ++index;
        continue;
      }
      spans.emplace_back(begin, index + 1);
      begin = index + 1;
      while (begin < count && is_space(value[starts[begin]])) ++begin;
      index = begin;
    }
    spans.emplace_back(begin, count);
    return spans;
  }
  std::size_t begin = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (value[starts[index]] != '\n') continue;
    spans.emplace_back(begin, index);
    begin = index + 1;
  }
  spans.emplace_back(begin, count);
  return spans;
}

std::optional<Boundary> span_at(const std::string_view value,
                                const Starts &starts, const std::size_t character,
                                const Granularity granularity) {
  const std::size_t count = starts.size() - 1;
  if (character > count) return std::nullopt;
  const auto candidates = spans(value, starts, granularity);
  if (granularity == Granularity::character && character == count)
    return Boundary{character, character, {}};
  if (candidates.empty()) return Boundary{character, character, {}};
  const Span *found = nullptr;
  for (const auto &span : candidates) {
    if (span.first <= character && character < span.second) {
      found = &span;
      break;
    }
  }
  if (found == nullptr && character == count) found = &candidates.back();
  if (found == nullptr) return std::nullopt;
  return Boundary{found->first, found->second,
                  std::string(value.substr(starts[found->first],
                                           starts[found->second] -
                                               starts[found->first]))};
}

} // namespace

std::uint32_t role_code(const Role role) { return role_entry(role).code; }

std::string_view role_name(const Role role) { return role_entry(role).name; }

std::uint32_t role_code(const Node &node) {
  if (node.role != Role::text_field) return role_code(node.role);
  if (node.states.password) return role_password_text;
  if (node.states.read_only) return role_text;
  return role_code(node.role);
}

std::string_view role_name(const Node &node) {
  if (node.role == Role::text_field) {
    if (node.states.password) return "password text";
    if (node.states.read_only) return "text";
  }
  return role_name(node.role);
}

StateSet state_bits(const Node &node) {
  StateSet bits{};
  const auto &states = node.states;
  add(bits, State::visible, states.visible);
  add(bits, State::showing, states.showing);
  add(bits, State::enabled, !states.disabled);
  add(bits, State::sensitive, !states.disabled);
  add(bits, State::modal, states.modal);
  add(bits, State::active, states.active);
  add(bits, State::resizable, states.resizable);
  add(bits, State::focusable, states.focusable);
  add(bits, State::focused, states.focused);
  add(bits, State::editable, states.editable && !states.read_only);
  add(bits, State::read_only, states.read_only);
  if (node.text) {
    add(bits, State::multi_line, states.multi_line);
    add(bits, State::single_line, !states.multi_line);
  }
  add(bits, State::checkable, states.checkable);
  add(bits, State::selectable, states.selectable);
  add(bits, State::selected, states.selected);
  add(bits, State::checked, states.checked);
  add(bits, State::indeterminate, states.mixed);
  add(bits, State::expanded, states.expanded);
  add(bits, State::collapsed, states.collapsed);
  add(bits, State::expandable, states.expanded || states.collapsed);
  return bits;
}

std::vector<std::string_view> state_names(const StateKey key) {
  switch (key) {
  case StateKey::focused: return {"focused"};
  case StateKey::focusable: return {"focusable"};
  case StateKey::selected: return {"selected"};
  case StateKey::selectable: return {"selectable"};
  case StateKey::checked: return {"checked"};
  case StateKey::mixed: return {"indeterminate"};
  case StateKey::expanded: return {"expanded"};
  case StateKey::collapsed: return {"collapsed"};
  case StateKey::disabled: return {"enabled", "sensitive"};
  case StateKey::read_only: return {"read-only"};
  case StateKey::modal: return {"modal"};
  case StateKey::showing: return {"showing"};
  case StateKey::password:
    // Masking changes the role (entry to password text), not a state.
    return {};
  case StateKey::visible: return {"visible"};
  case StateKey::active: return {"active"};
  case StateKey::resizable: return {"resizable"};
  case StateKey::editable: return {"editable"};
  case StateKey::multi_line: return {"multi-line"};
  case StateKey::checkable: return {"checkable"};
  }
  return {};
}

bool state_value_matches(const StateKey key) { return key != StateKey::disabled; }

std::vector<std::string_view> interfaces(const Node &node, const bool is_root) {
  std::vector<std::string_view> names{"org.a11y.atspi.Accessible"};
  if (is_root) {
    names.emplace_back("org.a11y.atspi.Application");
    return names;
  }
  if (node.bounds.width > 0.0 && node.bounds.height > 0.0)
    names.emplace_back("org.a11y.atspi.Component");
  if (!node.actions.empty()) names.emplace_back("org.a11y.atspi.Action");
  if (node.text) {
    names.emplace_back("org.a11y.atspi.Text");
    if (node.states.editable && !node.states.read_only && !node.states.disabled)
      names.emplace_back("org.a11y.atspi.EditableText");
  }
  if (node.range) names.emplace_back("org.a11y.atspi.Value");
  if (node.role == Role::list || node.role == Role::tab_list ||
      node.role == Role::drop_down || node.role == Role::menu)
    names.emplace_back("org.a11y.atspi.Selection");
  return names;
}

bool radio_group(const Snapshot &snapshot, const Node &node) {
  if (node.role != Role::group) return false;
  for (const NodeId child : node.children) {
    const auto found = snapshot.nodes.find(child);
    if (found != snapshot.nodes.end() && found->second.role == Role::radio_button)
      return true;
  }
  return false;
}

std::optional<std::size_t> char_offset(const std::string_view value,
                                       const std::size_t byte) {
  const auto starts = starts_of(value);
  if (!starts || byte > value.size()) return std::nullopt;
  const auto found = std::lower_bound(starts->begin(), starts->end(), byte);
  if (found == starts->end() || *found != byte) return std::nullopt;
  return static_cast<std::size_t>(found - starts->begin());
}

std::optional<std::size_t> byte_offset(const std::string_view value,
                                       const std::size_t character) {
  const auto starts = starts_of(value);
  if (!starts || character >= starts->size()) return std::nullopt;
  return (*starts)[character];
}

std::optional<Boundary> boundary_at(const std::string_view value,
                                    const std::size_t character,
                                    const Granularity granularity) {
  const auto starts = starts_of(value);
  if (!starts) return std::nullopt;
  return span_at(value, *starts, character, granularity);
}

} // namespace layerc::editor::a11y::atspi
