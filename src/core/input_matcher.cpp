#include "layerc/core/input_matcher.hpp"

#include <algorithm>

namespace layerc::core {

InputMatcher::InputMatcher(const std::span<const InputBinding> bindings)
    : bindings_(bindings), active_(bindings_.size(), false) {}

std::vector<std::string> InputMatcher::process(const InputEvent &event) {
  if (event.pressed) {
    pressed_.insert(event.token);
  } else {
    pressed_.erase(event.token);
  }

  std::vector<std::string> triggered;
  for (std::size_t index = 0; index < bindings_.size(); ++index) {
    const bool is_pressed = chord_pressed(bindings_[index].chord);
    if (is_pressed && !active_[index]) {
      triggered.push_back(bindings_[index].cooldown_id);
    }
    active_[index] = is_pressed;
  }
  return triggered;
}

void InputMatcher::clear() {
  pressed_.clear();
  std::fill(active_.begin(), active_.end(), false);
}

bool InputMatcher::chord_pressed(const InputChord &chord) const {
  return !chord.tokens.empty() &&
         std::ranges::all_of(chord.tokens, [this](const InputToken &token) {
           return pressed_.contains(token);
         });
}

} // namespace layerc::core
