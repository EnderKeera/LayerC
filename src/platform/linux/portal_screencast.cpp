#include "portal_screencast.hpp"

#include "layerc/core/utf8.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <limits>
#include <utility>

namespace layerc::platform {
namespace {

constexpr const char *request_interface = "org.freedesktop.portal.Request";
constexpr const char *session_interface = "org.freedesktop.portal.Session";

std::string next_token() {
  static std::atomic<std::uint64_t> sequence{};
  return "layerc_" + std::to_string(++sequence);
}

std::string session_path(const std::string &unique, const std::string &token) {
  auto path = portal_request_path(unique, token);
  const auto position = path.find("/request/");
  if (position != std::string::npos) path.replace(position, 9, "/session/");
  return path;
}

int option(sd_bus_message *message, const char *key, const char *type, const void *value) {
  int result = sd_bus_message_open_container(message, 'e', "sv");
  if (result >= 0) result = sd_bus_message_append(message, "s", key);
  if (result >= 0) result = sd_bus_message_open_container(message, 'v', type);
  if (result >= 0) result = sd_bus_message_append_basic(message, *type, value);
  if (result >= 0) result = sd_bus_message_close_container(message);
  if (result >= 0) result = sd_bus_message_close_container(message);
  return result;
}

bool read_u32(sd_bus_message *message, std::uint32_t &value) {
  return sd_bus_message_enter_container(message, 'v', "u") > 0 &&
         sd_bus_message_read(message, "u", &value) > 0 &&
         sd_bus_message_exit_container(message) >= 0;
}

bool read_pair(sd_bus_message *message, int &first, int &second) {
  std::int32_t a{}, b{};
  if (sd_bus_message_enter_container(message, 'v', "(ii)") <= 0 ||
      sd_bus_message_read(message, "(ii)", &a, &b) <= 0 ||
      sd_bus_message_exit_container(message) < 0)
    return false;
  first = a; second = b;
  return true;
}

bool read_string(sd_bus_message *message, const char *type, std::string &value) {
  const char *text{};
  if (sd_bus_message_enter_container(message, 'v', type) <= 0 ||
      sd_bus_message_read(message, type, &text) <= 0 || !text ||
      sd_bus_message_exit_container(message) < 0)
    return false;
  const std::string_view view{text};
  if (view.size() > screencast_token_max_bytes || !core::is_valid_utf8(view)) return false;
  value = view;
  return true;
}

} // namespace

PortalRemote::~PortalRemote() { if (fd >= 0) ::close(fd); }
PortalRemote::PortalRemote(PortalRemote &&other) noexcept { *this = std::move(other); }
PortalRemote &PortalRemote::operator=(PortalRemote &&other) noexcept {
  if (&other == this) return *this;
  if (fd >= 0) ::close(fd);
  fd = std::exchange(other.fd, -1);
  node = other.node; monitor = other.monitor; geometry = other.geometry;
  return *this;
}

PortalScreencast::~PortalScreencast() { stop(); }
bool PortalScreencast::take_changed() noexcept { return std::exchange(changed_, false); }
std::optional<PortalRemote> PortalScreencast::take_remote() { return std::exchange(remote_, std::nullopt); }

bool PortalScreencast::start(std::string profile_id) {
  if (wanted_) return profile_ == profile_id && reason_ != "portal_denied";
  if (!bus_->connected() || profile_id.empty() || profile_id.size() > screencast_token_max_bytes ||
      !core::is_valid_utf8(profile_id)) {
    reason_ = "portal_unavailable";
    changed_ = true;
    return false;
  }
  wanted_ = true;
  ready_ = false;
  profile_ = std::move(profile_id);
  owner_.clear();
  guard_ = make_bus_guard();
  ++generation_;
  properties_ = 0;
  version_ = source_types_ = cursor_modes_ = 0;
  reason_ = "portal_pending";
  changed_ = true;
  bus_->watch_name(portal_bus_name);
  const auto token = next_token();
  session_path_ = session_path(bus_->unique_name(), token);
  probe();
  return true;
}

void PortalScreencast::release_handles() noexcept {
  try {
    if (bus_->connected() && !owner_.empty()) {
      const auto ignore = [](sd_bus_message *, const sd_bus_error *) {};
      if (!request_path_.empty())
        (void)bus_->call(owner_.c_str(), request_path_.c_str(), request_interface,
                        "Close", {}, ignore, guard_);
      if (!session_path_.empty())
        (void)bus_->call(owner_.c_str(), session_path_.c_str(), session_interface,
                        "Close", {}, ignore, guard_);
    }
    // A signal handler can cancel itself; actual slot release follows dispatch.
    bus_->defer_release(std::move(response_match_));
    bus_->defer_release(std::move(closed_match_));
  } catch (...) {}
  request_path_.clear();
  session_path_.clear();
  remote_.reset();
  stream_ = PortalRemote{};
}

void PortalScreencast::stop() noexcept {
  release_handles();
  ++generation_;
  bus_->release(guard_);
  wanted_ = ready_ = false;
  profile_.clear();
  owner_.clear();
  restore_token_.clear();
  reason_.clear();
  changed_ = true;
}

void PortalScreencast::fail(std::string reason) {
  if (!wanted_) return;
  release_handles();
  ++generation_;
  bus_->release(guard_);
  ready_ = false;
  reason_ = std::move(reason);
  changed_ = true;
}

void PortalScreencast::sync() {
  if (!wanted_ || !guard_) return;
  if (!bus_->connected()) {
    fail("portal_connection_lost");
    return;
  }
  const auto current = bus_->owner(portal_bus_name);
  if (!owner_.empty() && bus_->owner_known(portal_bus_name) &&
      (!current || *current != owner_))
    fail("portal_owner_changed");
}

bool PortalScreencast::accept_reply(sd_bus_message *reply, const sd_bus_error *error) {
  if (error) { fail("portal_unavailable"); return false; }
  const char *sender = sd_bus_message_get_sender(reply);
  if (!sender || *sender != ':') { fail("portal_invalid_response"); return false; }
  const auto current = bus_->owner(portal_bus_name);
  if ((!owner_.empty() && owner_ != sender) ||
      (bus_->owner_known(portal_bus_name) && (!current || *current != sender))) {
    fail("portal_owner_changed");
    return false;
  }
  owner_ = sender;
  return true;
}

void PortalScreencast::probe() {
  const auto generation = generation_;
  constexpr std::array names{"version", "AvailableSourceTypes", "AvailableCursorModes"};
  for (unsigned index = 0; index < names.size(); ++index) {
    // The well-known name deliberately permits standard portal activation.
    // Every later request targets the authenticated unique reply sender.
    if (!bus_->get_property(portal_bus_name, portal_path, portal_screencast_interface, names[index],
        [this, generation, index](sd_bus_message *reply, const sd_bus_error *error) {
          if (generation != generation_ || !accept_reply(reply, error)) return;
          std::uint32_t value{};
          if (!read_u32(reply, value)) { fail("portal_invalid_response"); return; }
          if (index == 0) version_ = value;
          else if (index == 1) source_types_ = value;
          else cursor_modes_ = value;
          if (++properties_ != 3) return;
          if (version_ < 4) { fail("portal_version"); return; }
          if ((source_types_ & 3U) == 0) { fail("portal_sources_unavailable"); return; }
          request(Step::create);
        }, guard_)) {
      fail("portal_unavailable");
      return;
    }
  }
}

void PortalScreencast::request(const Step step) {
  const auto generation = generation_;
  const auto token = next_token();
  request_path_ = portal_request_path(bus_->unique_name(), token);
  const auto expected = request_path_;
  bus_->defer_release(std::move(response_match_));
  response_match_ = bus_->add_match(
      "type='signal',sender='" + owner_ + "',path='" + expected +
      "',interface='org.freedesktop.portal.Request',member='Response'",
      [this, generation, expected, step](sd_bus_message *message) {
        if (generation != generation_ || request_path_ != expected) return;
        sync();
        if (generation == generation_) response(step, message);
      }, guard_);
  if (!response_match_) { fail("portal_unavailable"); return; }

  // Tokens are single-use: consume the old token durably before SelectSources.
  restore_token_.clear();
  if (step == Step::select) {
    if (const auto previous = tokens_->token(profile_)) {
      restore_token_ = *previous;
      if (!tokens_->replace(profile_, "")) { fail("portal_token_write_failed"); return; }
    }
  }
  const char *member = step == Step::create ? "CreateSession"
                      : step == Step::select ? "SelectSources" : "Start";
  if (!bus_->call(owner_.c_str(), portal_path, portal_screencast_interface, member,
      [this, step, token](sd_bus_message *message) {
        int result = 0;
        if (step != Step::create)
          result = sd_bus_message_append(message, step == Step::start ? "os" : "o",
                                         session_path_.c_str(), "");
        if (result >= 0) result = sd_bus_message_open_container(message, 'a', "{sv}");
        if (result >= 0) result = option(message, "handle_token", "s", token.c_str());
        if (step == Step::create && result >= 0) {
          const auto session_token = session_path_.substr(session_path_.find_last_of('/') + 1);
          result = option(message, "session_handle_token", "s", session_token.c_str());
        }
        if (step == Step::select) {
          const std::uint32_t types = (source_types_ & 2U) ? 2U : 1U;
          const std::uint32_t persistence = 2U, hidden = 1U;
          const int multiple = 0;
          if (result >= 0) result = option(message, "types", "u", &types);
          if (result >= 0) result = option(message, "multiple", "b", &multiple);
          if (result >= 0) result = option(message, "persist_mode", "u", &persistence);
          if (result >= 0 && (cursor_modes_ & hidden)) result = option(message, "cursor_mode", "u", &hidden);
          if (result >= 0 && !restore_token_.empty())
            result = option(message, "restore_token", "s", restore_token_.c_str());
        }
        if (result >= 0) result = sd_bus_message_close_container(message);
        return result;
      }, [this, generation, expected](sd_bus_message *reply, const sd_bus_error *error) {
        if (generation != generation_ || request_path_ != expected) return;
        if (!accept_reply(reply, error)) return;
        const char *path{};
        if (sd_bus_message_read(reply, "o", &path) <= 0 || !path || expected != path)
          fail("portal_invalid_response");
      }, guard_))
    fail("portal_unavailable");
}

void PortalScreencast::response(const Step step, sd_bus_message *message) {
  std::uint32_t code{};
  if (sd_bus_message_read(message, "u", &code) <= 0) { fail("portal_invalid_response"); return; }
  if (code != 0) { fail(code == 1 ? "portal_denied" : "portal_failed"); return; }
  if (!parse_results(step, message)) { fail("portal_invalid_response"); return; }
  request_path_.clear();
  bus_->defer_release(std::move(response_match_));
  if (step == Step::create) {
    closed_match_ = bus_->add_match(
        "type='signal',sender='" + owner_ + "',path='" + session_path_ +
        "',interface='org.freedesktop.portal.Session',member='Closed'",
        [this](sd_bus_message *) { fail("portal_session_closed"); }, guard_);
    if (!closed_match_) { fail("portal_unavailable"); return; }
    request(Step::select);
  } else if (step == Step::select) {
    request(Step::start);
  } else {
    if (!restore_token_.empty() && !tokens_->replace(profile_, restore_token_)) {
      fail("portal_token_write_failed");
      return;
    }
    open_remote();
  }
}

bool PortalScreencast::parse_streams(sd_bus_message *message) {
  if (sd_bus_message_enter_container(message, 'v', "a(ua{sv})") <= 0 ||
      sd_bus_message_enter_container(message, 'a', "(ua{sv})") <= 0 ||
      sd_bus_message_enter_container(message, 'r', "ua{sv}") <= 0 ||
      sd_bus_message_read(message, "u", &stream_.node) <= 0 ||
      stream_.node == std::numeric_limits<std::uint32_t>::max() ||
      sd_bus_message_enter_container(message, 'a', "{sv}") <= 0)
    return false;
  LogicalRect geometry{};
  bool position{}, size{};
  std::uint32_t type = (source_types_ & 2U) ? 2U : 1U;
  int entered{};
  unsigned count{};
  while ((entered = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
    if (++count > 64) return false;
    const char *key{};
    if (sd_bus_message_read(message, "s", &key) <= 0 || !key) return false;
    const std::string_view name{key};
    if (name == "position") {
      if (position || !read_pair(message, geometry.x, geometry.y)) return false;
      position = true;
    } else if (name == "size") {
      if (size || !read_pair(message, geometry.width, geometry.height) ||
          geometry.width <= 0 || geometry.height <= 0) return false;
      size = true;
    } else if (name == "source_type") {
      if (!read_u32(message, type) || (type != 1 && type != 2)) return false;
    } else if (sd_bus_message_skip(message, "v") < 0) return false;
    if (sd_bus_message_exit_container(message) < 0) return false;
  }
  if (entered < 0 || sd_bus_message_exit_container(message) < 0 ||
      sd_bus_message_exit_container(message) < 0 ||
      sd_bus_message_enter_container(message, 'r', "ua{sv}") != 0 ||
      sd_bus_message_exit_container(message) < 0 ||
      sd_bus_message_exit_container(message) < 0)
    return false; // multiple=false: a second stream must not be silently guessed.
  stream_.monitor = type == 1;
  if (position && size) stream_.geometry = geometry;
  return true;
}

bool PortalScreencast::parse_results(const Step step, sd_bus_message *message) {
  if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) return false;
  bool found{};
  bool token_seen{};
  int entered{};
  unsigned count{};
  while ((entered = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
    if (++count > 64) return false;
    const char *key{};
    if (sd_bus_message_read(message, "s", &key) <= 0 || !key) return false;
    const std::string_view name{key};
    if (step == Step::create && name == "session_handle") {
      std::string value;
      if (found || !read_string(message, "o", value) || value != session_path_) return false;
      found = true;
    } else if (step == Step::start && name == "streams") {
      if (found || !parse_streams(message)) return false;
      found = true;
    } else if (step == Step::start && name == "restore_token") {
      if (token_seen || !read_string(message, "s", restore_token_)) return false;
      token_seen = true;
    } else if (sd_bus_message_skip(message, "v") < 0) return false;
    if (sd_bus_message_exit_container(message) < 0) return false;
  }
  return entered == 0 && sd_bus_message_exit_container(message) >= 0 &&
         (step == Step::select || found);
}

void PortalScreencast::open_remote() {
  const auto generation = generation_;
  if (!bus_->call(owner_.c_str(), portal_path, portal_screencast_interface, "OpenPipeWireRemote",
      [this](sd_bus_message *message) {
        int result = sd_bus_message_append(message, "o", session_path_.c_str());
        if (result >= 0) result = sd_bus_message_open_container(message, 'a', "{sv}");
        if (result >= 0) result = sd_bus_message_close_container(message);
        return result;
      }, [this, generation](sd_bus_message *reply, const sd_bus_error *error) {
        if (generation != generation_ || !accept_reply(reply, error)) return;
        int fd{-1};
        if (sd_bus_message_read(reply, "h", &fd) <= 0 || fd < 0) {
          fail("portal_invalid_response");
          return;
        }
        stream_.fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (stream_.fd < 0) { fail("portal_remote_failed"); return; }
        remote_ = std::move(stream_);
        ready_ = true;
        reason_.clear();
        changed_ = true;
      }, guard_))
    fail("portal_unavailable");
}

} // namespace layerc::platform
