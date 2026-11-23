#include "test_support.hpp"

#include <algorithm>
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

[[nodiscard]] std::optional<std::string> user_object_name(HANDLE object) {
  wchar_t buffer[256]{};
  DWORD needed{};
  if (!object || !GetUserObjectInformationW(object, UOI_NAME, buffer, sizeof buffer, &needed))
    return std::nullopt;
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 1) return std::nullopt;
  std::string name(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1, name.data(),
                          size, nullptr, nullptr) != size)
    return std::nullopt;
  name.pop_back();
  return name;
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

ImeDecision decide_ime_for_desktop(const DesktopIdentityProbe &probe) {
  if (!probe.input_opened)
    return {false, "OpenInputDesktop failed, error " + std::to_string(probe.open_error)};
  if (!probe.station || probe.station->empty())
    return {false, "could not read the window station name"};
  constexpr std::string_view interactive_station = "winsta0";
  const auto lower_ascii = [](const char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
  };
  if (!std::ranges::equal(*probe.station, interactive_station, {}, lower_ascii))
    return {false, "window station is not WinSta0: " + *probe.station};
  if (!probe.input_name || probe.input_name->empty())
    return {false, "could not read the input desktop name"};
  if (!probe.thread_name || probe.thread_name->empty())
    return {false, "could not read the thread desktop name"};
  if (*probe.input_name == *probe.thread_name)
    return {false, "thread is on the input desktop: " + *probe.input_name};
  return {true, *probe.thread_name + " vs " + *probe.input_name};
}

void disable_ime_off_input_desktop() {
#ifdef _WIN32
  DesktopIdentityProbe probe;
  const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (input) {
    probe.input_opened = true;
    probe.station = user_object_name(GetProcessWindowStation());
    probe.input_name = user_object_name(input);
    probe.thread_name = user_object_name(GetThreadDesktop(GetCurrentThreadId()));
    CloseDesktop(input);
  } else {
    probe.open_error = GetLastError();
  }
  const auto decision = decide_ime_for_desktop(probe);
  if (!decision.disable) {
    std::cerr << "IME: left enabled: " << decision.reason << '\n';
    return;
  }

  const HMODULE imm = LoadLibraryExW(L"imm32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!imm) {
    const DWORD error = GetLastError();
    std::cerr << "IME: left enabled: LoadLibraryExW failed, error " << error << '\n';
    return;
  }
  using DisableIme = BOOL (WINAPI *)(DWORD);
  const auto proc = GetProcAddress(imm, "ImmDisableIME");
  if (!proc) {
    const DWORD error = GetLastError();
    std::cerr << "IME: left enabled: GetProcAddress failed, error " << error << '\n';
  } else {
    const auto disable = std::bit_cast<DisableIme>(proc);
    SetLastError(ERROR_SUCCESS);
    const bool disabled = disable(static_cast<DWORD>(-1)) != FALSE;
    const DWORD error = disabled ? ERROR_SUCCESS : GetLastError();
    std::cerr << "IME: disabled off the input desktop (" << decision.reason << "); ImmDisableIME ";
    if (disabled) std::cerr << "succeeded\n";
    else std::cerr << "failed, error " << error << '\n';
  }
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
