#pragma once

// Single-use ScreenCast restore tokens, one per profile id, persisted in
// $XDG_DATA_HOME/layerc/state/screencast.json through the durable write
// helper (docs/Wayland.md, "Portal capture"). A missing, oversized, or
// malformed file reads as empty: the portal then asks the user again.

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

inline constexpr std::size_t screencast_token_max_bytes = 512;
inline constexpr std::size_t screencast_token_max_entries = 256;
inline constexpr std::size_t screencast_token_file_max_bytes = 256 * 1024;

// Empty when neither XDG_DATA_HOME nor an absolute HOME is set.
[[nodiscard]] std::filesystem::path screencast_token_path();

class ScreencastTokenStore {
public:
  explicit ScreencastTokenStore(std::filesystem::path path) : path_(std::move(path)) {}

  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }
  // Reads the file once; later calls are served from memory.
  [[nodiscard]] std::optional<std::string> token(std::string_view profile_id);
  // Replaces the profile's token (an empty token removes it) and publishes the
  // file durably. false when the value is out of bounds or the write failed.
  [[nodiscard]] bool replace(std::string_view profile_id, std::string_view token);

private:
  void load();

  std::filesystem::path path_;
  bool loaded_{};
  std::map<std::string, std::string, std::less<>> tokens_;
};

} // namespace layerc::platform
