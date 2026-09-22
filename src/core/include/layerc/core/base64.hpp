#pragma once

// Portable base64 codec shared by every core consumer (live-edit snapshot
// transport today, any module tomorrow). One owner — do not reimplement
// per module. Replaces the former CryptBinaryToStringA path with no
// bcrypt/crypt32 dependency.

#include <optional>
#include <string>
#include <string_view>

namespace layerc::core {

[[nodiscard]] std::string base64_encode(std::string_view source);
// Strict decode: nullopt on misaligned length, out-of-alphabet bytes, or
// misplaced/non-canonical padding (padding only in the final group, with
// zeroed trailing bits).
[[nodiscard]] std::optional<std::string>
base64_decode(std::string_view source);

} // namespace layerc::core
