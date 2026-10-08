// Linux-only disposable fixture enforcement for standalone prototypes.
//
// Threat model: this guard prevents accidental access to production data by a
// prototype launched outside its harness, with a misconfigured environment, or
// against a fixture containing link escapes. It does not claim protection
// against a concurrent adversary running as the same user, which already has
// direct access to that user's files.
#pragma once
#include "layerc/core/hash.hpp"
#include "layerc/platform/profile_store.hpp"
#include <fcntl.h>
#include <link.h>
#include <linux/landlock.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::prototype {
struct FixtureError { std::string reason; };
using FixtureResult = std::variant<std::filesystem::path, FixtureError>;

[[nodiscard]] inline bool inside(const std::filesystem::path &path,
                                 const std::filesystem::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || *p != *r) return false;
  return p != path.end();
}

namespace detail {
// Validates the fixture by pathname and records the root's identity, which
// confined_fixture_store() then pins with a descriptor.
[[nodiscard]] inline FixtureResult check_fixture(struct stat &root_status) {
  namespace fs = std::filesystem;
  const auto *raw_root = std::getenv("LAYERC_PROTO_FIXTURE_ROOT");
  const auto *token = std::getenv("LAYERC_PROTO_FIXTURE_TOKEN");
  const auto fail = [](const char *reason) -> FixtureResult { return FixtureError{reason}; };
  if (!raw_root || !token || !layerc::core::is_lowercase_sha256(token))
    return fail("private fixture marker is required; launch through the prototype harness");
  const fs::path root(raw_root);
  std::error_code error;
  if (!root.is_absolute() || root != fs::canonical(root, error) || error ||
      !root.filename().string().starts_with("layerc-editor-prototype."))
    return fail("fixture root must be a canonical disposable prototype directory");
  if (::lstat(root.c_str(), &root_status) != 0 || !S_ISDIR(root_status.st_mode) ||
      root_status.st_uid != ::geteuid() || (root_status.st_mode & 0077) != 0)
    return fail("fixture root must be private and owned by the current user");
  struct stat status{};
  const auto marker = root / ".layerc-prototype-fixture";
  if (::lstat(marker.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0 || status.st_nlink != 1)
    return fail("private fixture marker is absent or unsafe");
  std::ifstream input(marker);
  std::string actual_token;
  std::getline(input, actual_token);
  if (!input || actual_token != token || input.peek() != std::char_traits<char>::eof())
    return fail("private fixture marker does not match the harness token");
  for (const auto *name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                           "XDG_STATE_HOME", "XDG_RUNTIME_DIR"}) {
    const auto *value = std::getenv(name);
    if (!value || !fs::path(value).is_absolute()) return fail("all HOME/XDG roots must be private");
    const auto resolved = fs::weakly_canonical(value, error);
    if (error || !inside(resolved, root)) return fail("HOME/XDG root escapes the disposable fixture");
  }
  // Reject link escapes before any bundle/sidecar is read, not just on Save.
  // A hard-linked regular file could alias data outside the fixture.
  std::size_t entries = 0;
  fs::recursive_directory_iterator it(root, error), end;
  while (!error && it != end) {
    if (++entries > 10000 || it->is_symlink(error))
      return fail("fixture contains a symlink or exceeds the entry budget");
    if (!error && it->is_regular_file(error) && !error && it->hard_link_count(error) != 1)
      return fail("fixture contains a hard-linked file");
    if (error) break;
    it.increment(error);
  }
  if (error) return fail("cannot inspect the complete disposable fixture");
  const auto store = fs::canonical(layerc::platform::profile_directory(), error);
  if (error || !inside(store, root)) return fail("resolved profile store escapes the fixture");
  const auto *override_root = std::getenv("LAYERC_STORE_ROOT");
  if (!override_root || fs::canonical(override_root, error) != store || error)
    return fail("explicit fixture store must match the platform-resolved store");
  return store;
}

class Descriptor {
public:
  explicit Descriptor(const int fd) noexcept : fd_(fd) {}
  ~Descriptor() { if (fd_ >= 0) ::close(fd_); }
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

// Locations the confinement must never grant. The real home is a container:
// no grant may include it. Its configuration/data/state trees and runtime
// directory, plus the original XDG values the harness recorded in
// LAYERC_PROTO_PRODUCTION_ROOTS before replacing the environment, are data
// roots: no grant may overlap them. That variable holds colon-separated
// lowercase-hex encodings of absolute paths, so any pathname is unambiguous;
// malformed metadata fails closed.
struct ProductionRoots {
  std::filesystem::path home;
  std::vector<std::filesystem::path> data;
  bool malformed{};
};

[[nodiscard]] inline std::optional<std::string> decode_hex(const std::string_view hex) {
  if (hex.empty() || hex.size() % 2 != 0) return std::nullopt;
  const auto nibble = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string text;
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = nibble(hex[i]);
    const int low = nibble(hex[i + 1]);
    if (high < 0 || low < 0 || (high == 0 && low == 0)) return std::nullopt;
    text.push_back(static_cast<char>(high * 16 + low));
  }
  return text;
}

[[nodiscard]] inline ProductionRoots production_roots() {
  namespace fs = std::filesystem;
  ProductionRoots roots;
  std::error_code error;
  if (const auto *account = ::getpwuid(::geteuid()); account && account->pw_dir) {
    roots.home = fs::weakly_canonical(account->pw_dir, error);
    for (const auto *tree : {".config", ".local", ".cache"}) roots.data.push_back(roots.home / tree);
  }
  roots.data.push_back(fs::path("/run/user") / std::to_string(::geteuid()));
  const std::string_view declared =
      std::getenv("LAYERC_PROTO_PRODUCTION_ROOTS") ? std::getenv("LAYERC_PROTO_PRODUCTION_ROOTS") : "";
  for (std::size_t start = 0; !declared.empty() && start <= declared.size();) {
    const auto end = std::min(declared.find(':', start), declared.size());
    const auto decoded = decode_hex(declared.substr(start, end - start));
    start = end + 1;
    if (!decoded || !fs::path(*decoded).is_absolute()) {
      roots.malformed = true;
      continue;
    }
    roots.data.push_back(fs::weakly_canonical(fs::path(*decoded), error));
    if (error) roots.malformed = true;
  }
  return roots;
}

[[nodiscard]] inline bool overlaps_production(const std::filesystem::path &grant,
                                              const ProductionRoots &roots) {
  if (!roots.home.empty() && (grant == roots.home || inside(roots.home, grant))) return true;
  return std::ranges::any_of(roots.data, [&](const auto &data) {
    return grant == data || inside(grant, data) || inside(data, grant);
  });
}

// Canonical directories of the shared objects already loaded: the actual
// toolkit/bridge closure, whose lazily opened modules live beside it.
[[nodiscard]] inline std::vector<std::filesystem::path> loaded_library_directories() {
  std::vector<std::filesystem::path> directories;
  dl_iterate_phdr(
      [](dl_phdr_info *info, std::size_t, void *data) {
        auto &found = *static_cast<std::vector<std::filesystem::path> *>(data);
        if (info->dlpi_name == nullptr || info->dlpi_name[0] != '/') return 0;
        std::error_code error;
        const auto library = std::filesystem::canonical(info->dlpi_name, error);
        if (!error) found.push_back(library.parent_path());
        return 0;
      },
      &directories);
  std::ranges::sort(directories);
  const auto duplicates = std::ranges::unique(directories);
  directories.erase(duplicates.begin(), duplicates.end());
  return directories;
}
} // namespace detail

