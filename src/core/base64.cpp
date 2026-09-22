// Portable base64 codec. See base64.hpp.

#include "layerc/core/base64.hpp"

#include <array>
#include <cstdint>

namespace layerc::core {
namespace {

constexpr std::string_view k_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 256-entry decode table: O(1) per byte instead of an alphabet scan.
// -1 marks out-of-alphabet bytes; '=' is handled structurally, never here.
constexpr std::array<int, 256> make_decode_table() {
  std::array<int, 256> table{};
  table.fill(-1);
  for (std::size_t index = 0; index < k_alphabet.size(); ++index)
    table[static_cast<unsigned char>(k_alphabet[index])] =
        static_cast<int>(index);
  return table;
}

constexpr std::array<int, 256> k_decode = make_decode_table();

} // namespace

std::string base64_encode(const std::string_view source) {
  std::string output;
  output.reserve((source.size() + 2U) / 3U * 4U);
  for (std::size_t offset = 0; offset < source.size(); offset += 3U) {
    const auto remaining = source.size() - offset;
    const auto first = static_cast<unsigned char>(source[offset]);
    const auto second = remaining > 1U
                            ? static_cast<unsigned char>(source[offset + 1U])
                            : 0U;
    const auto third = remaining > 2U
                           ? static_cast<unsigned char>(source[offset + 2U])
                           : 0U;
    const auto value = (static_cast<std::uint32_t>(first) << 16U) |
                       (static_cast<std::uint32_t>(second) << 8U) |
                       static_cast<std::uint32_t>(third);
    output.push_back(k_alphabet[(value >> 18U) & 0x3fU]);
    output.push_back(k_alphabet[(value >> 12U) & 0x3fU]);
    output.push_back(remaining > 1U ? k_alphabet[(value >> 6U) & 0x3fU]
                                    : '=');
    output.push_back(remaining > 2U ? k_alphabet[value & 0x3fU] : '=');
  }
  return output;
}

std::optional<std::string> base64_decode(const std::string_view source) {
  if (source.size() % 4U != 0) return std::nullopt;
  std::string output;
  output.reserve(source.size() / 4U * 3U);
  for (std::size_t offset = 0; offset < source.size(); offset += 4U) {
    const auto first = k_decode[static_cast<unsigned char>(source[offset])];
    const auto second =
        k_decode[static_cast<unsigned char>(source[offset + 1U])];
    if (first < 0 || second < 0) return std::nullopt;

    const bool final_group = offset + 4U == source.size();
    const auto third_character = source[offset + 2U];
    const auto fourth_character = source[offset + 3U];
    output.push_back(static_cast<char>((first << 2U) | (second >> 4U)));
    if (third_character == '=') {
      if (!final_group || fourth_character != '=' || (second & 0x0f) != 0)
        return std::nullopt;
      continue;
    }

    const auto third =
        k_decode[static_cast<unsigned char>(third_character)];
    if (third < 0) return std::nullopt;
    output.push_back(static_cast<char>((second << 4U) | (third >> 2U)));
    if (fourth_character == '=') {
      if (!final_group || (third & 0x03) != 0) return std::nullopt;
      continue;
    }

    const auto fourth =
        k_decode[static_cast<unsigned char>(fourth_character)];
    if (fourth < 0) return std::nullopt;
    output.push_back(static_cast<char>((third << 6U) | fourth));
  }
  return output;
}

} // namespace layerc::core
