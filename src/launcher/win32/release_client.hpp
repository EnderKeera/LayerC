#pragma once

#include "layerc/launcher/signature.hpp"
#include "layerc/launcher/update_engine.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace layerc::launcher::win32 {

// Verifies the ECDSA P-256 / SHA-256 manifest signature (DER, as openssl
// writes it) with Windows CNG against the key compiled into the launcher.
class BcryptSignatureVerifier final : public SignatureVerifier {
public:
  explicit BcryptSignatureVerifier(const P256PublicKey &key) noexcept : key_(key) {}
  [[nodiscard]] bool verify(std::string_view manifest,
                            std::string_view der_signature) override;

private:
  P256PublicKey key_;
};

// Reads assets of one GitHub release over HTTPS only:
//   https://github.com/<repository>/releases/download/<tag>/<asset>
// WinHTTP follows GitHub's redirect to its asset host. No cookies, no
// credentials and no identifiers are sent; the user agent names only LayerC.
class GitHubReleaseSource final : public ReleaseSource {
public:
  struct Timeouts {
    std::chrono::milliseconds resolve{5'000};
    std::chrono::milliseconds connect{5'000};
    std::chrono::milliseconds send{10'000};
    std::chrono::milliseconds receive{30'000};
  };

  GitHubReleaseSource(std::wstring repository, std::wstring tag,
                      Timeouts timeouts);
  ~GitHubReleaseSource() override;
  GitHubReleaseSource(const GitHubReleaseSource &) = delete;
  GitHubReleaseSource &operator=(const GitHubReleaseSource &) = delete;

  [[nodiscard]] std::optional<std::string>
  fetch(std::string_view asset_name, std::size_t max_bytes) override;

  // The last transport failure, for the launcher log ("" after a success).
  [[nodiscard]] const std::string &last_error() const noexcept { return last_error_; }

private:
  struct Handles;
  std::unique_ptr<Handles> handles_;
  std::wstring repository_;
  std::wstring tag_;
  std::string last_error_;
};

} // namespace layerc::launcher::win32
