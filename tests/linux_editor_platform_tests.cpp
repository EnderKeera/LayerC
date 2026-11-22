#include "platform/calibration.hpp"
#include "platform/services.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace editor = layerc::editor;
namespace core = layerc::core;
namespace test = layerc::test;
using test::expect;

#ifndef _WIN32
std::optional<editor::FolderOpenExit> wait_for_exit(editor::FolderOpener &opener) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = opener.poll()) return result;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return std::nullopt;
}
#endif

void process_identity_is_stable() {
  const auto first = editor::current_process_id();
  expect(first != 0 && first == editor::current_process_id(),
         "the service exposes a stable, nonzero process identity");
}

#ifndef _WIN32
// POSIX launcher and in-process Linux calibration contracts are not the Windows shell/helper.
void missing_folder_launcher_remains_idle() {
  test::TempDirectory temporary{"layerc-folder-unavailable"};
  test::ScopedEnvironment path{"PATH", temporary.path()};
  if (!expect(path.applied(), "the test owns the executable search path")) return;
  editor::FolderOpener opener;
  expect(!opener.running() && !opener.poll(), "the folder service is initially idle");
  expect(!opener.start(temporary.path()), "a missing launcher reports failure");
  expect(!opener.running() && !opener.poll(), "spawn failure leaves no pending child");
  opener.stop();
  opener.stop();
  expect(!opener.running(), "idle shutdown is repeatable");
}

void folder_launcher_preserves_arguments_and_exit_status() {
  test::TempDirectory temporary{"layerc-folder-launcher"};
  const auto program = temporary.path() / "xdg-open";
  const auto log = temporary.path() / "argument.log";
  const auto directory = temporary.path() / "folder with spaces; $HOME 'quotes'";
  std::filesystem::create_directory(directory);
  test::ScopedEnvironment path{"PATH", temporary.path()};
  test::ScopedEnvironment output{"LAYERC_TEST_FOLDER_LOG", log};
  if (!expect(path.applied() && output.applied(), "launcher fixtures own their environment"))
    return;
  editor::FolderOpener opener;
  for (const auto code : std::array{0, 7}) {
    const auto script = std::string{"#!/bin/sh\nprintf '%s' \"$1\" > \"$LAYERC_TEST_FOLDER_LOG\"\nexit "} +
        std::to_string(code) + "\n";
    if (!expect(test::write_file(program, script), "the private launcher is written")) return;
    std::filesystem::permissions(program, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    if (!expect(opener.start(directory), "the private launcher starts on demand")) return;
    expect(opener.running(), "a launched child is retained until polled");
    expect(!opener.start(directory), "a pending child prevents a duplicate launch");
    const auto result = wait_for_exit(opener);
    expect(result && result->code == code, "normal child exit status is preserved");
    expect(test::read_file(log) == std::optional{directory.string()},
           "spaces and shell metacharacters remain one literal argument");
    expect(!opener.running() && !opener.poll(), "completed children are reaped once");
  }
}

void unavailable_calibration_has_a_fault_and_rejects_unknown_keys() {
  test::ScopedEnvironment display{"DISPLAY", std::nullopt};
  if (!expect(display.applied(), "the test cannot access a production display")) return;
  editor::CalibrationSource source;
  const core::GameMatch match;
  const auto scan = source.scan(match);
  expect(scan.windows.empty() && scan.fault && !scan.fault->empty(),
         "headless calibration retains a visible fault without a native handle");
  expect(!source.running(), "an unavailable scan does not start capture");
  for (const auto key : std::array{std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
    const auto result = source.start(key, match, core::CaptureQuality{});
    expect(result.state == editor::CalibrationStartState::identity_changed,
           "unrecognized opaque keys cannot select a native window");
    expect(!source.running(), "rejected keys leave capture stopped");
  }
  const auto sample = source.poll(0, core::TimePoint{});
  expect(!sample.identity_changed, "polling an idle source does not invent an identity change");
  source.set_quality(core::CaptureQuality{});
  source.stop();
  source.stop();
  expect(!source.running(), "headless source cleanup is repeatable");
}

#endif
} // namespace

#ifdef _WIN32
void windows_editor_platform_contracts() {
  process_identity_is_stable();
  std::cout << "Not applicable: missing_folder_launcher_remains_idle, "
               "folder_launcher_preserves_arguments_and_exit_status "
               "(xdg-open/POSIX; Windows uses the shell, no matching fake launcher)\n"
               "Not applicable: unavailable_calibration_has_a_fault_and_rejects_unknown_keys "
               "(headless X11 capture; Windows: layerc_windows_fltk_calibration_tests)\n";
}
#else
LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(process_identity_is_stable),
    LAYERC_TEST_CASE(missing_folder_launcher_remains_idle),
    LAYERC_TEST_CASE(folder_launcher_preserves_arguments_and_exit_status),
    LAYERC_TEST_CASE(unavailable_calibration_has_a_fault_and_rejects_unknown_keys))
#endif
