#pragma once

#include "layerc/core/json_reader.hpp"
#include "layerc/core/profile.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::core::detail {

// Reads profile JSON shape and types; validate_profile owns the semantics.
class ProfileReader {
public:
  explicit ProfileReader(const json::Value &root);
  ProfileResult read();

private:
  [[nodiscard]] ProfileError failure() const;
  bool read_cooldowns(GameProfile &profile, std::uint32_t source_schema);
  std::optional<std::int64_t> read_duration(const json::Value &value,
                                            const std::string &path,
                                            std::string_view key);
  std::optional<std::int64_t> read_charges(const json::Value &value,
                                           const std::string &path,
                                           std::string_view key);
  bool read_cooldown_groups(GameProfile &profile,
                            std::uint32_t source_schema);
  bool read_panels(GameProfile &profile, std::uint32_t source_schema);
  bool read_items(const json::Value &value, const std::string &path,
                  Panel &panel);
  bool read_item(const json::Value &value, const std::string &path,
                 OverlayItem &item);
  bool read_modules(GameProfile &profile, std::uint32_t source_schema);
  bool read_hid_bindings(GameProfile &profile, std::uint32_t source_schema);
  bool read_hid_device(const json::Value &device, const std::string &path,
                       HidDeviceSelector &out);
  bool read_hid_control(const json::Value &control, const std::string &path,
                        HidControlBinding &out);
  bool read_bindings(GameProfile &profile);
  bool read_chord_token(const json::Value &token_value,
                        const std::string &token_path, InputBinding &binding);

  const json::Value &root_;
  std::map<std::string, std::uint16_t, std::less<>> hid_binding_ids_;
  json::Reader fields_;
};

} // namespace layerc::core::detail
