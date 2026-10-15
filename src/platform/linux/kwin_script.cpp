#include "kwin_script.hpp"

#include "dbus_shell_service.hpp"
#include "layerc/platform/profile_file_guard_linux.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace layerc::platform {
namespace {

[[nodiscard]] bool write_private_file(const std::filesystem::path &path,
                                      const std::string_view contents) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  struct stat status {};
  bool ok = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_uid == ::geteuid() && status.st_nlink == 1 &&
            ::fchmod(fd, 0600) == 0 && ::ftruncate(fd, 0) == 0;
  std::size_t written = 0;
  while (ok && written < contents.size()) {
    const auto result = ::write(fd, contents.data() + written, contents.size() - written);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) ok = false;
    else written += static_cast<std::size_t>(result);
  }
  return ::close(fd) == 0 && ok;
}

[[nodiscard]] bool missing_object(const sd_bus_error *error) noexcept {
  return error != nullptr && error->name != nullptr &&
         (std::strcmp(error->name, "org.freedesktop.DBus.Error.UnknownObject") == 0 ||
          std::strcmp(error->name, "org.freedesktop.DBus.Error.UnknownMethod") == 0 ||
          std::strcmp(error->name, "org.freedesktop.DBus.Error.UnknownInterface") == 0);
}

} // namespace

std::filesystem::path kwin_script_path() {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || *runtime != '/') return {};
  return std::filesystem::path{runtime} / "layerc" / "kwin" / "layerc-windows.js";
}

KwinScript::~KwinScript() { unload(); }

void KwinScript::set_wanted(const bool wanted) {
  if (wanted == wanted_) return;
  wanted_ = wanted;
  if (!wanted_) {
    unload();
    return;
  }
  if (bus_->raw() != nullptr) bus_->watch_name(kwin_bus_name);
  sync();
}

void KwinScript::sync() {
  if (!wanted_) return;
  if (bus_->raw() == nullptr) {
    state_ = State::failed;
    reason_ = bus_->reason().empty() ? "bus_unavailable" : bus_->reason();
    owner_.clear();
    return;
  }
  bus_->watch_name(kwin_bus_name);
  if (!bus_->owner_known(kwin_bus_name)) return;
  const auto owner = bus_->owner(kwin_bus_name);
  if (!owner) {
    ++generation_;
    owner_.clear();
    state_ = State::waiting_for_kwin;
    reason_ = "kwin_unavailable";
    return;
  }
  // A failed load is retried only when KWin itself is replaced.
  if (*owner == owner_ && state_ != State::waiting_for_kwin && state_ != State::idle) return;
  begin(*owner);
}

void KwinScript::fail(std::string reason) {
  state_ = State::failed;
  reason_ = std::move(reason);
}

void KwinScript::begin(const std::string &owner) {
  owner_ = owner;
  const auto generation = ++generation_;
  bus_->release(guard_); // cancel the replaced owner's pending load
  guard_ = make_bus_guard();
  state_ = State::loading;
  reason_.clear();
  const auto path = kwin_script_path();
  if (path.empty() ||
      !ensure_owned_directory(path.parent_path().parent_path(), OwnedDirectoryPolicy::require_private) ||
      !ensure_owned_directory(path.parent_path(), OwnedDirectoryPolicy::require_private) ||
      !write_private_file(path, kwin_window_script())) {
    fail("kwin_script_write_failed");
    return;
  }
  const auto current = [this, generation] { return generation == generation_; };
  // A stale plugin from an earlier daemon would keep reporting through an
  // old script; unloading a missing plugin is harmless.
  (void)bus_->call(
      owner_.c_str(), "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
      [](sd_bus_message *call) { return sd_bus_message_append(call, "s", kwin_script_plugin); },
      [](sd_bus_message *, const sd_bus_error *) {}, guard_);
  const auto text = path.string();
  const bool queued = bus_->call(
      owner_.c_str(), "/Scripting", "org.kde.kwin.Scripting", "loadScript",
      [&](sd_bus_message *call) {
        return sd_bus_message_append(call, "ss", text.c_str(), kwin_script_plugin);
      },
      [this, current, owner](sd_bus_message *reply, const sd_bus_error *error) {
        if (!current()) return;
        std::int32_t id{-1};
        if (error != nullptr || sd_bus_message_read(reply, "i", &id) < 0 || id < 0) {
          fail("kwin_script_failed");
          return;
        }
        run(owner, id, false);
      },
      guard_);
  load_requested_ = queued;
  if (!queued) fail("kwin_script_failed");
}

void KwinScript::run(const std::string &owner, const std::int32_t id, const bool legacy_path) {
  const auto generation = generation_;
  // KWin 6 exports /Scripting/Script<id>; KWin 5.27 exports /<id>.
  const auto object = legacy_path ? "/" + std::to_string(id)
                                  : "/Scripting/Script" + std::to_string(id);
  const bool queued = bus_->call(
      owner.c_str(), object.c_str(), "org.kde.kwin.Script", "run", {},
      [this, generation, owner, id, legacy_path](sd_bus_message *, const sd_bus_error *error) {
        if (generation != generation_) return;
        if (error == nullptr) {
          state_ = State::running;
          reason_.clear();
        } else if (!legacy_path && missing_object(error)) {
          run(owner, id, true);
        } else {
          fail("kwin_script_failed");
        }
      },
      guard_);
  if (!queued) fail("kwin_script_failed");
}

void KwinScript::unload() noexcept {
  ++generation_;
  if (std::exchange(load_requested_, false) && bus_->raw() != nullptr && !owner_.empty()) {
    try {
      (void)bus_->call(
          owner_.c_str(), "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
          [](sd_bus_message *call) { return sd_bus_message_append(call, "s", kwin_script_plugin); },
          [](sd_bus_message *, const sd_bus_error *) {}, guard_);
    } catch (...) {
    }
  }
  bus_->release(guard_);
  state_ = State::idle;
  reason_.clear();
  owner_.clear();
}

} // namespace layerc::platform
