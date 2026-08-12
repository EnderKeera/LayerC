#include "layerc/platform/visual_assets.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

namespace layerc::platform {
namespace {

template <typename T> class ComHolder {
public:
  ~ComHolder() {
    if (value_ != nullptr) value_->Release();
  }
  ComHolder() = default;
  ComHolder(const ComHolder &) = delete;
  ComHolder &operator=(const ComHolder &) = delete;
  [[nodiscard]] T *get() const noexcept { return value_; }
  [[nodiscard]] T **put() noexcept { return &value_; }
  [[nodiscard]] T *operator->() const noexcept { return value_; }

private:
  T *value_{};
};

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

bool path_exists(const std::filesystem::path &path, std::error_code &error) {
  const bool exists = std::filesystem::exists(path, error);
  return !error && exists;
}

bool visual_bundle_exists(const std::filesystem::path &profile_path) {
  const auto sidecar = visual_sidecar_path(profile_path);
  auto backup = sidecar;
  backup += L".bak";
  auto state = sidecar;
  state += L".state";
  std::error_code error;
  if (path_exists(sidecar, error)) return true;
  if (error) return true;
  if (path_exists(backup, error)) return true;
  if (error) return true;
  return path_exists(state, error) || static_cast<bool>(error);
}

void collect_asset_references(const layerc::core::VisualProfile &profile,
                              std::vector<std::string> &references) {
  for (const auto &state : profile.templates) {
    for (const auto &reference : state.references)
      references.push_back(reference.asset_sha256);
  }
}

bool collect_active_asset_references(std::vector<std::string> &references) {
  const auto directory = profile_directory();
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return !error;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) {
      if (error) return false;
      continue;
    }
    const auto path = iterator->path();
    if (_wcsicmp(path.extension().c_str(), L".json") != 0) continue;
    const auto contents = read_profile_file(path);
    if (!contents.has_value()) {
      if (visual_bundle_exists(path)) return false;
      continue;
    }
    auto parsed = layerc::core::parse_profile_json(*contents);
    const auto *profile =
        std::get_if<layerc::core::GameProfile>(&parsed);
    if (profile == nullptr) {
      if (visual_bundle_exists(path)) return false;
      continue;
    }
    const auto visual = load_visual_sidecar(path, *profile);
    if (visual.state == VisualSidecarState::invalid) return false;
    if (visual.profile.has_value())
      collect_asset_references(*visual.profile, references);
    // A valid recovery copy must retain its assets even while the primary is
    // healthy. A malformed stale backup is unusable and can be ignored.
    if (visual.state == VisualSidecarState::valid) {
      auto backup = visual_sidecar_path(path);
      backup += L".bak";
      if (const auto backup_contents = read_visual_file(backup);
          backup_contents.has_value()) {
        auto parsed_backup =
            layerc::core::parse_visual_profile_json(*backup_contents,
                                                    *profile);
        if (const auto *backup_profile =
                std::get_if<layerc::core::VisualProfile>(&parsed_backup))
          collect_asset_references(*backup_profile, references);
      }
    }
  }
  return !error;
}

std::optional<std::vector<std::uint8_t>>
read_asset_bytes(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > max_template_asset_bytes)
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
    return std::nullopt;
  return bytes;
}

std::variant<layerc::core::TemplateImage, VisualAssetError>
decode_png(std::vector<std::uint8_t> &bytes) {
  ComScope com;
  if (!com.usable()) return failure("COM initialization failed");
  ComHolder<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.put()))))
    return failure("Windows Imaging Component is unavailable");
  ComHolder<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.put())) ||
      FAILED(stream->InitializeFromMemory(
          bytes.data(), static_cast<DWORD>(bytes.size()))))
    return failure("PNG input stream could not be opened");
  ComHolder<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.get(), nullptr, WICDecodeMetadataCacheOnLoad,
          decoder.put())))
    return failure("asset is not a supported PNG image");
  ComHolder<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put())))
    return failure("PNG has no decodable first frame");
  UINT width{};
  UINT height{};
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
      width > 512 || height > 512)
    return failure("template dimensions must be between 1 and 512 pixels");
  ComHolder<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.put())) ||
      FAILED(converter->Initialize(
          frame.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
          nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    return failure("PNG could not be converted to RGBA");
  const auto stride = static_cast<UINT>(width * 4U);
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(stride) * height);
  if (FAILED(converter->CopyPixels(nullptr, stride,
                                   static_cast<UINT>(rgba.size()),
                                   rgba.data())))
    return failure("PNG pixels could not be decoded");

  layerc::core::TemplateImage image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(width) * height);
  image.alpha.resize(image.pixels.size());
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    image.pixels[index] = {rgba[index * 4U], rgba[index * 4U + 1U],
                           rgba[index * 4U + 2U]};
    image.alpha[index] = rgba[index * 4U + 3U];
  }
  if (!image.valid())
    return failure("template requires at least 64 nontransparent pixels");
  return image;
}

