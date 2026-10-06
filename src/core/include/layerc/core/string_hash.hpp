#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

namespace layerc::core {

// Transparent hashing lets std::string-keyed unordered containers look entries
// up by string_view without constructing temporary keys on hot paths. Pair it
// with std::equal_to<>.
struct TransparentStringHash {
  using is_transparent = void;
  [[nodiscard]] std::size_t
  operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

} // namespace layerc::core
