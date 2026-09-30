#pragma once

#include <string>
#include <string_view>

// Throwaway P-256 test key made with `openssl ecparam -name prime256v1
// -genkey`; its private half was discarded. It is NOT the release key and
// must never be built into a shipping launcher.
namespace layerc::test::signature_vectors {

inline constexpr std::string_view public_key_hex =
    "04642c8da978f742a0ac989d3a9e207cbebdc3ad2a0bf7fb9dbde80a8788306fc1"
    "6b323419dd25b0f1cef0a21702fc872decbdb7c0491f382173ed1e596fadd557";

inline constexpr std::string_view message =
    R"({"schemaVersion":1,"channel":"rolling","sequence":1,)"
    R"("commit":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","files":[{"path":)"
    R"("layerd.exe","module":"layerd","size":1,"sha256":)"
    R"("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}]})";

// `openssl dgst -sha256 -sign` over `message`: r needs no sign byte, s does.
inline constexpr std::string_view der_signature_hex =
    "304502202be7f4422158e0b32e62a30cdee250d70dcf940ce5644d7b181d5dedbdc2"
    "8004022100d467b8d33d16a765d9b0dc7e3476fc8a1c2b8c29648f8eaac289d34aca"
    "c6cdbb";

inline std::string from_hex(const std::string_view hex) {
  const auto digit = [](char value) {
    return value <= '9' ? value - '0' : value - 'a' + 10;
  };
  std::string bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    bytes.push_back(static_cast<char>((digit(hex[index]) << 4) | digit(hex[index + 1])));
  return bytes;
}

} // namespace layerc::test::signature_vectors
