// Confinement probe for the prototype fixture guard (Linux test harness only).
// Usage, inside a private fixture:
//   layere_proto_confinement OUTSIDE_FILE OUTSIDE_DIR [OUTSIDE_SHM_FILE]
// After Model::scan() confines the process, reading OUTSIDE_FILE and creating
// a file in OUTSIDE_DIR must both fail with EACCES, while the store stays
// writable, system files stay readable, and (with a populated fixture) the
// shared durable Save pipeline still commits. The caller verifies OUTSIDE_FILE
// was readable beforehand, so a denial is attributable to the confinement.
#include "proto_common.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>  // NOLINT(misc-include-cleaner): std::string below

namespace {
int failures = 0;

void expect_denied(const char *what, const int fd) {
  if (fd >= 0) {
    ::close(fd);
    std::printf("FAIL %s was allowed\n", what);
    ++failures;
  } else if (errno != EACCES) {
    std::printf("FAIL %s failed with errno %d, not EACCES\n", what, errno);
    ++failures;
  } else {
    std::printf("ok %s denied\n", what);
  }
}

void expect_denied_call(const char *what, const int result) {
  if (result == 0) {
    std::printf("FAIL %s was allowed\n", what);
    ++failures;
  } else if (errno != EACCES) {
    std::printf("FAIL %s failed with errno %d, not EACCES\n", what, errno);
    ++failures;
  } else {
    std::printf("ok %s denied\n", what);
  }
}

void expect_allowed(const char *what, const int fd) {
  if (fd < 0) {
    std::printf("FAIL %s was denied (errno %d)\n", what, errno);
    ++failures;
    return;
  }
  ::close(fd);
  std::printf("ok %s allowed\n", what);
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) return 64;
  layerc::prototype::Model model;
  if (!model.scan()) return 64;  // Applies the Landlock confinement.
  const std::filesystem::path store(std::getenv("LAYERC_STORE_ROOT"));
  expect_denied("reading a file outside the fixture", ::open(argv[1], O_RDONLY | O_CLOEXEC));
  expect_denied_call("truncating a file outside the fixture", ::truncate(argv[1], 0));
  if (argc == 4) {
    expect_denied("reading a shared-memory file outside the fixture",
                  ::open(argv[3], O_RDONLY | O_CLOEXEC));
    const auto shm = "/dev/shm/layerc-confinement-probe-" + std::to_string(::getpid());
    expect_denied("creating a shared-memory file outside the fixture",
                  ::open(shm.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
  }
  const auto outside = std::string(argv[2]) + "/confinement-probe";
  expect_denied("creating a file outside the fixture",
                ::open(outside.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
  const auto inside = store / "confinement-probe.tmp";
  expect_allowed("creating a file in the fixture store",
                 ::open(inside.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
  ::unlink(inside.c_str());
  expect_allowed("reading a system file", ::open("/etc/hostname", O_RDONLY | O_CLOEXEC));
  if (!model.rows().empty()) {
    if (!model.open(0)) {
      std::printf("FAIL opening the first fixture profile\n");
      ++failures;
    } else {
      model.document().profile().name += " confined";
      const auto outcome = model.save();
      std::printf("%s durable Save under confinement: %s\n", outcome == "Saved" ? "ok" : "FAIL",
                  outcome.c_str());
      if (outcome != "Saved") ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
