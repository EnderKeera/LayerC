#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

// Edge-triggers bindings whose whole chord becomes pressed. Only tokens that
// appear in some chord are tracked, and an event re-checks only the bindings
// that contain its token.
class InputMatcher {
public:
  explicit InputMatcher(std::span<const InputBinding> bindings);

  [[nodiscard]] std::vector<std::string> process(const InputEvent &event);
  void clear();
  void replace_bindings(std::span<const InputBinding> bindings);
  void seed(const InputToken &token, bool pressed);

private:
  struct Binding {
    std::string cooldown_id;
    std::vector<std::size_t> tokens;
  };

  [[nodiscard]] std::size_t token_index(const InputToken &token) const;
  [[nodiscard]] bool chord_pressed(const Binding &binding) const;

  std::vector<Binding> bindings_;
  // Sorted unique bound tokens; pressed_ and token_bindings_ are parallel.
  std::vector<InputToken> tokens_;
  std::vector<bool> pressed_;
  std::vector<std::vector<std::size_t>> token_bindings_;
};

} // namespace layerc::core
