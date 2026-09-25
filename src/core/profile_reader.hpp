#pragma once

#include "layerc/core/json_reader.hpp"
#include "layerc/core/profile.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace layerc::core::detail {

class ProfileReader {
public:
  explicit ProfileReader(const json::Value &root);
  ProfileResult read();

private:
  [[nodiscard]] ProfileError failure() const;
  bool read_cooldowns(GameProfile &profile, std::uint32_t source_schema);
  bool read_cooldown_groups(GameProfile &profile,
                            std::uint32_t source_schema);
  bool read_panels(GameProfile &profile);
  bool read_modules(GameProfile &profile, std::uint32_t source_schema);
  bool read_hid_bindings(GameProfile &profile, std::uint32_t source_schema);
  bool read_hid_device(const json::Value &device, const std::string &path,
                       HidDeviceSelector &out);
  bool read_hid_control(const json::Value &control, const std::string &path,
                        HidControlBinding &out);
  bool read_bindings(GameProfile &profile);
  bool read_chord_token(const json::Value &token_value,
                        const std::string &token_path,
                        std::set<InputToken> &tokens,
                        InputBinding &binding);

  const json::Value &root_;
  std::set<std::string, std::less<>> cooldown_ids_;
  std::map<std::string, std::uint16_t, std::less<>> hid_binding_ids_;
  json::Reader fields_;
};

} // namespace layerc::core::detail
