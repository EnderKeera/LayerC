#include "release_client.hpp"

#include "layerc/core/update_manifest.hpp"
#include "layerc/launcher/signature.hpp"
#include "layerc/launcher/update_engine.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool read_bounded(const fs::path &path, const std::size_t maximum,
                                std::string &contents) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;

  std::array<char, 4096> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count <= 0) break;
    const auto amount = static_cast<std::size_t>(count);
    if (contents.size() > maximum || amount > maximum - contents.size())
      return false;
    contents.append(buffer.data(), amount);
  }
  return input.eof();
}

} // namespace

int main(const int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "Usage: layerc_verify_update <public-key.hex> <manifest.json> "
                 "<manifest.sig>\n";
    return 64;
  }

  std::string public_key_hex;
  std::string manifest;
  std::string signature;
  if (!read_bounded(argv[1], 4096, public_key_hex) ||
      !read_bounded(argv[2], layerc::core::max_update_manifest_bytes, manifest) ||
      !read_bounded(argv[3], layerc::launcher::max_signature_bytes, signature)) {
    std::cerr << "Invalid update signature input\n";
    return 1;
  }

  const auto key = layerc::launcher::parse_p256_public_key_hex(public_key_hex);
  const auto parsed_manifest =
      layerc::core::parse_update_manifest_json(manifest);
  if (!key ||
      std::holds_alternative<layerc::core::UpdateManifestError>(parsed_manifest)) {
    std::cerr << "Invalid update signature input\n";
    return 1;
  }

  layerc::launcher::win32::BcryptSignatureVerifier verifier(*key);
  if (!verifier.verify(manifest, signature)) {
    std::cerr << "Invalid update signature\n";
    return 1;
  }
  return 0;
}
