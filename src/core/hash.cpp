#include "layerc/core/hash.hpp"

#include <array>
#include <cstdint>
#include <string>

#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>

namespace layerc::core {
namespace {

// One shared algorithm handle; BCrypt hash handles are created per call.
NTSTATUS open_sha256(BCRYPT_ALG_HANDLE &handle) {
  return BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM,
                                     nullptr, 0);
}

} // namespace

std::string sha256(const std::span<const std::uint8_t> bytes) {
  std::array<std::uint8_t, 32> digest{};
  BCRYPT_ALG_HANDLE algorithm{};
  if (!BCRYPT_SUCCESS(open_sha256(algorithm)))
    return {};
  const NTSTATUS hashed = BCryptHash(
      algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
      static_cast<ULONG>(bytes.size()), digest.data(),
      static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!BCRYPT_SUCCESS(hashed))
    return {};

  constexpr char hex[] = "0123456789abcdef";
  std::string output(64, '\0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    const auto value = digest[index];
    output[index * 2U] = hex[value >> 4U];
    output[index * 2U + 1U] = hex[value & 0x0FU];
  }
  return output;
}

std::string sha256(const std::span<const std::byte> bytes) {
  if (bytes.empty()) return sha256(std::span<const std::uint8_t>{});
  return sha256(std::span(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
}

std::string sha256(const std::string_view text) {
  if (text.empty()) return sha256(std::span<const std::uint8_t>{});
  return sha256(std::span(
      reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

bool is_lowercase_sha256(const std::string_view value) noexcept {
  if (value.size() != 64U) return false;
  for (const auto character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f')))
      return false;
  }
  return true;
}

} // namespace layerc::core