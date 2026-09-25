#include "layerc/platform/visual_assets_windows.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <winioctl.h>

#include <cstring>

namespace layerc::platform {
namespace {

using Microsoft::WRL::ComPtr;

class ComScope {
public:
  ComScope() {
    const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized_ = SUCCEEDED(result);
    usable_ = initialized_ || result == RPC_E_CHANGED_MODE;
  }
  ~ComScope() {
    if (initialized_) CoUninitialize();
  }
  [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
  bool initialized_{};
  bool usable_{};
};

VisualAssetError failure(std::string message) {
  return {.message = std::move(message)};
}

std::optional<bool> visual_bundle_exists(
    const std::filesystem::path &profile_path, VisualAssetGcReport &report) {
  const auto paths = sidecar_paths(profile_path, ModuleSidecarKind::visual);
  for (const auto &path : {paths.primary, paths.backup, paths.marker}) {
    const auto read =
        read_durable_file(path, layerc::core::max_visual_json_bytes);
    if (read.status == FileReadStatus::ok ||
        read.status == FileReadStatus::too_large)
      return true;
    if (read.status == FileReadStatus::io_error) {
      report.blocked_by = path;
      report.fault = "visual sidecar location could not be inspected";
      return std::nullopt;
    }
  }
  return false;
}

bool is_lower_hex(const wchar_t character) noexcept {
  return (character >= L'0' && character <= L'9') ||
         (character >= L'a' && character <= L'f');
}

// Content-addressed stage name: 64 hex digits, ".png.", then the unique suffix.
bool is_asset_stage_filename(const std::wstring &filename) noexcept {
  return filename.size() > 64U + 5U &&
         filename.compare(64U, 5U, L".png.") == 0 &&
         std::ranges::all_of(filename.begin(), filename.begin() + 64,
                             is_lower_hex);
}

bool is_sha256_png_stem(const std::wstring &stem) noexcept {
  return stem.size() == 64 &&
         std::ranges::all_of(stem, is_lower_hex);
}

void collect_asset_references(const layerc::core::VisualProfile &profile,
                              std::vector<std::string> &references) {
  for (const auto &state : profile.templates) {
    for (const auto &reference : state.references)
      references.push_back(reference.asset_sha256);
  }
}

bool collect_active_asset_references(std::vector<std::string> &references,
                                     VisualAssetGcReport &report) {
  const auto directory = profile_directory();
  const auto scan = scan_profile_directory(directory);
  if (!scan.error.empty()) {
    report.blocked_by = directory;
    report.fault = scan.error;
    return false;
  }
  for (const auto &fault : scan.faults) {
    const auto has_visual = visual_bundle_exists(fault.path, report);
    if (!has_visual.has_value()) return false;
    if (*has_visual) {
      report.blocked_by = fault.path;
      report.fault =
          "a malformed base profile still has visual sidecar state";
      return false;
    }
  }
  for (const auto &stored : scan.profiles) {
    const auto visual = load_visual_sidecar(stored.path, stored.profile);
    if (visual.state == ModuleSidecarState::invalid) {
      report.blocked_by = visual.source_path;
      report.fault = visual.error.message.empty()
                         ? "an active visual sidecar is invalid"
                         : visual.error.message;
      return false;
    }
    if (visual.usable())
      collect_asset_references(*visual.profile, references);
    // A valid recovery copy must retain its assets even while the primary is
    // healthy. A malformed stale backup is unusable and can be ignored.
    if (visual.state == ModuleSidecarState::valid) {
      const auto paths =
          sidecar_paths(stored.path, ModuleSidecarKind::visual);
      const auto backup = read_durable_file(
          paths.backup, layerc::core::max_visual_json_bytes);
      if (backup.status == FileReadStatus::io_error) {
        report.blocked_by = paths.backup;
        report.fault = "visual recovery sidecar could not be read";
        return false;
      }
      if (backup.has_value()) {
        const std::string backup_json(
            reinterpret_cast<const char *>(backup->data()), backup->size());
        auto parsed_backup = layerc::core::parse_visual_profile_json(
            backup_json, stored.profile);
        if (const auto *backup_profile =
                std::get_if<layerc::core::VisualProfile>(&parsed_backup))
          collect_asset_references(*backup_profile, references);
      }
    }
  }
  return true;
}

std::optional<std::vector<std::uint8_t>>
read_asset_bytes(const std::filesystem::path &path) {
  const auto read = read_durable_file(path, max_template_asset_bytes);
  if (!read.has_value() || read->empty()) return std::nullopt;
  std::vector<std::uint8_t> bytes(read->size());
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = std::to_integer<std::uint8_t>((*read)[index]);
  return bytes;
}

std::variant<layerc::core::TemplateImage, VisualAssetError>
decode_png(std::vector<std::uint8_t> &bytes) {
  ComScope com;
  if (!com.usable()) return failure("COM initialization failed");
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))))
    return failure("Windows Imaging Component is unavailable");
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.ReleaseAndGetAddressOf())) ||
      FAILED(stream->InitializeFromMemory(
          bytes.data(), static_cast<DWORD>(bytes.size()))))
    return failure("PNG input stream could not be opened");
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
          decoder.ReleaseAndGetAddressOf())))
    return failure("asset is not a supported PNG image");
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf())))
    return failure("PNG has no decodable first frame");
  UINT width{};
  UINT height{};
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
      width > 512 || height > 512)
    return failure("template dimensions must be between 1 and 512 pixels");
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf())) ||
      FAILED(converter->Initialize(
          frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
          nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    return failure("PNG could not be converted to RGBA");
  const auto stride = static_cast<UINT>(width * 4U);
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(stride) * height);
  if (FAILED(converter->CopyPixels(nullptr, stride,
                                   static_cast<UINT>(rgba.size()),
                                   rgba.data())))
    return failure("PNG pixels could not be decoded");

  auto image = layerc::core::template_image_from_rgba(width, height, rgba);
  if (!image.has_value())
    return failure("template requires at least 64 nontransparent pixels");
  return std::move(*image);
}

