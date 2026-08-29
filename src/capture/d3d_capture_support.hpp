#pragma once

#include "layerc/capture/window_capture.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace layerc::capture {

using Microsoft::WRL::ComPtr;

struct D3dDevice {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  std::string description;
};

[[nodiscard]] bool create_hardware_device(IDXGIAdapter *adapter,
                                          D3dDevice &output,
                                          std::string &error);

class TextureReadback {
public:
  [[nodiscard]] std::shared_ptr<CaptureFrame>
  read(D3dDevice &device, ID3D11Texture2D *texture, const D3D11_BOX &source,
       std::uint32_t destination_width, std::uint32_t destination_height,
       std::uint32_t destination_x, std::uint32_t destination_y,
       const core::NormalizedRegion &available_region,
       OutputRotation rotation, core::CaptureQuality quality,
       core::TimePoint captured_at,
       std::string &error);
  void reset();

private:
  ComPtr<ID3D11Texture2D> staging_;
  D3D11_TEXTURE2D_DESC staging_description_{};
};

[[nodiscard]] std::string hresult_message(HRESULT result);

} // namespace layerc::capture
