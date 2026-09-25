#include "layerc/core/input_matcher.hpp"

#include <algorithm>

namespace layerc::core {
namespace {

void record_press(std::set<InputToken> &pressed, const InputToken &token,
                  const bool is_pressed) {
  if (is_pressed)
    pressed.insert(token);
  else
    pressed.erase(token);
}

} // namespace

InputMatcher::InputMatcher(const std::span<const InputBinding> bindings)
    : bindings_(bindings.begin(), bindings.end()),
      active_(bindings_.size(), false) {}

std::vector<std::string> InputMatcher::process(const InputEvent &event) {
  record_press(pressed_, event.token, event.pressed);

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

void InputMatcher::replace_bindings(
    const std::span<const InputBinding> bindings) {
  bindings_.assign(bindings.begin(), bindings.end());
  pressed_.clear();
  active_.assign(bindings_.size(), false);
}

void InputMatcher::seed(const InputToken &token, const bool pressed) {
  record_press(pressed_, token, pressed);
  for (std::size_t index = 0; index < bindings_.size(); ++index)
    active_[index] = chord_pressed(bindings_[index].chord);
}

bool InputMatcher::chord_pressed(const InputChord &chord) const {
  return !chord.tokens.empty() &&
         std::ranges::all_of(chord.tokens, [this](const InputToken &token) {
           return pressed_.contains(token);
         });
}

} // namespace layerc::core
