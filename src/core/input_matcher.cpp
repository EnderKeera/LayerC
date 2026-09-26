#include "layerc/core/input_matcher.hpp"

#include <algorithm>

namespace layerc::core {

InputMatcher::InputMatcher(const std::span<const InputBinding> bindings) {
  replace_bindings(bindings);
}

std::vector<std::string> InputMatcher::process(const InputEvent &event) {
  const auto index = token_index(event.token);
  if (index == tokens_.size() || pressed_[index] == event.pressed)
    return {};
  pressed_[index] = event.pressed;
  if (!event.pressed)
    return {};

  // A chord containing this token could not have been pressed before, so every
  // chord that is fully pressed now has just become active.
  std::vector<std::string> triggered;
  for (const auto binding : token_bindings_[index]) {
    if (chord_pressed(bindings_[binding]))
      triggered.push_back(bindings_[binding].cooldown_id);
  }
  return triggered;
}

void InputMatcher::clear() {
  pressed_.assign(pressed_.size(), false);
}

void InputMatcher::replace_bindings(
    const std::span<const InputBinding> bindings) {
  tokens_.clear();
  for (const auto &binding : bindings)
    tokens_.insert(tokens_.end(), binding.chord.tokens.begin(),
                   binding.chord.tokens.end());
  std::ranges::sort(tokens_);
  const auto duplicates = std::ranges::unique(tokens_);
  tokens_.erase(duplicates.begin(), duplicates.end());
  pressed_.assign(tokens_.size(), false);
  token_bindings_.assign(tokens_.size(), {});

  bindings_.clear();
  bindings_.reserve(bindings.size());
  for (const auto &binding : bindings) {
    const auto binding_index = bindings_.size();
    auto &entry = bindings_.emplace_back(Binding{binding.cooldown_id, {}});
    entry.tokens.reserve(binding.chord.tokens.size());
    for (const auto &token : binding.chord.tokens) {
      const auto index = token_index(token);
      entry.tokens.push_back(index);
      auto &owners = token_bindings_[index];
      if (owners.empty() || owners.back() != binding_index)
        owners.push_back(binding_index);
    }
  }
}

void InputMatcher::seed(const InputToken &token, const bool pressed) {
  const auto index = token_index(token);
  if (index != tokens_.size())
    pressed_[index] = pressed;
}

std::size_t InputMatcher::token_index(const InputToken &token) const {
  const auto found = std::ranges::lower_bound(tokens_, token);
  if (found == tokens_.end() || *found != token)
    return tokens_.size();
  return static_cast<std::size_t>(found - tokens_.begin());
}

bool InputMatcher::chord_pressed(const Binding &binding) const {
  return !binding.tokens.empty() &&
         std::ranges::all_of(binding.tokens, [this](const std::size_t token) {
           return static_cast<bool>(pressed_[token]);
         });
}

} // namespace layerc::core
