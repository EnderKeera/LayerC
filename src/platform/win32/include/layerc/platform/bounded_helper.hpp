#pragma once

#include "layerc/platform/win32_handle.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <windows.h>

namespace layerc::platform {

// Quotes one argument so CommandLineToArgvW recovers it exactly.
[[nodiscard]] std::wstring quote_command_argument(std::wstring_view value);

struct BoundedHelper {
  OwnedHandle job;
  OwnedHandle process;
  DWORD process_id{};

  void terminate(UINT exit_code) const noexcept;
};

enum class HelperLaunchStep {
  attributes,
  create_job,
  set_limits,
  create_process,
  assign_job,
  validate,
  resume,
};
struct HelperLaunchError {
  HelperLaunchStep step{};
  DWORD code{};
};

struct HelperLaunchOptions {
  std::span<const HANDLE> inherited_handles{};
  DWORD creation_flags{};
  // Extra restrictions are OR-ed with KILL_ON_JOB_CLOSE (never replace it).
  DWORD job_flags{};
  DWORD active_process_limit{};
  SIZE_T process_memory_limit{};
  // Runs while the child is suspended and Job-owned, before ResumeThread.
  std::function<bool(HANDLE, DWORD)> validate_suspended;
  // Fault-injection seam for the Job assignment failure path; production
  // callers leave this empty.
  std::function<bool(HANDLE)> fail_assignment_for_test;
};

// Never returns a running child without a kill-on-close Job. On every failure
// after CreateProcessW, terminates and waits for the suspended child.
[[nodiscard]] std::variant<BoundedHelper, HelperLaunchError>
launch_bounded_helper(const std::filesystem::path &image,
                      const std::wstring &command,
                      const HelperLaunchOptions &options = {});

} // namespace layerc::platform
