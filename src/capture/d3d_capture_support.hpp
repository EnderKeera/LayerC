#pragma once

#include "layerc/capture/window_capture.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <string>
#include <utility>

namespace layerc::capture {

template <typename T> class ComPtr {
public:
  ComPtr() = default;
  explicit ComPtr(T *value) : value_(value) {}
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr &other) : value_(other.value_) {
    if (value_ != nullptr) value_->AddRef();
  }
  ComPtr &operator=(const ComPtr &other) {
    if (this == &other) return *this;
    reset();
    value_ = other.value_;
    if (value_ != nullptr) value_->AddRef();
    return *this;
  }
  ComPtr(ComPtr &&other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  ComPtr &operator=(ComPtr &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] T *get() const noexcept { return value_; }
  [[nodiscard]] T **put() noexcept {
    reset();
    return &value_;
  }
  [[nodiscard]] void **put_void() noexcept {
    return reinterpret_cast<void **>(put());
  }
  [[nodiscard]] T *operator->() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }
  void reset(T *replacement = nullptr) noexcept {
    if (value_ != nullptr) value_->Release();
    value_ = replacement;
  }

private:
  T *value_{};
};

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