[[nodiscard]] inline FixtureResult checked_fixture_store() {
  struct stat root_status{};
  return detail::check_fixture(root_status);
}

// Checks the fixture, pins its root by descriptor and confines this process
// (and every thread/child it creates later) with Landlock ABI >= 3, so writes,
// truncation and removal are all handled:
//  - all access beneath the pinned fixture;
//  - read/execute beneath system trees and the loaded libraries' directories;
//  - only the individual device nodes the toolkits use (/dev/null read/write,
//    /dev/zero, /dev/random and /dev/urandom read);
//  - directory listing (names only) everywhere, for the durable store's
//    ancestor fsync barrier.
// No grant may include the real home or overlap production data roots.
// Call while single-threaded, before toolkit initialization; fails closed.
[[nodiscard]] inline FixtureResult confined_fixture_store() {
  namespace fs = std::filesystem;
  struct stat checked{};
  auto result = detail::check_fixture(checked);
  if (std::holds_alternative<FixtureError>(result)) return result;
  const auto fail = [](std::string reason) -> FixtureResult { return FixtureError{std::move(reason)}; };
  const auto abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 3) return fail("Landlock ABI 3 or later (with truncation control) is required");
  constexpr std::uint64_t read_access =
      LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
  std::uint64_t all = read_access | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                      LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                      LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                      LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                      LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM |
                      LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
  if (abi >= 5) all |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
  landlock_ruleset_attr attributes{};
  attributes.handled_access_fs = all;
  const detail::Descriptor ruleset(static_cast<int>(
      ::syscall(SYS_landlock_create_ruleset, &attributes, sizeof attributes, 0)));
  if (ruleset.get() < 0) return fail("cannot create the Landlock ruleset");
  const auto allow = [&](const int fd, const std::uint64_t access) {
    landlock_path_beneath_attr beneath{};
    beneath.allowed_access = access;
    beneath.parent_fd = fd;
    return ::syscall(SYS_landlock_add_rule, ruleset.get(), LANDLOCK_RULE_PATH_BENEATH, &beneath, 0) == 0;
  };
  const auto production = detail::production_roots();
  if (production.malformed) return fail("recorded production roots are malformed");
  const auto *root = std::getenv("LAYERC_PROTO_FIXTURE_ROOT");
  if (detail::overlaps_production(root, production))
    return fail("fixture root overlaps a production location");
  const detail::Descriptor fixture(::open(root, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat pinned{};
  if (fixture.get() < 0 || ::fstat(fixture.get(), &pinned) != 0 ||
      pinned.st_dev != checked.st_dev || pinned.st_ino != checked.st_ino)
    return fail("fixture root changed while it was being pinned");
  if (!allow(fixture.get(), all)) return fail("cannot grant the pinned fixture");
  // Absent trees and device nodes are skipped; a grant never follows a
  // symlinked device node out of /dev.
  const auto grant = [&](const fs::path &path, const std::uint64_t access, const bool directory) {
    const detail::Descriptor fd(
        ::open(path.c_str(), O_PATH | O_CLOEXEC | (directory ? O_DIRECTORY : O_NOFOLLOW)));
    return fd.get() < 0 || allow(fd.get(), access);
  };
  // The durable store's write-ahead barrier fsyncs every ancestor directory
  // up to "/", which needs directory opens. Landlock grants are subtree-wide,
  // so directory listing (names only) is allowed everywhere; file contents
  // outside the granted trees stay unreadable and unwritable.
  if (!grant("/", LANDLOCK_ACCESS_FS_READ_DIR, true)) return fail("cannot grant directory barriers");
  constexpr std::uint64_t device_read = LANDLOCK_ACCESS_FS_READ_FILE;
  if (!grant("/dev/null", device_read | LANDLOCK_ACCESS_FS_WRITE_FILE, false) ||
      !grant("/dev/zero", device_read, false) || !grant("/dev/random", device_read, false) ||
      !grant("/dev/urandom", device_read, false))
    return fail("cannot grant device nodes");
  std::vector<fs::path> trees{"/usr", "/lib", "/lib64", "/bin", "/etc", "/proc", "/sys", "/var/cache"};
  for (const auto &library : detail::loaded_library_directories())
    if (std::ranges::none_of(trees, [&](const auto &tree) { return library == tree || inside(library, tree); }))
      trees.push_back(library);
  for (const auto &tree : trees) {
    std::error_code error;
    const auto resolved = fs::weakly_canonical(tree, error);
    if (error || detail::overlaps_production(resolved, production))
      return fail("read grant " + tree.string() + " would expose production data");
    if (!grant(resolved, read_access, true)) return fail("cannot grant " + tree.string());
  }
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      ::syscall(SYS_landlock_restrict_self, ruleset.get(), 0) != 0)
    return fail("cannot enforce Landlock confinement");
  // Confirm the confined view is still the same checked fixture and store.
  struct stat after{};
  auto confirmed = detail::check_fixture(after);
  if (std::holds_alternative<FixtureError>(confirmed)) return confirmed;
  if (after.st_dev != pinned.st_dev || after.st_ino != pinned.st_ino ||
      std::get<fs::path>(confirmed) != std::get<fs::path>(result))
    return fail("fixture changed while confinement was applied");
  return confirmed;
}

// Opens an existing fixture document for reading only if the opened inode is
// a single-link regular file that the pathname still names. Callers re-check
// identity_matches() after loading by pathname, so a pre-existing or swapped
// hard-link alias is rejected rather than read.
class ValidatedDocument {
public:
  explicit ValidatedDocument(const std::filesystem::path &path)
      : path_(path), fd_(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) {
    struct stat named{};
    valid_ = fd_.get() >= 0 && ::fstat(fd_.get(), &opened_) == 0 && S_ISREG(opened_.st_mode) &&
             opened_.st_nlink == 1 && ::lstat(path.c_str(), &named) == 0 &&
             named.st_dev == opened_.st_dev && named.st_ino == opened_.st_ino;
  }
  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] bool identity_matches() const {
    struct stat now{};
    struct stat named{};
    return valid_ && ::fstat(fd_.get(), &now) == 0 && now.st_nlink == 1 &&
           ::lstat(path_.c_str(), &named) == 0 && named.st_dev == opened_.st_dev &&
           named.st_ino == opened_.st_ino;
  }

private:
  std::filesystem::path path_;
  detail::Descriptor fd_;
  struct stat opened_{};
  bool valid_{};
};
} // namespace layerc::prototype
