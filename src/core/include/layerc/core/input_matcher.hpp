#pragma once

#include "layerc/core/types.hpp"

#include <set>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

class InputMatcher {
public:
  explicit InputMatcher(std::span<const InputBinding> bindings);

  [[nodiscard]] std::vector<std::string> process(const InputEvent &event);
  void clear();

private:
  [[nodiscard]] bool chord_pressed(const InputChord &chord) const;

  std::span<const InputBinding> bindings_;
  std::set<InputToken> pressed_;
  std::vector<bool> active_;
};

} // namespace layerc::core