std::variant<std::vector<std::uint8_t>, VisualAssetError>
encode_png(const layerc::core::TemplateImage &image) {
  if (!image.valid())
    return failure("template pixels or alpha mask are invalid");
  ComScope com;
  if (!com.usable()) return failure("COM initialization failed");
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))))
    return failure("Windows Imaging Component is unavailable");
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.ReleaseAndGetAddressOf())))
    return failure("PNG output stream could not be created");
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.ReleaseAndGetAddressOf())) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    return failure("PNG encoder could not be initialized");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(), properties.ReleaseAndGetAddressOf())) ||
      FAILED(frame->Initialize(properties.Get())) ||
      FAILED(frame->SetSize(image.width, image.height)))
    return failure("PNG frame could not be initialized");
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format)) ||
      format != GUID_WICPixelFormat32bppBGRA)
    return failure("PNG encoder rejected BGRA pixels");
  std::vector<std::uint8_t> bgra(image.pixels.size() * 4U);
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    bgra[index * 4U] = image.pixels[index].blue;
    bgra[index * 4U + 1U] = image.pixels[index].green;
    bgra[index * 4U + 2U] = image.pixels[index].red;
    bgra[index * 4U + 3U] = image.alpha[index];
  }
  const auto stride = image.width * 4U;
  if (FAILED(frame->WritePixels(image.height, stride,
                                static_cast<UINT>(bgra.size()),
                                bgra.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    return failure("PNG pixels could not be encoded");
  HGLOBAL memory{};
  if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)) || memory == nullptr)
    return failure("encoded PNG could not be read");
  STATSTG statistics{};
  if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME)) ||
      statistics.cbSize.HighPart != 0)
    return failure("encoded PNG size is invalid");
  const auto size = static_cast<std::size_t>(statistics.cbSize.LowPart);
  const auto *data =
      static_cast<const std::uint8_t *>(GlobalLock(memory));
  if (data == nullptr || size == 0 || size > max_template_asset_bytes) {
    if (data != nullptr) GlobalUnlock(memory);
    return failure("encoded PNG exceeds the 4 MiB asset limit");
  }
  std::vector<std::uint8_t> output(data, data + size);
  GlobalUnlock(memory);
  return output;
}

