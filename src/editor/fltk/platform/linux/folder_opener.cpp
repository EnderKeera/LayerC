#include "platform/services.hpp"

#include <array>
#include <signal.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace layerc::editor {

struct FolderOpener::Impl {
  pid_t child{};
};

FolderOpener::FolderOpener() = default;
FolderOpener::~FolderOpener() { stop(); }

bool FolderOpener::running() const noexcept {
  return impl_ && impl_->child > 0;
}

bool FolderOpener::start(const std::filesystem::path &directory) {
  if (running()) return false;
  if (!impl_) impl_ = std::make_unique<Impl>();
  const std::string path = directory.string();
  posix_spawnattr_t attributes;
  if (posix_spawnattr_init(&attributes) != 0) return false;
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  int result = posix_spawnattr_setsigmask(&attributes, &none);
  if (result == 0) result = posix_spawnattr_setsigdefault(&attributes, &all);
  if (result == 0)
    result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID |
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  if (result != 0) {
    posix_spawnattr_destroy(&attributes);
    return false;
  }
  posix_spawn_file_actions_t files;
  if (posix_spawn_file_actions_init(&files) != 0) {
    posix_spawnattr_destroy(&attributes);
    return false;
  }
  result = posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (result == 0)
    result = posix_spawn_file_actions_addclosefrom_np(&files, STDERR_FILENO + 1);
  char program[] = "xdg-open";
  std::array<char *, 3> argv{program, const_cast<char *>(path.c_str()), nullptr};
  pid_t child{};
  if (result == 0)
    result = posix_spawnp(&child, program, &files, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&files);
  posix_spawnattr_destroy(&attributes);
  if (result != 0) return false;
  impl_->child = child;
  return true;
}

std::optional<FolderOpenExit> FolderOpener::poll() {
  if (!running()) return std::nullopt;
  int status{};
  const auto done = ::waitpid(impl_->child, &status, WNOHANG);
  if (done == 0) return std::nullopt;
  impl_->child = 0;
  if (done < 0 || !WIFEXITED(status)) return FolderOpenExit{};
  return FolderOpenExit{WEXITSTATUS(status)};
}

void FolderOpener::stop() noexcept {
  if (running()) {
    (void)::waitpid(impl_->child, nullptr, WNOHANG);
    impl_->child = 0;
  }
}

} // namespace layerc::editor
