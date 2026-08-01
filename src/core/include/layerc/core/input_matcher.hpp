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
  void replace_bindings(std::span<const InputBinding> bindings);
  void seed(const InputToken &token, bool pressed);

private:
  [[nodiscard]] bool chord_pressed(const InputChord &chord) const;

  std::vector<InputBinding> bindings_;
  std::set<InputToken> pressed_;
  std::vector<bool> active_;
};

} // namespace layerc::core