bool write_asset_atomically(const std::filesystem::path &path,
                            const std::span<const std::uint8_t> bytes) {
  if (!file_guard_ensure_directory(path.parent_path().parent_path()) ||
      !file_guard_ensure_directory(path.parent_path()) ||
      !file_guard_absent_or_regular(path)) return false;
  return write_durable_file(path, std::as_bytes(bytes)) ==
         DurablePublishOutcome::published_durable;
}

// An already-vanished file counts as collected; any other failure records
// `fault` and tells the caller to stop.
bool delete_asset_file(const std::filesystem::path &path,
                       VisualAssetGcReport &report, const char *const fault) {
  if (DeleteFileW(path.c_str()) != FALSE) {
    ++report.deleted;
    return true;
  }
  if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
  report.blocked_by = path;
  report.fault = fault;
  return false;
}

} // namespace

// The store-wide asset directory is the one the shared layout assigns to a
// profile stored directly in profile_directory().
std::filesystem::path visual_asset_directory() {
  return profile_visual_asset_directory(profile_directory() /
                                        L"asset.profile.json");
}

std::filesystem::path visual_asset_path(const std::string_view sha256_value) {
  return profile_visual_asset_path(profile_directory() / L"asset.profile.json",
                                   sha256_value);
}

VisualAssetSignature asset_signature(const std::string_view hash) {
  const auto path = visual_asset_path(hash);
  if (path.empty())
    return {};
  const HANDLE file =
      CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return {};
  BY_HANDLE_FILE_INFORMATION information{};
  const bool loaded = GetFileInformationByHandle(file, &information) != FALSE;
  if (!loaded) {
    CloseHandle(file);
    return {};
  }
  const auto combine = [](const DWORD high, const DWORD low) {
    return (static_cast<std::uint64_t>(high) << 32U) | low;
  };
  // The change-journal USN binds the signature to content history: any
  // write bumps it, so a replacement that preserves size and mtime — and
  // even one whose MFT record reuses the same file index — still produces a
  // different signature and forces the worker to re-decode. Volumes without
  // a journal keep 0 and fall back to the metadata fields.
  std::uint64_t change_usn{};
  std::array<unsigned char, 128> journal{};
  DWORD journal_bytes{};
  if (DeviceIoControl(file, FSCTL_READ_FILE_USN_DATA, nullptr, 0,
                      journal.data(), static_cast<DWORD>(journal.size()),
                      &journal_bytes, nullptr) &&
      journal_bytes >= sizeof(DWORD) + sizeof(WORD)) {
    WORD version{};
    std::memcpy(&version, journal.data() + sizeof(DWORD), sizeof(version));
    std::size_t usn_offset = std::size_t(-1);
    // V2: RecordLength(4) + versions(4) + two 8-byte file IDs = 24.
    // V3 replaces those file IDs with two 16-byte IDs, moving Usn to 40.
    // USN_RECORD is the V2-compatible alias; spell V3 manually for MinGW.
    static_assert(offsetof(USN_RECORD, Usn) == 24,
                  "USN_RECORD Usn offset changed — update V3 manual offset");
    if (version == 2)
      usn_offset = offsetof(USN_RECORD, Usn);
    else if (version == 3)
      usn_offset = sizeof(DWORD) + 2U * sizeof(WORD) +
                   2U * 2U * sizeof(std::uint64_t); // 4+4+32 = 40
    if (usn_offset != std::size_t(-1) &&
        journal_bytes >= usn_offset + sizeof(change_usn))
      std::memcpy(&change_usn, journal.data() + usn_offset, sizeof(change_usn));
  }
  CloseHandle(file);
  return {
      .present = true,
      .volume_serial = information.dwVolumeSerialNumber,
      .file_index =
          combine(information.nFileIndexHigh, information.nFileIndexLow),
      .file_size = combine(information.nFileSizeHigh, information.nFileSizeLow),
      .last_write_ticks = combine(information.ftLastWriteTime.dwHighDateTime,
                                  information.ftLastWriteTime.dwLowDateTime),
      .change_usn = change_usn};
}

