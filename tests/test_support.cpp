#include "test_support.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

namespace layerc::test {
namespace {

struct SkippedCase {};

} // namespace

int failures{};

bool expect(const bool condition, const std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

[[noreturn]] void skip() { throw SkippedCase{}; }

bool x11_display_available() {
#ifdef _WIN32
  return false; // The live-X11 suites build only on Linux.
#else
  return std::getenv("DISPLAY") != nullptr;
#endif
}

int run(const std::initializer_list<Case> cases) {
  failures = 0;
  std::size_t passed_cases{};
  std::size_t skipped_cases{};
  for (const auto &test_case : cases) {
    const int before = failures;
    bool skipped{};
    try {
      test_case.run();
    } catch (const SkippedCase &) {
      skipped = true;
      ++skipped_cases;
      std::cout << "SKIP: " << test_case.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL: " << test_case.name << ": uncaught exception: "
                << error.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "FAIL: " << test_case.name << ": uncaught exception\n";
    }
    if (failures != before) {
      std::cerr << "CASE FAILED: " << test_case.name << " ("
                << failures - before << " assertion(s))\n";
    } else if (!skipped) {
      ++passed_cases;
      std::cout << "PASS: " << test_case.name << '\n';
    }
  }
  std::cout << passed_cases << '/' << cases.size() << " case(s) passed";
  if (skipped_cases != 0) std::cout << ", " << skipped_cases << " skipped";
  std::cout << '\n';
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  return skipped_cases == 0 ? EXIT_SUCCESS : skip_code;
}

} // namespace layerc::test
