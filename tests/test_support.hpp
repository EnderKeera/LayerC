#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
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

// Skips the current case when DISPLAY is unset. X11 suites still verify
// connection and extension availability before exercising live operations.
inline void require_x11() {
  if (!x11_display_available()) skip();
}

// Owns a fresh directory under the system temp root, unique per process and
// instance so parallel CTest runs never share fixtures. Removed recursively
// on destruction, including during exception unwinding.
class TempDirectory {
public:
  explicit TempDirectory(std::string_view prefix);
  ~TempDirectory();
  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

// Sets one environment variable (std::nullopt clears it) and restores the
// previous value or absence on destruction, including during unwinding.
class ScopedEnvironment {
public:
  ScopedEnvironment(std::string name,
                    const std::optional<std::filesystem::path> &value);
  ~ScopedEnvironment();
  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

  [[nodiscard]] bool applied() const noexcept { return applied_; }

private:
  std::string name_;
  std::optional<std::filesystem::path::string_type> previous_;
  bool applied_{};
};

// Binary file fixtures. write_file creates missing parent directories;
// read_file returns nullopt when the file cannot be opened.
bool write_file(const std::filesystem::path &path, std::string_view contents);
[[nodiscard]] std::optional<std::string>
read_file(const std::filesystem::path &path);

[[nodiscard]] int run(std::initializer_list<Case> cases);

} // namespace layerc::test

#define LAYERC_TEST_CASE(function) \
  ::layerc::test::Case{#function, [] { function(); }}

#define LAYERC_TEST_MAIN(...)                                                \
  int main() { return ::layerc::test::run({__VA_ARGS__}); }
