#pragma once

#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"

#include <cstdint>
#include <optional>
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
  [[nodiscard]] bool ok() const;
  bool read_cooldowns(GameProfile &profile, std::uint32_t source_schema);
  bool read_cooldown_groups(GameProfile &profile,
                            std::uint32_t source_schema);
  bool read_panels(GameProfile &profile);
  bool read_modules(GameProfile &profile, std::uint32_t source_schema);
  bool read_hid_bindings(GameProfile &profile, std::uint32_t source_schema);
  bool read_bindings(GameProfile &profile);

  const json::Value *required_object(const json::Value &parent,
                                     std::string_view key,
                                     const std::string &path);
  const json::Value::Array *required_array(const json::Value &parent,
                                            std::string_view key,
                                            const std::string &path);
  bool required_string(const json::Value &parent, std::string_view key,
                       const std::string &path, std::string &output);
  void optional_string(const json::Value &parent, std::string_view key,
                       const std::string &path, std::string &output);
  void optional_boolean(const json::Value &parent, std::string_view key,
                        const std::string &path, bool &output);
  void optional_color(const json::Value &parent, std::string_view key,
                      const std::string &path, std::uint32_t &output);
  std::optional<std::int64_t>
  required_integer(const json::Value &parent, std::string_view key,
                   const std::string &path, std::int64_t minimum,
                   std::int64_t maximum);
  std::optional<std::int64_t>
  optional_integer(const json::Value &parent, std::string_view key,
                   const std::string &path, std::int64_t minimum,
                   std::int64_t maximum, std::int64_t fallback);
  std::optional<std::int64_t> checked_integer(const json::Value &value,
                                               const std::string &path,
                                               std::int64_t minimum,
                                               std::int64_t maximum);
  std::optional<double> optional_number(const json::Value &parent,
                                        std::string_view key,
                                        const std::string &path,
                                        double minimum, double maximum,
                                        double fallback);
  ProfileError error(const std::string &path, const std::string &message);
  bool set_error(std::string path, std::string message);

  const json::Value &root_;
  std::set<std::string, std::less<>> cooldown_ids_;
  std::map<std::string, std::uint16_t, std::less<>> hid_binding_ids_;
  ProfileError error_;
};

} // namespace layerc::core::detail
