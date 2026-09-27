#include "release_client.hpp"

#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace layerc::launcher::win32 {
namespace {

class AlgorithmHandle {
public:
  explicit AlgorithmHandle(const wchar_t *algorithm) noexcept {
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle_, algorithm, nullptr, 0)))
      handle_ = nullptr;
  }
  ~AlgorithmHandle() {
    if (handle_ != nullptr) BCryptCloseAlgorithmProvider(handle_, 0);
  }
  AlgorithmHandle(const AlgorithmHandle &) = delete;
  AlgorithmHandle &operator=(const AlgorithmHandle &) = delete;
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
  BCRYPT_ALG_HANDLE handle_{};
};

class KeyHandle {
public:
  KeyHandle() = default;
  ~KeyHandle() {
    if (handle_ != nullptr) BCryptDestroyKey(handle_);
  }
  KeyHandle(const KeyHandle &) = delete;
  KeyHandle &operator=(const KeyHandle &) = delete;
  [[nodiscard]] BCRYPT_KEY_HANDLE *put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return handle_; }

private:
  BCRYPT_KEY_HANDLE handle_{};
};

class InternetHandle {
public:
  InternetHandle() = default;
  explicit InternetHandle(HINTERNET handle) noexcept : handle_(handle) {}
  ~InternetHandle() {
    if (handle_ != nullptr) WinHttpCloseHandle(handle_);
  }
  InternetHandle(const InternetHandle &) = delete;
  InternetHandle &operator=(const InternetHandle &) = delete;
  [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
  void reset(HINTERNET handle) noexcept {
    if (handle_ != nullptr) WinHttpCloseHandle(handle_);
    handle_ = handle;
  }

private:
  HINTERNET handle_{};
};

[[nodiscard]] int milliseconds(const std::chrono::milliseconds value) noexcept {
  return static_cast<int>(value.count());
}

[[nodiscard]] std::wstring widen_ascii(const std::string_view text) {
  return {text.begin(), text.end()};
}

[[nodiscard]] std::string win32_message(const char *what) {
  return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

} // namespace

bool BcryptSignatureVerifier::verify(const std::string_view manifest,
                                     const std::string_view der_signature) {
  const auto signature = ecdsa_der_to_p1363(der_signature);
  if (!signature) return false;

  std::array<UCHAR, 32> digest{};
  const AlgorithmHandle sha256(BCRYPT_SHA256_ALGORITHM);
  if (sha256.get() == nullptr ||
      !BCRYPT_SUCCESS(BCryptHash(
          sha256.get(), nullptr, 0,
          reinterpret_cast<PUCHAR>(const_cast<char *>(manifest.data())),
          static_cast<ULONG>(manifest.size()), digest.data(),
          static_cast<ULONG>(digest.size()))))
    return false;

  std::array<UCHAR, sizeof(BCRYPT_ECCKEY_BLOB) + std::tuple_size_v<P256PublicKey>> blob{};
  const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC,
                                  static_cast<ULONG>(p256_coordinate_bytes)};
  std::memcpy(blob.data(), &header, sizeof(header));
  std::memcpy(blob.data() + sizeof(header), key_.data(), key_.size());

  const AlgorithmHandle ecdsa(BCRYPT_ECDSA_P256_ALGORITHM);
  KeyHandle key;
  if (ecdsa.get() == nullptr ||
      !BCRYPT_SUCCESS(BCryptImportKeyPair(ecdsa.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                          key.put(), blob.data(),
                                          static_cast<ULONG>(blob.size()), 0)))
    return false;
  auto raw = *signature;
  return BCRYPT_SUCCESS(BCryptVerifySignature(
      key.get(), nullptr, digest.data(), static_cast<ULONG>(digest.size()),
      raw.data(), static_cast<ULONG>(raw.size()), 0));
}

struct GitHubReleaseSource::Handles {
  InternetHandle session;
  InternetHandle connection;
};

GitHubReleaseSource::GitHubReleaseSource(std::wstring repository, std::wstring tag,
                                         const Timeouts timeouts)
    : handles_(std::make_unique<Handles>()), repository_(std::move(repository)),
      tag_(std::move(tag)) {
  // Automatic proxy discovery honours the user's system proxy settings.
  handles_->session.reset(
      WinHttpOpen(L"LayerC-Launcher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (handles_->session.get() == nullptr) {
    last_error_ = win32_message("WinHttpOpen");
    return;
  }
  WinHttpSetTimeouts(handles_->session.get(), milliseconds(timeouts.resolve),
                     milliseconds(timeouts.connect), milliseconds(timeouts.send),
                     milliseconds(timeouts.receive));
  // TLS 1.2 or newer only; TLS 1.3 is added where the SDK and OS know it.
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!WinHttpSetOption(handles_->session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS,
                        &protocols, sizeof(protocols)))
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#endif
  WinHttpSetOption(handles_->session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS,
                   &protocols, sizeof(protocols));
  handles_->connection.reset(WinHttpConnect(
      handles_->session.get(), L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (handles_->connection.get() == nullptr)
    last_error_ = win32_message("WinHttpConnect");
}

GitHubReleaseSource::~GitHubReleaseSource() = default;

std::optional<std::string> GitHubReleaseSource::fetch(const std::string_view asset_name,
                                                      const std::size_t max_bytes) {
  if (handles_->connection.get() == nullptr) return std::nullopt;
  const auto path = L"/" + repository_ + L"/releases/download/" + tag_ + L"/" +
                    widen_ascii(asset_name);
  const InternetHandle request(WinHttpOpenRequest(
      handles_->connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (request.get() == nullptr) {
    last_error_ = win32_message("WinHttpOpenRequest");
    return std::nullopt;
  }
  // Never follow a redirect from HTTPS down to HTTP, and send no cookies.
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                   sizeof(redirect));
  DWORD no_cookies = WINHTTP_DISABLE_COOKIES;
  WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &no_cookies,
                   sizeof(no_cookies));

  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    last_error_ = win32_message("the HTTPS request");
    return std::nullopt;
  }
  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(),
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    last_error_ = std::string(asset_name) + " returned HTTP " + std::to_string(status);
    return std::nullopt;
  }

  std::string body;
  std::vector<char> chunk(64 * 1024);
  while (true) {
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()),
                         &read)) {
      last_error_ = win32_message("reading the response");
      return std::nullopt;
    }
    if (read == 0) break;
    if (body.size() + read > max_bytes) {
      last_error_ = std::string(asset_name) + " is larger than expected";
      return std::nullopt;
    }
    body.append(chunk.data(), read);
  }
  last_error_.clear();
  return body;
}

} // namespace layerc::launcher::win32
