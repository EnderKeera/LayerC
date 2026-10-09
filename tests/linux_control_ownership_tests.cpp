#include "control_channel.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <string>

namespace {
bool fail_bind{};
int tracked_fd{-1};
int replacement_fd{-1};
int closes{};
}

extern "C" int __real_bind(int, const sockaddr *, socklen_t);
extern "C" int __real_close(int);

// Link-time fault simulation in this test executable only. Production
// binaries link libc directly; no runtime interposition or process injection.
extern "C" int __wrap_bind(int fd, const sockaddr *address, socklen_t size) {
  if (!fail_bind) return __real_bind(fd, address, size);
  tracked_fd = fd;
  errno = EADDRINUSE;
  return -1;
}

extern "C" int __wrap_close(int fd) {
  const int result = __real_close(fd);
  if (fail_bind && fd == tracked_fd) {
    ++closes;
    if (closes == 1) {
      int pipe[2]{-1, -1};
      if (::pipe2(pipe, O_CLOEXEC) == 0) {
        replacement_fd = pipe[0];
        (void)__real_close(pipe[1]);
        if (replacement_fd != fd && ::dup2(replacement_fd, fd) >= 0) {
          (void)__real_close(replacement_fd);
          replacement_fd = fd;
        }
      }
    }
    errno = EBADF; // Cleanup must not erase the original bind failure.
  }
  return result;
}

namespace {
void bind_failure_closes_once() {
  using layerc::test::expect;
  layerc::test::TempDirectory temp("layerc-control-bind-failure");
  const auto runtime_path = temp.path() / "runtime";
  std::filesystem::create_directories(runtime_path);
  expect(::chmod(runtime_path.c_str(), 0700) == 0, "private fixture runtime");
  const layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR", runtime_path);
  std::string error, warning;
  fail_bind = true;
  const auto listener = layerc::platform::ControlListener::open(error, warning);
  fail_bind = false;
  expect(!listener && closes == 1,
         "failed bind releases its owned descriptor exactly once");
  expect(replacement_fd == tracked_fd && ::fcntl(replacement_fd, F_GETFD) >= 0,
         "RAII cleanup never closes a descriptor reused by another owner");
  expect(error == "the control socket appeared during startup",
         "cleanup preserves the original bind failure");
  if (replacement_fd >= 0) (void)::close(replacement_fd);
}
}

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(bind_failure_closes_once))