TemplateAssetResult
store_template_asset(const layerc::core::TemplateImage &image) {
  if (!layerc::core::template_image_has_structure(image))
    return failure(
        "template needs visible light/color variation so its position can be recognized");
  auto encoded = encode_png(image);
  if (const auto *error = std::get_if<VisualAssetError>(&encoded))
    return *error;
  auto &bytes = std::get<std::vector<std::uint8_t>>(encoded);
  const auto digest = layerc::core::sha256(std::as_bytes(std::span(bytes)));
  const auto destination = visual_asset_path(digest);
  if (destination.empty())
    return failure("content-addressed asset path is invalid");

  bool needs_write = true;
  if (auto existing = read_asset_bytes(destination); existing.has_value()) {
    needs_write = layerc::core::sha256(std::as_bytes(std::span(*existing))) != digest;
  }
  if (needs_write && !write_asset_atomically(destination, bytes))
    return failure("template asset could not be stored atomically");

  auto stored = image;
  stored.asset_sha256 = digest;
  return ImportedTemplateAsset{
      .reference = {.asset_sha256 = digest},
      .image = std::move(stored),
      .path = destination};
}

TemplateAssetResult
import_template_asset(const std::filesystem::path &source) {
  auto bytes = read_asset_bytes(source);
  if (!bytes.has_value())
    return failure("PNG is missing, unreadable, empty, or larger than 4 MiB");
  auto decoded = decode_png(*bytes);
  if (const auto *error = std::get_if<VisualAssetError>(&decoded))
    return *error;
  return store_template_asset(
      std::get<layerc::core::TemplateImage>(std::move(decoded)));
}

TemplateImageResult load_template_asset(const std::string_view sha256_value) {
  return verified_template_from_bytes(visual_asset_path(sha256_value),
                                      sha256_value, decode_png);
}

VisualAssetGcReport garbage_collect_visual_assets(
    const std::span<const std::string> referenced_sha256) {
  VisualAssetGcReport report;
  std::set<std::string, std::less<>> referenced(referenced_sha256.begin(),
                                                 referenced_sha256.end());
  std::error_code error;
  const auto directory = visual_asset_directory();
  const bool directory_exists = std::filesystem::exists(directory, error);
  if (error) {
    report.blocked_by = directory;
    report.fault = "visual asset directory could not be inspected";
    return report;
  }
  if (!directory_exists) return report;

  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) {
      if (error) {
        report.blocked_by = iterator->path();
        report.fault = "visual asset entry could not be inspected";
        return report;
      }
      continue;
    }
    const auto path = iterator->path();
    if (_wcsicmp(path.extension().c_str(), L".tmp") == 0) {
      const auto filename = path.filename().wstring();
      if (is_asset_stage_filename(filename)) {
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) {
          report.blocked_by = path;
          report.fault = "temporary visual asset timestamp could not be read";
          return report;
        }
        if (std::filesystem::file_time_type::clock::now() - modified >=
                std::chrono::hours(24) &&
            !delete_asset_file(
                path, report,
                "stale temporary visual asset could not be deleted"))
          return report;
      }
      continue;
    }
    if (path.extension() != L".png") continue;
    const auto stem_wide = path.stem().wstring();
    if (!is_sha256_png_stem(stem_wide)) continue;
    std::string stem;
    stem.reserve(stem_wide.size());
    for (const wchar_t character : stem_wide)
      stem.push_back(static_cast<char>(character));
    if (referenced.contains(stem)) {
      ++report.retained;
      continue;
    }
    if (!delete_asset_file(path, report,
                           "unreferenced visual asset could not be deleted"))
      return report;
  }
  if (error) {
    report.blocked_by = directory;
    report.fault = "visual asset directory enumeration failed";
  }
  return report;
}

VisualAssetGcReport garbage_collect_unreferenced_visual_assets() {
  VisualAssetGcReport report;
  std::vector<std::string> references;
  // Follow the same base-profile and primary/backup sidecar recovery rules as
  // LayerD. A corrupt stale backup cannot block collection when its primary
  // is valid, while an invalid active bundle still aborts before deletion.
  if (!collect_active_asset_references(references, report)) return report;
  std::ranges::sort(references);
  const auto unique = std::ranges::unique(references);
  references.erase(unique.begin(), unique.end());
  return garbage_collect_visual_assets(references);
}

} // namespace layerc::platform
