#pragma once

#include "layerc/core/types.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace layerc::core {

class CooldownEngine {
public:
  explicit CooldownEngine(std::span<const CooldownDefinition> definitions);

  [[nodiscard]] bool trigger(std::string_view cooldown_id, TimePoint now);
  [[nodiscard]] bool reset(std::string_view cooldown_id);
  void update(TimePoint now);

  [[nodiscard]] std::vector<CooldownState> snapshot(TimePoint now);
  void snapshot(TimePoint now, std::vector<CooldownState> &output);
  [[nodiscard]] std::optional<Milliseconds>
  remaining(std::string_view cooldown_id, TimePoint now);

private:
  struct Entry {
    const CooldownDefinition *definition{};
    std::uint32_t available_charges{};
    std::optional<TimePoint> next_charge_at;
  };

  void update_entry(Entry &entry, TimePoint now);

  std::vector<std::string_view> order_;
  std::unordered_map<std::string_view, Entry> entries_;
};

} // namespace layerc::core
