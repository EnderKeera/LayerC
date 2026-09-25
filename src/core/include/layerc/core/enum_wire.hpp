#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace layerc::core {

template <typename Enum> struct EnumWireEntry {
  Enum value;
  std::string_view name;
};

template <typename Enum>
[[nodiscard]] constexpr std::optional<std::string_view>
enum_wire_name(const std::span<const EnumWireEntry<Enum>> entries,
               const Enum value) noexcept {
  for (const auto &entry : entries)
    if (entry.value == value) return entry.name;
  return std::nullopt;
}

template <typename Enum>
[[nodiscard]] constexpr std::optional<Enum>
parse_enum_wire(const std::span<const EnumWireEntry<Enum>> entries,
                const std::string_view name) noexcept {
  for (const auto &entry : entries)
    if (entry.name == name) return entry.value;
  return std::nullopt;
}

template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::optional<std::string_view>
enum_wire_name(const std::array<EnumWireEntry<Enum>, N> &entries,
               Enum value) noexcept {
  return enum_wire_name(std::span<const EnumWireEntry<Enum>>(entries), value);
}

template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::optional<Enum>
parse_enum_wire(const std::array<EnumWireEntry<Enum>, N> &entries,
                std::string_view name) noexcept {
  return parse_enum_wire(std::span<const EnumWireEntry<Enum>>(entries), name);
}

} // namespace layerc::core
