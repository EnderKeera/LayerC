#include "layerc/launcher/signature.hpp"

#include <algorithm>

namespace layerc::launcher {
namespace {

[[nodiscard]] std::uint8_t byte_at(const std::string_view bytes,
                                   const std::size_t index) noexcept {
  return static_cast<std::uint8_t>(bytes[index]);
}

// Reads one DER INTEGER at `offset` into a big-endian coordinate, left
// padded with zeros. Advances `offset` past it.
[[nodiscard]] bool read_integer(const std::string_view der, std::size_t &offset,
                                std::uint8_t *coordinate) noexcept {
  if (offset + 2 > der.size() || byte_at(der, offset) != 0x02) return false;
  const std::size_t length = byte_at(der, offset + 1);
  offset += 2;
  if (length == 0 || length > p256_coordinate_bytes + 1 ||
      offset + length > der.size())
    return false;
  auto value = der.substr(offset, length);
  offset += length;
  // A negative INTEGER is never a valid r or s.
  if ((byte_at(value, 0) & 0x80U) != 0) return false;
  if (byte_at(value, 0) == 0x00) {
    // The zero byte is allowed only to keep a high-bit value positive.
    if (value.size() == 1 || (byte_at(value, 1) & 0x80U) == 0) return false;
    value.remove_prefix(1);
  }
  if (value.size() > p256_coordinate_bytes) return false;
  const auto padding = p256_coordinate_bytes - value.size();
  std::fill_n(coordinate, padding, std::uint8_t{0});
  for (std::size_t index = 0; index < value.size(); ++index)
    coordinate[padding + index] = byte_at(value, index);
  return true;
}

[[nodiscard]] int hex_value(const char digit) noexcept {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  return -1;
}

[[nodiscard]] bool is_space(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

} // namespace

std::optional<P256Signature> ecdsa_der_to_p1363(const std::string_view der) noexcept {
  if (der.size() < 8 || byte_at(der, 0) != 0x30) return std::nullopt;
  const std::size_t body = byte_at(der, 1);
  if ((body & 0x80U) != 0 || body + 2 != der.size()) return std::nullopt;
  P256Signature signature{};
  std::size_t offset = 2;
  if (!read_integer(der, offset, signature.data()) ||
      !read_integer(der, offset, signature.data() + p256_coordinate_bytes) ||
      offset != der.size())
    return std::nullopt;
  return signature;
}

std::optional<P256PublicKey>
parse_p256_public_key_hex(std::string_view hex) noexcept {
  while (!hex.empty() && is_space(hex.front())) hex.remove_prefix(1);
  while (!hex.empty() && is_space(hex.back())) hex.remove_suffix(1);
  if (hex.size() != 2 + 4 * p256_coordinate_bytes || hex.substr(0, 2) != "04")
    return std::nullopt;
  hex.remove_prefix(2);
  P256PublicKey key{};
  for (std::size_t index = 0; index < key.size(); ++index) {
    const int high = hex_value(hex[2 * index]);
    const int low = hex_value(hex[2 * index + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    key[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return key;
}

} // namespace layerc::launcher
