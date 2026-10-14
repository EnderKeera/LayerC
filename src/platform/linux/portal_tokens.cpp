#include "portal_tokens.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard_linux.hpp"

#include <cstdlib>
#include <span>

namespace layerc::platform {
namespace {

[[nodiscard]] std::filesystem::path absolute_environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') return {};
  std::filesystem::path path{value};
  return path.is_absolute() ? path : std::filesystem::path{};
}

[[nodiscard]] bool valid_entry(const std::string_view profile_id,
                               const std::string_view token) noexcept {
  return !profile_id.empty() && profile_id.size() <= screencast_token_max_bytes &&
         token.size() <= screencast_token_max_bytes &&
         layerc::core::is_valid_utf8(profile_id) && layerc::core::is_valid_utf8(token);
}

} // namespace

std::filesystem::path screencast_token_path() {
  auto data = absolute_environment("XDG_DATA_HOME");
  if (data.empty()) {
    const auto home = absolute_environment("HOME");
    if (home.empty()) return {};
    data = home / ".local" / "share";
  }
  return (data / "layerc" / "state" / "screencast.json").lexically_normal();
}

void ScreencastTokenStore::load() {
  if (loaded_) return;
  loaded_ = true;
  if (path_.empty()) return;
  const auto bytes = read_durable_file(path_, screencast_token_file_max_bytes);
  if (!bytes) return;
  const std::string_view text{reinterpret_cast<const char *>(bytes->data()), bytes->size()};
  const auto parsed = layerc::core::json::parse_bounded(text, screencast_token_file_max_bytes);
  const auto *root = std::get_if<layerc::core::json::Value>(&parsed);
  if (root == nullptr || !root->is_object()) return;
  const auto *version = root->find("version");
  const auto *tokens = root->find("tokens");
  if (version == nullptr || !version->is_integer() || version->as_integer() != 1 ||
      tokens == nullptr || !tokens->is_object())
    return;
  for (const auto &[profile, value] : tokens->as_object()) {
    if (tokens_.size() >= screencast_token_max_entries) break;
    if (!value.is_string() || value.as_string().empty() ||
        !valid_entry(profile, value.as_string()))
      continue;
    tokens_.emplace(profile, value.as_string());
  }
}

std::optional<std::string> ScreencastTokenStore::token(const std::string_view profile_id) {
  load();
  const auto found = tokens_.find(profile_id);
  if (found == tokens_.end()) return std::nullopt;
  return found->second;
}

bool ScreencastTokenStore::replace(const std::string_view profile_id,
                                   const std::string_view token) {
  load();
  if (path_.empty() || !valid_entry(profile_id, token)) return false;
  auto next = tokens_;
  if (token.empty())
    next.erase(std::string{profile_id});
  else
    next.insert_or_assign(std::string{profile_id}, std::string{token});
  if (next.size() > screencast_token_max_entries) return false;
  if (next == tokens_) return true;

  const auto state = path_.parent_path();
  if (!ensure_owned_directory(state.parent_path()) || !ensure_owned_directory(state))
    return false;
  std::string out = "{\"version\":1,\"tokens\":{";
  bool first = true;
  for (const auto &[profile, value] : next) {
    if (!first) out += ',';
    first = false;
    layerc::core::json::append_quoted(out, profile);
    out += ':';
    layerc::core::json::append_quoted(out, value);
  }
  out += "}}\n";
  if (write_durable_file(path_, std::as_bytes(std::span{out.data(), out.size()})) ==
      DurablePublishOutcome::failed)
    return false;
  // An unconfirmed publication is still visible, so memory follows it.
  tokens_ = std::move(next);
  return true;
}

} // namespace layerc::platform
