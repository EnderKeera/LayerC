#pragma once

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#include <sal.h>
// Tells /analyze that expect returns its condition, so a case guarded by
// `if (!expect(pointer != nullptr, ...)) return;` may dereference it.
#define LAYERC_TEST_RETURNS_CONDITION _Post_equal_to_(condition)
#else
#define LAYERC_TEST_RETURNS_CONDITION
#endif

namespace layerc::test {

extern int failures;
LAYERC_TEST_RETURNS_CONDITION bool expect(bool condition,
                                          std::string_view message);

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

// For suites that open FLTK windows: skips like require_x11, then selects
// FLTK's X11 backend as LayerE itself does (src/editor/fltk/main.cpp), so a
// reachable Wayland compositor cannot change the backend under test. Call it
// before the suite's first FLTK window.
inline void require_fltk_x11() {
  require_x11();
#ifndef _WIN32
  if (::setenv("FLTK_BACKEND", "x11", 1) != 0) skip();
#endif
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
