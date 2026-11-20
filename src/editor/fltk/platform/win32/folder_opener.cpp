// Opens the profiles folder through the shell, as the Win32 editor does. The
// shell usually hands the folder to an Explorer process that is already
// running, so a successful request may have no child to reap; that outcome is
// reported without inventing a process exit code. Launched children are reaped
// without waiting. Nothing is elevated.
#include "platform/services.hpp"

#include <windows.h>
#include <shellapi.h>

namespace layerc::editor {

struct FolderOpener::Impl {
  HANDLE process{};
  bool accepted{}; // the shell completed the request without a process
};

FolderOpener::FolderOpener() = default;
FolderOpener::~FolderOpener() { stop(); }

bool FolderOpener::running() const noexcept {
  return impl_ && (impl_->process != nullptr || impl_->accepted);
}

bool FolderOpener::start(const std::filesystem::path &directory) {
  if (running()) return false;
  if (!impl_) impl_ = std::make_unique<Impl>();
  const std::wstring path = directory.wstring();
  SHELLEXECUTEINFOW request{};
  request.cbSize = sizeof(request);
  request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  request.lpVerb = L"open";
  request.lpFile = path.c_str();
  request.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&request)) return false;
  impl_->process = request.hProcess;
  impl_->accepted = request.hProcess == nullptr;
  return true;
}

std::optional<FolderOpenExit> FolderOpener::poll() {
  if (!running()) return std::nullopt;
  if (impl_->accepted) {
    impl_->accepted = false;
    return FolderOpenExit{std::nullopt, true};
  }
  if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) return std::nullopt;
  DWORD code{};
  const bool known = GetExitCodeProcess(impl_->process, &code) != FALSE;
  CloseHandle(impl_->process);
  impl_->process = nullptr;
  if (!known) return FolderOpenExit{};
  return FolderOpenExit{static_cast<int>(code)};
}

void FolderOpener::stop() noexcept {
  if (!impl_) return;
  // Closing the editor never signals the opened folder window.
  if (impl_->process != nullptr) CloseHandle(impl_->process);
  impl_->process = nullptr;
  impl_->accepted = false;
}

} // namespace layerc::editor
