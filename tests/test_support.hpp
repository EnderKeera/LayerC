#pragma once

#include <functional>
#include <initializer_list>
#include <string_view>

namespace layerc::test {

extern int failures;
bool expect(bool condition, std::string_view message);

struct Case {
  std::string_view name;
  std::function<void()> run;
};

[[noreturn]] void skip();

// CTest treats 77 as "skipped" (SKIP_RETURN_CODE in CMakeLists.txt); suites
// that need a live X11 display return it instead of failing without one.
inline constexpr int skip_code = 77;
[[nodiscard]] bool x11_display_available();

[[nodiscard]] int run(std::initializer_list<Case> cases);

} // namespace layerc::test

#define LAYERC_TEST_CASE(function) \
  ::layerc::test::Case{#function, [] { function(); }}

#define LAYERC_TEST_MAIN(...)                                                \
  int main() { return ::layerc::test::run({__VA_ARGS__}); }