std::variant<std::vector<std::uint8_t>, VisualAssetError>
encode_png(const layerc::core::TemplateImage &image) {
  if (!image.valid())
    return failure("template pixels or alpha mask are invalid");
  ComScope com;
  if (!com.usable()) return failure("COM initialization failed");
  ComHolder<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.put()))))
    return failure("Windows Imaging Component is unavailable");
  ComHolder<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())))
    return failure("PNG output stream could not be created");
  ComHolder<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.put())) ||
      FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)))
    return failure("PNG encoder could not be initialized");
  ComHolder<IWICBitmapFrameEncode> frame;
  ComHolder<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(frame.put(), properties.put())) ||
      FAILED(frame->Initialize(properties.get())) ||
      FAILED(frame->SetSize(image.width, image.height)))
    return failure("PNG frame could not be initialized");
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format)) ||
      format != GUID_WICPixelFormat32bppBGRA)
    return failure("PNG encoder rejected BGRA pixels");
  std::vector<std::uint8_t> rgba(image.pixels.size() * 4U);
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    rgba[index * 4U] = image.pixels[index].blue;
    rgba[index * 4U + 1U] = image.pixels[index].green;
    rgba[index * 4U + 2U] = image.pixels[index].red;
    rgba[index * 4U + 3U] = image.alpha[index];
  }
  const auto stride = image.width * 4U;
  if (FAILED(frame->WritePixels(image.height, stride,
                                static_cast<UINT>(rgba.size()),
                                rgba.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    return failure("PNG pixels could not be encoded");
  HGLOBAL memory{};
  if (FAILED(GetHGlobalFromStream(stream.get(), &memory)) || memory == nullptr)
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
  static std::atomic_uint64_t sequence{};
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  auto temporary = path;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
               std::to_wstring(
                   sequence.fetch_add(1, std::memory_order_relaxed)) +
               L".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      (void)DeleteFileW(temporary.c_str());
      return false;
    }
  }
  if (MoveFileExW(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    (void)DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

} // namespace

std::filesystem::path visual_asset_directory() {
  return visual_directory() / L"assets";
}

std::filesystem::path visual_asset_path(const std::string_view sha256_value) {
  if (!layerc::core::is_lowercase_sha256(sha256_value)) return {};
  std::wstring filename(sha256_value.begin(), sha256_value.end());
  filename += L".png";
  return visual_asset_directory() / filename;
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
  const auto digest = layerc::core::sha256(bytes);
  const auto destination = visual_asset_path(digest);
  if (destination.empty())
    return failure("content-addressed asset path is invalid");

  bool needs_write = true;
  if (auto existing = read_asset_bytes(destination); existing.has_value()) {
    needs_write = layerc::core::sha256(*existing) != digest;
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
  if (!layerc::core::is_lowercase_sha256(sha256_value))
    return failure("template asset id is not a lower-case SHA-256 digest");
  const auto path = visual_asset_path(sha256_value);
  auto bytes = read_asset_bytes(path);
  if (!bytes.has_value())
    return failure("template asset is missing or exceeds the 4 MiB limit");
  const auto digest = layerc::core::sha256(*bytes);
  if (digest != sha256_value)
    return failure("template asset hash does not match its profile reference");
  auto decoded = decode_png(*bytes);
  if (auto *image = std::get_if<layerc::core::TemplateImage>(&decoded)) {
    if (!layerc::core::template_image_has_structure(*image))
      return failure(
          "template asset lacks enough visual structure for positional matching");
    image->asset_sha256 = std::string(sha256_value);
    return std::move(*image);
  }
  return std::get<VisualAssetError>(std::move(decoded));
}

bool garbage_collect_visual_assets(
    const std::span<const std::string> referenced_sha256) {
  std::set<std::string, std::less<>> referenced(referenced_sha256.begin(),
                                                 referenced_sha256.end());
  std::error_code error;
  const auto directory = visual_asset_directory();
  if (!std::filesystem::exists(directory, error)) return !error;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error)) {
      if (error) return false;
      continue;
    }
    const auto path = iterator->path();
    if (_wcsicmp(path.extension().c_str(), L".tmp") == 0) {
      const auto filename = path.filename().wstring();
      const bool asset_temporary =
          filename.size() > 64U + 5U &&
          filename.compare(64U, 5U, L".png.") == 0 &&
          std::ranges::all_of(
              filename.begin(), filename.begin() + 64,
              [](const wchar_t character) {
                return (character >= L'0' && character <= L'9') ||
                       (character >= L'a' && character <= L'f');
              });
      if (asset_temporary) {
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error) {
          error.clear();
          continue;
        }
        if (std::filesystem::file_time_type::clock::now() - modified >=
                std::chrono::hours(24) &&
            DeleteFileW(path.c_str()) == FALSE &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
          return false;
      }
      continue;
    }
    if (path.extension() != L".png") continue;
    const auto stem_wide = path.stem().wstring();
    if (stem_wide.size() != 64 ||
        !std::ranges::all_of(stem_wide, [](const wchar_t character) {
          return (character >= L'0' && character <= L'9') ||
                 (character >= L'a' && character <= L'f');
        }))
      continue;
    std::string stem;
    stem.reserve(stem_wide.size());
    for (const wchar_t character : stem_wide)
      stem.push_back(static_cast<char>(character));
    if (!referenced.contains(stem) &&
        DeleteFileW(path.c_str()) == FALSE &&
        GetLastError() != ERROR_FILE_NOT_FOUND)
      return false;
  }
  return !error;
}

bool garbage_collect_unreferenced_visual_assets() {
  std::vector<std::string> references;
  // Follow the same base-profile and primary/backup sidecar recovery rules as
  // LayerD. A corrupt stale backup cannot block collection when its primary
  // is valid, while an invalid active bundle still aborts before deletion.
  if (!collect_active_asset_references(references)) return false;
  std::ranges::sort(references);
  const auto unique = std::ranges::unique(references);
  references.erase(unique.begin(), unique.end());
  return garbage_collect_visual_assets(references);
}

} // namespace layerc::platform
