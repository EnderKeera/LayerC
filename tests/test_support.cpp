#include "test_support.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace layerc::test {
namespace {

struct SkippedCase {};

std::atomic_uint64_t temp_sequence{};

[[nodiscard]] std::uint64_t process_id() noexcept {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

#ifdef _WIN32
[[nodiscard]] std::wstring wide_name(const std::string &name) {
  return {name.begin(), name.end()}; // Variable names are ASCII.
}

[[nodiscard]] std::optional<std::wstring>
read_environment(const std::string &name) {
  const auto wide = wide_name(name);
  const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
  if (size == 0) return std::nullopt;
  std::wstring value(size, L'\0');
  const DWORD length = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
  if (length >= size) return std::nullopt;
  value.resize(length);
  return value;
}

bool write_environment(const std::string &name, const wchar_t *value) {
  return SetEnvironmentVariableW(wide_name(name).c_str(), value) != FALSE;
}
#else
[[nodiscard]] std::optional<std::string>
read_environment(const std::string &name) {
  const char *value = std::getenv(name.c_str());
  if (value == nullptr) return std::nullopt;
  return std::string(value);
}

bool write_environment(const std::string &name, const char *value) {
  return value != nullptr ? ::setenv(name.c_str(), value, 1) == 0
                          : ::unsetenv(name.c_str()) == 0;
}
#endif

} // namespace

void disable_ime_off_input_desktop() {
#ifdef _WIN32
  const auto name = [](HDESK desktop) {
    wchar_t buffer[256]{};
    DWORD needed{};
    if (!GetUserObjectInformationW(desktop, UOI_NAME, buffer, sizeof buffer, &needed))
      return std::wstring{};
    return std::wstring(buffer);
  };
  const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  const bool off_input = !input || name(input) != name(GetThreadDesktop(GetCurrentThreadId()));
  if (input) CloseDesktop(input);
  if (!off_input) return;
  const HMODULE imm = LoadLibraryExW(L"imm32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!imm) return;
  using DisableIme = BOOL (WINAPI *)(DWORD);
  const auto disable = std::bit_cast<DisableIme>(GetProcAddress(imm, "ImmDisableIME"));
  if (disable) disable(static_cast<DWORD>(-1));
  FreeLibrary(imm);
#endif
}

int failures{};

LAYERC_TEST_RETURNS_CONDITION bool expect(const bool condition,
                                          const std::string_view message) {
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

TempDirectory::TempDirectory(const std::string_view prefix) {
  const auto root = std::filesystem::temp_directory_path();
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  for (;;) {
    auto candidate =
        root / (std::string(prefix) + "-" + std::to_string(process_id()) +
                "-" + std::to_string(ticks) + "-" +
                std::to_string(temp_sequence.fetch_add(1)));
    // create_directories reports false for an existing path, so a leftover
    // from a crashed run is never reused.
    if (std::filesystem::create_directories(candidate)) {
      path_ = std::move(candidate);
      return;
    }
  }
}

TempDirectory::~TempDirectory() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

bool write_file(const std::filesystem::path &path,
                const std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(output);
}

std::optional<std::string> read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

ScopedEnvironment::ScopedEnvironment(
    std::string name, const std::optional<std::filesystem::path> &value)
    : name_(std::move(name)), previous_(read_environment(name_)) {
  applied_ =
      write_environment(name_, value.has_value() ? value->c_str() : nullptr);
}

ScopedEnvironment::~ScopedEnvironment() {
  (void)write_environment(name_,
                          previous_.has_value() ? previous_->c_str() : nullptr);
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
