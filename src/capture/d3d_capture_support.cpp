#include "d3d_capture_support.hpp"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <system_error>

namespace layerc::capture {
namespace {

float half_to_float(const std::uint16_t value) {
  const auto sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
  auto exponent = static_cast<int>((value >> 10U) & 0x1FU);
  auto mantissa = static_cast<std::uint32_t>(value & 0x03FFU);
  std::uint32_t bits{};
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x0400U) == 0) {
        mantissa <<= 1U;
        --exponent;
      }
      mantissa &= 0x03FFU;
      bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) |
             (mantissa << 13U);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000U | (mantissa << 13U);
  } else {
    bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) |
           (mantissa << 13U);
  }
  float result{};
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

std::uint8_t tone_map_channel(const float linear) {
  const auto positive = std::max(0.0F, linear);
  const auto mapped = positive / (1.0F + positive);
  const auto srgb = mapped <= 0.0031308F
                        ? 12.92F * mapped
                        : 1.055F * std::pow(mapped, 1.0F / 2.4F) - 0.055F;
  return static_cast<std::uint8_t>(
      std::clamp(std::lround(srgb * 255.0F), 0L, 255L));
}

std::uint32_t sampled_coordinate(const std::uint32_t index,
                                 const std::uint32_t source_extent,
                                 const std::uint32_t output_extent) {
  return std::min<std::uint32_t>(
      source_extent - 1U,
      static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(index) * source_extent +
           source_extent / 2U) /
          output_extent));
}

std::optional<core::RgbPixel> decode_pixel(const DXGI_FORMAT format,
                                           const std::byte *row,
                                           const std::uint32_t source_x) {
  if (format == DXGI_FORMAT_B8G8R8A8_UNORM ||
      format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
    const auto *value = reinterpret_cast<const std::uint8_t *>(row) +
                        static_cast<std::size_t>(source_x) * 4U;
    return core::RgbPixel{value[2], value[1], value[0]};
  }
  if (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
      format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
    const auto *value = reinterpret_cast<const std::uint8_t *>(row) +
                        static_cast<std::size_t>(source_x) * 4U;
    return core::RgbPixel{value[0], value[1], value[2]};
  }
  if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    const auto *value = reinterpret_cast<const std::uint16_t *>(row) +
                        static_cast<std::size_t>(source_x) * 4U;
    return core::RgbPixel{tone_map_channel(half_to_float(value[0])),
                          tone_map_channel(half_to_float(value[1])),
                          tone_map_channel(half_to_float(value[2]))};
  }
  return std::nullopt;
}

} // namespace

