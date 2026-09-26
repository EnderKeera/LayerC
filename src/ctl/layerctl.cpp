// layerctl — LayerC control CLI (Linux). Control commands talk to the
// running daemon over the Unix socket; import/export commands operate on the
// profile store directly (offline-capable) and notify a running daemon
// afterwards. Payload exports write only payload bytes to stdout; every
// diagnostic goes to stderr.
#include "control_channel.hpp"

#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store_linux.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace {

namespace bundle = layerc::bundle;
namespace platform = layerc::platform;

[[noreturn]] int usage() {
  std::fputs(
      "usage: layerctl status [--json]\n"
      "       layerctl reload\n"
      "       layerctl stop\n"
      "       layerctl profile import-json\n"
      "       layerctl profile export-json <id>\n"
      "       layerctl bundle import [--keep-existing] [--dry-run]\n"
      "       layerctl bundle export <id> [<id>...]\n",
      stderr);
  std::exit(2);
}

void require_args(const int argc, const int expected) {
  if (argc != expected) usage();
}

void require_min_args(const int argc, const int minimum) {
  if (argc < minimum) usage();
}

// Progress verb for `bundle import`: dry runs describe the plan, otherwise
// colliding profiles are replaced and new ones are imported.
const char *import_verb(const bool dry_run, const bool collides) {
  if (dry_run) return collides ? "would replace" : "would import";
  if (collides) return "replacing";
  return "importing";
}

// Shared store scan: prints warnings, fails visibly on store errors.
[[nodiscard]] std::optional<platform::ProfileDirectoryScan> scan_or_die(
    const platform::ProfileStoreRoots &roots) {
  auto scan = platform::scan_profile_directory(roots.config_profiles);
  if (!scan.warning.empty())
    std::fprintf(stderr, "layerctl: %s\n", scan.warning.c_str());
  if (!scan.error.empty()) {
    std::fprintf(stderr, "layerctl: profile directory is invalid (%s)\n",
                 scan.error.c_str());
    return std::nullopt;
  }
  return scan;
}

// Nullopt on I/O error or over-limit (each prints its own diagnostic); an
// empty string is a clean EOF with no bytes, which callers report as empty
// input — never conflated with a failed read.
[[nodiscard]] std::optional<std::string> read_stream(int fd,
                                                     std::size_t limit) {
  std::string result;
  char chunk[65536];
  for (;;) {
    const ssize_t received = ::read(fd, chunk, sizeof(chunk));
    if (received == 0) return result;
    if (received < 0) {
      if (errno == EINTR) continue;
      std::fputs("layerctl: input could not be read\n", stderr);
      return std::nullopt;
    }
    if (result.size() + static_cast<std::size_t>(received) > limit) {
      std::fputs("layerctl: input exceeds its size limit\n", stderr);
      return std::nullopt;
    }
    result.append(chunk, static_cast<std::size_t>(received));
  }
}

// Staged bundle bytes through the platform durable-file primitive (O_EXCL,
// owner-only, fsynced, symlink-safe) instead of a hand-rolled mkstemp
// wrapper. The stage file is removed on scope exit.
class StagedFile {
public:
  [[nodiscard]] static std::optional<StagedFile> stage_bytes(
      const std::string_view bytes) {
    auto reserved = reserve();
    if (!reserved.has_value()) return std::nullopt;
    if (!platform::create_durable_file(reserved->path_,
                                       layerc::core::text_bytes(bytes)))
      return std::nullopt;
    return reserved;
  }

  [[nodiscard]] static std::optional<StagedFile> reserve() {
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) return std::nullopt;
    return StagedFile{
        platform::unique_durable_stage_path(directory / "layerctl-pack")};
  }

  StagedFile(const StagedFile &) = delete;
  StagedFile &operator=(const StagedFile &) = delete;
  StagedFile(StagedFile &&other) noexcept
      : path_(std::move(other.path_)) {}
  StagedFile &operator=(StagedFile &&) = delete;

  ~StagedFile() {
    if (!path_.empty()) (void)platform::remove_durable_file(path_);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  explicit StagedFile(std::filesystem::path path)
      : path_(std::move(path)) {}

  std::filesystem::path path_;
};

