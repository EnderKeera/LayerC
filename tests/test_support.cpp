#include "test_support.hpp"

#include <iostream>

namespace layerc::test {

int failures{};

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

} // namespace layerc::test