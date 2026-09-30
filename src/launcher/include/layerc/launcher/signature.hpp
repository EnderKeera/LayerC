#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace layerc::launcher {

// The release key is ECDSA over NIST P-256 with SHA-256. CI signs with
// `openssl dgst -sha256 -sign`, which writes a DER SEQUENCE of two INTEGERs;
// Windows BCryptVerifySignature takes the fixed-width r||s form instead.
inline constexpr std::size_t p256_coordinate_bytes = 32;
using P256Signature = std::array<std::uint8_t, 2 * p256_coordinate_bytes>;
// Uncompressed point without the 0x04 prefix: X||Y.
using P256PublicKey = std::array<std::uint8_t, 2 * p256_coordinate_bytes>;

// Strict DER: short-form lengths, minimal positive INTEGERs no wider than a
// coordinate, and no trailing bytes. Anything else is rejected, not repaired.
[[nodiscard]] std::optional<P256Signature>
ecdsa_der_to_p1363(std::string_view der) noexcept;

// Accepts the uncompressed point as 130 hex digits beginning with "04"
// (the last 65 bytes of `openssl ec -pubout -outform DER`), either case,
// surrounding whitespace ignored. An empty string means no key is built in.
[[nodiscard]] std::optional<P256PublicKey>
parse_p256_public_key_hex(std::string_view hex) noexcept;

} // namespace layerc::launcher