[[nodiscard]] std::optional<std::string> read_file(
    const std::filesystem::path &path) {
  // Symlink-safe, owner-gated, bounded read through the platform primitive
  // instead of a second chunked-read loop. The export stage is always below
  // the bundle ceiling, so max_pack_bytes bounds it.
  auto bytes = platform::read_durable_file(path, bundle::max_pack_bytes);
  if (!bytes.has_value()) return std::nullopt;
  return layerc::core::as_text(*bytes);
}

// Human-readable `status`: sessions and capability availability from the
// payload; `--json` keeps the full, byte-stable document.
void print_status_summary(const std::string_view response) {
  namespace json = layerc::core::json;
  const auto parsed = json::parse(response);
  const auto *root = std::get_if<json::Value>(&parsed);
  const auto *payload = root != nullptr && root->is_object()
                            ? root->find("payload")
                            : nullptr;
  if (payload == nullptr || !payload->is_object()) {
    std::puts("layerd: running (status payload unavailable; use --json)");
    return;
  }
  const auto text = [](const json::Value *value) {
    return value != nullptr && value->is_string() ? value->as_string()
                                                  : std::string{};
  };
  const auto *process = payload->find("process");
  const auto *pid = process != nullptr && process->is_object()
                        ? process->find("pid")
                        : nullptr;
  std::printf("layerd %s, pid %lld\n",
              text(payload->find("packageVersion")).c_str(),
              pid != nullptr && pid->is_integer()
                  ? static_cast<long long>(pid->as_integer())
                  : 0LL);
  if (const auto *x11 = payload->find("x11"); x11 != nullptr && x11->is_object()) {
    const auto *connected = x11->find("connected");
    const bool up = connected != nullptr && connected->is_bool() &&
                    connected->as_bool();
    const auto reason = text(x11->find("reason"));
    std::printf("x11: %s%s%s\n", up ? "connected" : "not connected",
                reason.empty() ? "" : " - ", reason.c_str());
  }
  if (const auto *sessions = payload->find("sessions");
      sessions != nullptr && sessions->is_array()) {
    std::printf("sessions: %zu\n", sessions->as_array().size());
    for (const auto &session : sessions->as_array()) {
      if (!session.is_object()) continue;
      const auto *foreground = session.find("foreground");
      std::printf("  %s window %s%s\n", text(session.find("profileId")).c_str(),
                  text(session.find("window")).c_str(),
                  foreground != nullptr && foreground->is_bool() &&
                          foreground->as_bool()
                      ? " (foreground)"
                      : "");
    }
  }
  if (const auto *capabilities = payload->find("capabilities");
      capabilities != nullptr && capabilities->is_array()) {
    std::puts("capabilities:");
    for (const auto &capability : capabilities->as_array()) {
      if (!capability.is_object()) continue;
      std::string reasons;
      if (const auto *list = capability.find("reasons");
          list != nullptr && list->is_array())
        for (const auto &reason : list->as_array())
          if (reason.is_string())
            reasons += (reasons.empty() ? " (" : ", ") + reason.as_string();
      if (!reasons.empty()) reasons += ')';
      std::printf("  %-15s %s%s\n", text(capability.find("capability")).c_str(),
                  text(capability.find("availability")).c_str(),
                  reasons.c_str());
    }
  }
}

