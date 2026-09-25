#include "layerc/platform/bounded_helper.hpp"

#include <cstddef>
#include <vector>

namespace layerc::platform {
namespace {

class AttributeList {
public:
  ~AttributeList() {
    if (initialized_) DeleteProcThreadAttributeList(list());
  }
  AttributeList(const AttributeList &) = delete;
  AttributeList &operator=(const AttributeList &) = delete;
  AttributeList() = default;

  [[nodiscard]] bool initialize(std::span<const HANDLE> handles) {
    SIZE_T size{};
    SetLastError(ERROR_SUCCESS);
    if (InitializeProcThreadAttributeList(nullptr, 1, 0, &size) != FALSE ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
      return false;
    storage_.resize(size);
    if (InitializeProcThreadAttributeList(list(), 1, 0, &size) == FALSE)
      return false;
    initialized_ = true;
    inherited_.assign(handles.begin(), handles.end());
    return UpdateProcThreadAttribute(list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     inherited_.data(), inherited_.size() * sizeof(HANDLE),
                                     nullptr, nullptr) != FALSE;
  }
  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept {
    return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
  }

private:
  std::vector<std::byte> storage_;
  std::vector<HANDLE> inherited_;
  bool initialized_{};
};

} // namespace

std::wstring quote_command_argument(const std::wstring_view value) {
  std::wstring output = L"\"";
  std::size_t slashes{};
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++slashes;
      continue;
    }
    if (character == L'\"') {
      output.append(slashes * 2U + 1U, L'\\');
      output.push_back(L'\"');
      slashes = 0;
      continue;
    }
    output.append(slashes, L'\\');
    slashes = 0;
    output.push_back(character);
  }
  output.append(slashes * 2U, L'\\');
  output.push_back(L'\"');
  return output;
}

void BoundedHelper::terminate(const UINT exit_code) const noexcept {
  if (job.valid()) (void)TerminateJobObject(job.get(), exit_code);
  if (process.valid()) {
    (void)TerminateProcess(process.get(), exit_code);
    (void)WaitForSingleObject(process.get(), 2'000);
  }
}

std::variant<BoundedHelper, HelperLaunchError>
launch_bounded_helper(const std::filesystem::path &image,
                      const std::wstring &command,
                      const HelperLaunchOptions &options) {
  AttributeList attributes;
  const bool inherit = !options.inherited_handles.empty();
  if (inherit && !attributes.initialize(options.inherited_handles))
    return HelperLaunchError{HelperLaunchStep::attributes, GetLastError()};

  BoundedHelper child;
  child.job.reset(CreateJobObjectW(nullptr, nullptr));
  if (!child.job.valid())
    return HelperLaunchError{HelperLaunchStep::create_job, GetLastError()};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | options.job_flags;
  limits.BasicLimitInformation.ActiveProcessLimit = options.active_process_limit;
  limits.ProcessMemoryLimit = options.process_memory_limit;
  if (SetInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation,
                              &limits, sizeof(limits)) == FALSE)
    return HelperLaunchError{HelperLaunchStep::set_limits, GetLastError()};

  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = inherit ? sizeof(startup) : sizeof(STARTUPINFOW);
  startup.lpAttributeList = inherit ? attributes.list() : nullptr;
  PROCESS_INFORMATION process_info{};
  const DWORD flags = options.creation_flags | CREATE_SUSPENDED |
                      (inherit ? EXTENDED_STARTUPINFO_PRESENT : 0);
  if (CreateProcessW(image.c_str(), mutable_command.data(), nullptr, nullptr,
                     inherit ? TRUE : FALSE, flags, nullptr,
                     image.parent_path().c_str(), &startup.StartupInfo,
                     &process_info) == FALSE)
    return HelperLaunchError{HelperLaunchStep::create_process, GetLastError()};
  child.process.reset(process_info.hProcess);
  OwnedHandle thread(process_info.hThread);
  child.process_id = process_info.dwProcessId;
  const auto fail = [&](const HelperLaunchStep step, const DWORD code) {
    child.terminate(code == 0 ? ERROR_ACCESS_DENIED : code);
    return HelperLaunchError{step, code};
  };
  bool assigned{};
  try {
    if (options.fail_assignment_for_test &&
        options.fail_assignment_for_test(child.process.get())) {
      SetLastError(ERROR_ACCESS_DENIED);
    } else {
      assigned = AssignProcessToJobObject(child.job.get(), child.process.get()) != FALSE;
    }
  } catch (...) {
    return fail(HelperLaunchStep::assign_job, ERROR_ACCESS_DENIED);
  }
  if (!assigned)
    return fail(HelperLaunchStep::assign_job, GetLastError());
  try {
    if (options.validate_suspended &&
        !options.validate_suspended(child.process.get(), child.process_id))
      return fail(HelperLaunchStep::validate, ERROR_ACCESS_DENIED);
  } catch (...) {
    return fail(HelperLaunchStep::validate, ERROR_ACCESS_DENIED);
  }
  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    return fail(HelperLaunchStep::resume, GetLastError());
  return child;
}

} // namespace layerc::platform