bool create_hardware_device(IDXGIAdapter *adapter, D3dDevice &output,
                            std::string &error) {
  output = {};
  constexpr D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1,
                                           D3D_FEATURE_LEVEL_11_0,
                                           D3D_FEATURE_LEVEL_10_1,
                                           D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL selected{};
  const auto driver = adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE
                                         : D3D_DRIVER_TYPE_UNKNOWN;
  const auto result = D3D11CreateDevice(
      adapter, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested,
      static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
      output.device.ReleaseAndGetAddressOf(), &selected, output.context.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    error = "hardware D3D11 device creation failed: " +
            hresult_message(result);
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  if (SUCCEEDED(output.device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
    ComPtr<IDXGIAdapter> actual_adapter;
    if (SUCCEEDED(dxgi_device->GetAdapter(actual_adapter.ReleaseAndGetAddressOf()))) {
      DXGI_ADAPTER_DESC description{};
      if (SUCCEEDED(actual_adapter->GetDesc(&description)))
        output.description =
            layerc::platform::wide_to_utf8(description.Description);
    }
  }
  if (output.description.empty()) output.description = "hardware D3D11 adapter";
  return true;
}

std::shared_ptr<CaptureFrame> TextureReadback::read(
    D3dDevice &device, ID3D11Texture2D *texture, const D3D11_BOX &source,
    const std::uint32_t destination_width,
    const std::uint32_t destination_height,
    const std::uint32_t destination_x, const std::uint32_t destination_y,
    const core::NormalizedRegion &available_region,
    const OutputRotation rotation, const core::CaptureQuality quality,
    const core::TimePoint captured_at,
    std::string &error) {
  if (texture == nullptr || source.right <= source.left ||
      source.bottom <= source.top || destination_width == 0 ||
      destination_height == 0)
    return {};
  D3D11_TEXTURE2D_DESC source_description{};
  texture->GetDesc(&source_description);
  if (source.right > source_description.Width ||
      source.bottom > source_description.Height) {
    error = "capture texture bounds changed unexpectedly";
    return {};
  }
  const auto copy_width = source.right - source.left;
  const auto copy_height = source.bottom - source.top;
  const bool swaps_axes = rotation == OutputRotation::rotate90 ||
                          rotation == OutputRotation::rotate270;
  const auto logical_copy_width = swaps_axes ? copy_height : copy_width;
  const auto logical_copy_height = swaps_axes ? copy_width : copy_height;
  const auto [output_width, output_height] = capture_sample_dimensions(
      destination_width, destination_height, quality);
  const bool bounded_readback = output_width != destination_width ||
                                output_height != destination_height;
  const bool copy_columns =
      bounded_readback &&
      (rotation == OutputRotation::rotate90 ||
       rotation == OutputRotation::rotate270);
  auto staging_width = copy_width;
  auto staging_height = copy_height;
  if (bounded_readback) {
    if (copy_columns)
      staging_width = output_height;
    else
      staging_height = output_height;
  }

  if (!staging_ || staging_description_.Width != staging_width ||
      staging_description_.Height != staging_height ||
      staging_description_.Format != source_description.Format) {
    reset();
    staging_description_ = source_description;
    staging_description_.Width = staging_width;
    staging_description_.Height = staging_height;
    staging_description_.BindFlags = 0;
    staging_description_.MiscFlags = 0;
    staging_description_.Usage = D3D11_USAGE_STAGING;
    staging_description_.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_description_.MipLevels = 1;
    staging_description_.ArraySize = 1;
    const auto created =
        device.device->CreateTexture2D(&staging_description_, nullptr,
                                       staging_.ReleaseAndGetAddressOf());
    if (FAILED(created)) {
      error = "capture readback texture creation failed: " +
              hresult_message(created);
      return {};
    }
  }

  if (!bounded_readback) {
    device.context->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0,
                                          texture, 0, &source);
  } else {
    // A CPU-readable full-window texture dominates the Low/Balanced memory
    // budget. Copy only the source rows (or rotated columns) represented by
    // the bounded output grid, then map the compact strip texture once.
    for (std::uint32_t output_y = 0; output_y < output_height; ++output_y) {
      const auto original_y = sampled_coordinate(
          output_y, destination_height, output_height);
      if (original_y < destination_y ||
          original_y >= destination_y + logical_copy_height)
        continue;
      const auto logical_y = original_y - destination_y;
      if (copy_columns) {
        const auto source_x =
            rotation == OutputRotation::rotate90
                ? logical_y
                : logical_copy_height - 1U - logical_y;
        const D3D11_BOX column{source.left + source_x, source.top, 0,
                               source.left + source_x + 1U, source.bottom, 1};
        device.context->CopySubresourceRegion(
            staging_.Get(), 0, output_y, 0, 0, texture, 0, &column);
      } else {
        const auto source_y =
            rotation == OutputRotation::rotate180
                ? logical_copy_height - 1U - logical_y
                : logical_y;
        const D3D11_BOX row{source.left, source.top + source_y, 0,
                            source.right, source.top + source_y + 1U, 1};
        device.context->CopySubresourceRegion(
            staging_.Get(), 0, 0, output_y, 0, texture, 0, &row);
      }
    }
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  const auto mapped_result =
      device.context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(mapped_result)) {
    error = "capture readback map failed: " + hresult_message(mapped_result);
    return {};
  }

  auto output = std::make_shared<CaptureFrame>();
  output->width = output_width;
  output->height = output_height;
  output->available_region = available_region;
  output->captured_at = captured_at;
  output->pixels.assign(static_cast<std::size_t>(output_width) * output_height,
                        core::RgbPixel{});
  for (std::uint32_t output_y = 0; output_y < output_height; ++output_y) {
    const auto original_y = sampled_coordinate(
        output_y, destination_height, output_height);
    if (original_y < destination_y ||
        original_y >= destination_y + logical_copy_height)
      continue;
    for (std::uint32_t output_x = 0; output_x < output_width; ++output_x) {
      const auto original_x = sampled_coordinate(
          output_x, destination_width, output_width);
      if (original_x < destination_x ||
          original_x >= destination_x + logical_copy_width)
        continue;
      const auto logical_x = original_x - destination_x;
      const auto logical_y = original_y - destination_y;
      std::uint32_t source_x{};
      std::uint32_t source_y{};
      switch (rotation) {
      case OutputRotation::rotate90:
        source_x = logical_y;
        source_y = logical_copy_width - 1U - logical_x;
        break;
      case OutputRotation::rotate180:
        source_x = logical_copy_width - 1U - logical_x;
        source_y = logical_copy_height - 1U - logical_y;
        break;
      case OutputRotation::rotate270:
        source_x = logical_copy_height - 1U - logical_y;
        source_y = logical_x;
        break;
      case OutputRotation::identity:
      default:
        source_x = logical_x;
        source_y = logical_y;
        break;
      }
      if (bounded_readback) {
        if (copy_columns)
          source_x = output_y;
        else
          source_y = output_y;
      }
      const auto *row = static_cast<const std::byte *>(mapped.pData) +
                        static_cast<std::size_t>(source_y) * mapped.RowPitch;
      const auto pixel =
          decode_pixel(source_description.Format, row, source_x);
      if (!pixel) {
        device.context->Unmap(staging_.Get(), 0);
        error = "capture texture format is unsupported";
        return {};
      }
      output->pixels[static_cast<std::size_t>(output_y) * output_width +
                     output_x] = *pixel;
    }
  }
  device.context->Unmap(staging_.Get(), 0);
  return output;
}

void TextureReadback::reset() {
  staging_.Reset();
  staging_description_ = {};
}

} // namespace layerc::capture