int call_daemon(const platform::ControlCommand command, bool json_output) {
  std::string error;
  const auto response = platform::control_roundtrip(
      platform::encode_control_request(command), error);
  if (!response.has_value()) {
    std::fprintf(stderr, "layerctl: %s\n", error.c_str());
    return 1;
  }
  if (json_output) {
    std::fwrite(response->data(), 1, response->size(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }
  const auto decoded = platform::decode_control_response(*response);
  if (!decoded.has_value() || !decoded->ok) {
    const std::string message =
        decoded.has_value() && !decoded->error.empty()
            ? decoded->error
            : "the daemon returned an invalid response";
    std::fprintf(stderr, "layerctl: %s\n", message.c_str());
    return 1;
  }
  if (command == platform::ControlCommand::status) {
    print_status_summary(*response);
    return 0;
  }
  std::printf("layerctl: %s acknowledged\n",
              std::string(platform::control_command_name(command)).c_str());
  return 0;
}

int profile_command(int argc, char **argv) {
  const std::string_view subcommand{argv[2]};
  const auto roots = platform::profile_store_roots();
  if (subcommand == "import-json") {
    require_args(argc, 3);
    const auto input =
        read_stream(STDIN_FILENO, layerc::core::max_profile_json_bytes);
    if (!input.has_value()) return 1;
    const auto parsed = layerc::core::parse_profile_json(*input);
    const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
    if (input->empty() || profile == nullptr) {
      const auto *error = std::get_if<layerc::core::ProfileError>(&parsed);
      if (input->empty() || error == nullptr)
        std::fputs("layerctl: input is not a valid LayerC profile JSON\n",
                   stderr);
      else
        std::fprintf(stderr,
                     "layerctl: input is not a valid LayerC profile JSON "
                     "(%s: %s)\n",
                     error->path.c_str(), error->message.c_str());
      return 1;
    }
    if (!platform::ensure_profile_store_roots(roots)) {
      std::fputs("layerctl: the profile store roots could not be created\n",
                 stderr);
      return 1;
    }
    // Collision-aware: an id whose sanitized name matches another profile's
    // file gets a suffixed name instead of failing as "already exists".
    const auto scan = scan_or_die(roots);
    if (!scan.has_value()) return 1;
    const auto target = scan->path_for(roots.config_profiles, profile->id);
    const auto written =
        platform::write_profile_bundle_create_only(target, *profile);
    if (const auto error = written.error()) {
      std::fprintf(stderr, "layerctl: profile import failed: %s\n",
                   platform::profile_write_error_text(*error));
      return 1;
    }
    std::fprintf(stderr, "layerctl: imported profile '%s'\n",
                 profile->id.c_str());
    (void)platform::signal_profile_reload();
    return 0;
  }
  if (subcommand == "export-json") {
    require_args(argc, 4);
    const std::string_view id{argv[3]};
    const auto scan = scan_or_die(roots);
    if (!scan.has_value()) return 1;
    const auto *entry = scan->find(id);
    const auto contents = entry != nullptr
                              ? platform::read_profile_file(entry->path)
                              : std::nullopt;
    if (!contents.has_value()) {
      std::fprintf(stderr, "layerctl: profile '%s' was not found\n",
                   std::string{id}.c_str());
      return 1;
    }
    std::fwrite(contents->data(), 1, contents->size(), stdout);
    return 0;
  }
  usage();
}

int bundle_command(int argc, char **argv) {
  const std::string_view subcommand{argv[2]};
  if (subcommand != "import" && subcommand != "export") usage();

  const auto roots = platform::profile_store_roots();

  if (subcommand == "import") {
    // `bundle import` replaces colliding profiles by default; --keep-existing
    // skips them instead, and --dry-run prints the plan without writing.
    bool keep_existing{};
    bool dry_run{};
    for (int index = 3; index < argc; ++index) {
      const std::string_view flag{argv[index]};
      if (flag == "--keep-existing")
        keep_existing = true;
      else if (flag == "--dry-run")
        dry_run = true;
      else
        usage();
    }
    const auto input = read_stream(STDIN_FILENO, bundle::max_pack_bytes + 1);
    if (!input.has_value()) return 1;
    if (input->empty() || input->size() > bundle::max_pack_bytes) {
      std::fputs("layerctl: empty or oversized bundle input\n", stderr);
      return 1;
    }
    auto staged = StagedFile::stage_bytes(*input);
    if (!staged.has_value()) {
      std::fputs("layerctl: the bundle could not be staged\n", stderr);
      return 1;
    }
    const auto validated = bundle::validate_profile_pack(staged->path());
    const auto *pack = std::get_if<bundle::ValidatedPack>(&validated);
    if (pack == nullptr) {
      const auto *error = std::get_if<bundle::PackError>(&validated);
      std::fprintf(stderr, "layerctl: invalid bundle (%s)\n",
                   error != nullptr ? error->message.c_str() : "unknown");
      return 1;
    }
    if (!platform::ensure_profile_store_roots(roots)) {
      std::fputs("layerctl: the profile store roots could not be created\n",
                 stderr);
      return 1;
    }
    const auto scan = scan_or_die(roots);
    if (!scan.has_value()) return 1;
    // One resolved plan for preview and commit: filename-aware collision
    // detection and validation errors come from the library, never from a
    // CLI-local ID-only reimplementation. The preview binds each choice to
    // the reviewed destination, so the commit fails if the store changed.
    std::vector<bundle::ImportChoice> requested;
    requested.reserve(pack->profiles.size());
    for (std::size_t index = 0; index < pack->profiles.size(); ++index)
      requested.push_back(
          {.profile_index = index, .action = bundle::ImportAction::replace});
    const auto preview = bundle::preview_profile_import(
        *pack, requested, roots.config_profiles);
    if (preview.error.has_value()) {
      std::fprintf(stderr, "layerctl: bundle import failed (%s)\n",
                   preview.error->message.c_str());
      return 1;
    }
    auto choices = preview.choices;
    for (std::size_t index = 0; index < preview.entries.size(); ++index) {
      const auto &entry = preview.entries[index];
      const bool keep = keep_existing && entry.collides;
      if (keep) choices[index].action = bundle::ImportAction::skip;
      if (keep) {
        std::fprintf(stderr, "layerctl: keeping existing profile '%s'\n",
                     entry.id.c_str());
      } else {
        const char *verb = import_verb(dry_run, entry.collides);
        std::fprintf(stderr, "layerctl: %s profile '%s'\n", verb,
                     entry.id.c_str());
      }
    }
    if (dry_run) {
      // Dry-run validates the exact choices the commit would use through
      // the side-effect-free planner, so preview/commit parity is enforced
      // and no profile mutations are published on this path.
      if (const auto invalid = bundle::validate_import_choices(
              *pack, choices, roots.config_profiles);
          invalid.has_value()) {
        std::fprintf(stderr, "layerctl: bundle import failed (%s)\n",
                     invalid->message.c_str());
        return 1;
      }
      return 0;
    }
    const auto imported = bundle::import_profile_pack(
        *pack, choices, roots.config_profiles);
    if (!std::holds_alternative<std::monostate>(imported)) {
      const auto *error = std::get_if<bundle::PackError>(&imported);
      std::fprintf(stderr, "layerctl: bundle import failed (%s)\n",
                   error != nullptr ? error->message.c_str() : "unknown");
      return 1;
    }
    return 0;
  }
  require_min_args(argc, 4);
  const auto scan = scan_or_die(roots);
  if (!scan.has_value()) return 1;
  std::vector<std::filesystem::path> targets;
  for (int index = 3; index < argc; ++index) {
    const std::string_view id{argv[index]};
    const auto *entry = scan->find(id);
    if (entry == nullptr) {
      std::fprintf(stderr, "layerctl: profile '%s' was not found\n",
                   std::string{id}.c_str());
      return 1;
    }
    targets.push_back(entry->path);
  }
  auto staged = StagedFile::reserve();
  if (!staged.has_value()) {
    std::fputs("layerctl: a secure staging file could not be reserved\n",
               stderr);
    return 1;
  }
  const auto exported = bundle::export_profile_pack(
      staged->path(), {targets.data(), targets.size()});
  if (!std::holds_alternative<std::monostate>(exported)) {
    const auto *error = std::get_if<bundle::PackError>(&exported);
    std::fprintf(stderr, "layerctl: bundle export failed (%s)\n",
                 error != nullptr ? error->message.c_str() : "unknown");
    return 1;
  }
  const auto bytes = read_file(staged->path());
  if (!bytes.has_value()) {
    std::fputs("layerctl: the exported bundle was unreadable\n", stderr);
    return 1;
  }
  std::fwrite(bytes->data(), 1, bytes->size(), stdout);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) usage();
  const std::string_view command{argv[1]};
  if (command == "status") {
    // Reject trailing/unknown args: `status foo` must not silently behave
    // like a bare `status`.
    if (argc > 3) usage();
    if (argc == 3 && std::string_view{argv[2]} != "--json") usage();
    return call_daemon(platform::ControlCommand::status, argc == 3);
  }
  if (command == "reload")
    return call_daemon(platform::ControlCommand::reload, false);
  if (command == "stop")
    return call_daemon(platform::ControlCommand::stop, false);
  if (command == "profile" && argc >= 3) return profile_command(argc, argv);
  if (command == "bundle" && argc >= 3) return bundle_command(argc, argv);
  usage();
}
