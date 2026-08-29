#include "capture_backend.hpp"
#include "d3d_capture_support.hpp"
#include "wgc_abi.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace layerc::capture {
namespace {

class WinRtRuntime {
public:
  using RoInitializeFn = HRESULT(WINAPI *)(int);
  using RoUninitializeFn = void(WINAPI *)();
  using WindowsCreateStringFn = HRESULT(WINAPI *)(PCNZWCH, UINT32, HSTRING *);
  using WindowsDeleteStringFn = HRESULT(WINAPI *)(HSTRING);
  using RoGetActivationFactoryFn = HRESULT(WINAPI *)(HSTRING, REFIID, void **);
  using CreateD3dDeviceFn = HRESULT(WINAPI *)(IDXGIDevice *, IInspectable **);

  ~WinRtRuntime() {
    if (initialized_ && ro_uninitialize_ != nullptr)
      ro_uninitialize_();
    if (d3d11_ != nullptr)
      FreeLibrary(d3d11_);
    if (combase_ != nullptr)
      FreeLibrary(combase_);
  }

  bool initialize(std::string &error) {
    combase_ = LoadLibraryW(L"combase.dll");
    d3d11_ = LoadLibraryW(L"d3d11.dll");
    if (combase_ == nullptr || d3d11_ == nullptr) {
      error = "Windows capture runtime libraries are unavailable";
      return false;
    }
    ro_initialize_ = function<RoInitializeFn>(combase_, "RoInitialize");
    ro_uninitialize_ = function<RoUninitializeFn>(combase_, "RoUninitialize");
    create_string_ =
        function<WindowsCreateStringFn>(combase_, "WindowsCreateString");
    delete_string_ =
        function<WindowsDeleteStringFn>(combase_, "WindowsDeleteString");
    get_factory_ =
        function<RoGetActivationFactoryFn>(combase_, "RoGetActivationFactory");
    create_device_ = function<CreateD3dDeviceFn>(
        d3d11_, "CreateDirect3D11DeviceFromDXGIDevice");
    if (ro_initialize_ == nullptr || ro_uninitialize_ == nullptr ||
        create_string_ == nullptr || delete_string_ == nullptr ||
        get_factory_ == nullptr || create_device_ == nullptr) {
      error = "Windows capture runtime entry points are unavailable";
      return false;
    }
    const auto initialized = ro_initialize_(1); // RO_INIT_MULTITHREADED
    if (FAILED(initialized)) {
      error = "WinRT initialization failed: " + hresult_message(initialized);
      return false;
    }
    initialized_ = true;
    return true;
  }

  HRESULT create_string(const wchar_t *value, HSTRING *output) const {
    return create_string_(value, static_cast<UINT32>(wcslen(value)), output);
  }
  void delete_string(HSTRING value) const {
    if (value != nullptr)
      (void)delete_string_(value);
  }
  HRESULT get_factory(HSTRING name, REFIID iid, void **output) const {
    return get_factory_(name, iid, output);
  }
  HRESULT create_d3d_device(IDXGIDevice *device, IInspectable **output) const {
    return create_device_(device, output);
  }

private:
  template <typename T> static T function(HMODULE module, const char *name) {
    const FARPROC procedure = GetProcAddress(module, name);
    static_assert(sizeof(T) == sizeof(procedure));
    T function{};
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
  }

  HMODULE combase_{};
  HMODULE d3d11_{};
  RoInitializeFn ro_initialize_{};
  RoUninitializeFn ro_uninitialize_{};
  WindowsCreateStringFn create_string_{};
  WindowsDeleteStringFn delete_string_{};
  RoGetActivationFactoryFn get_factory_{};
  CreateD3dDeviceFn create_device_{};
  bool initialized_{};
};

template <typename T> void close_inspectable(ComPtr<T> &value) {
  if (!value)
    return;
  ComPtr<wgc_abi::Closable> closable;
  if (SUCCEEDED(
          value->QueryInterface(wgc_abi::iid_closable, &closable)))
    (void)closable->Close();
  value.Reset();
}

void pin_graphics_capture_runtime() {
  // Free-threaded frame pools may finish internal callbacks after Close has
  // returned. Some Windows builds otherwise unload GraphicsCapture.dll while
  // one of those callbacks is still retiring, producing an execute AV in
  // GraphicsCapture.dll_unloaded when WGC falls back or the worker stops.
  // Pinning the already-loaded system module preserves normal object teardown
  // while keeping its callback code valid until process exit.
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE module{};
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,
                             L"GraphicsCapture.dll", &module);
  });
}

class WgcBackend final : public CaptureBackend {
public:
  explicit WgcBackend(const HWND window) : window_(window) {}
  ~WgcBackend() override { stop(); }

  bool start() override {
    stop();
    if (!IsWindow(window_)) {
      error_ = "capture target is no longer available";
      return false;
    }
    runtime_ = std::make_unique<WinRtRuntime>();
    if (!runtime_->initialize(error_) ||
        !create_hardware_device(nullptr, d3d_, error_)) {
      stop();
      return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    auto result = d3d_.device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    ComPtr<IInspectable> inspectable_device;
    if (SUCCEEDED(result))
      result = runtime_->create_d3d_device(dxgi_device.Get(),
                                           inspectable_device.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result))
      result = inspectable_device->QueryInterface(wgc_abi::iid_direct3d_device,
                                                  &direct3d_device_);
    if (FAILED(result))
      return fail("D3D11 WinRT interop failed", result);

    HSTRING item_class{};
    result = runtime_->create_string(
        L"Windows.Graphics.Capture.GraphicsCaptureItem", &item_class);
    ComPtr<wgc_abi::CaptureItemInterop> item_factory;
    if (SUCCEEDED(result))
      result =
          runtime_->get_factory(item_class, wgc_abi::iid_capture_item_interop,
                                &item_factory);
    runtime_->delete_string(item_class);
    if (SUCCEEDED(result))
      result = item_factory->CreateForWindow(window_, wgc_abi::iid_capture_item,
                                             &item_);
    if (FAILED(result))
      return fail("WGC window item creation failed", result);

    wgc_abi::SizeInt32 size{};
    result = item_->get_Size(&size);
    if (FAILED(result) || size.width <= 0 || size.height <= 0)
      return fail("WGC target size is unavailable", result);

    HSTRING pool_class{};
    result = runtime_->create_string(
        L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", &pool_class);
    ComPtr<wgc_abi::CaptureFramePoolStatics2> pool_factory;
    if (SUCCEEDED(result))
      result = runtime_->get_factory(pool_class,
                                     wgc_abi::iid_capture_frame_pool_statics2,
                                     &pool_factory);
    runtime_->delete_string(pool_class);
    if (SUCCEEDED(result))
      // LayerC consumes only the newest frame at 5-15 Hz. One pool buffer is
      // sufficient for that drop-old-frames contract and avoids retaining a
      // second full-size game texture in the daemon's capture session.
      result = pool_factory->CreateFreeThreaded(
          direct3d_device_.Get(), wgc_abi::pixel_format_bgra8_unorm, 1, size,
          pool_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result))
      result = pool_->CreateCaptureSession(item_.Get(), session_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result))
      result = session_->StartCapture();
    if (FAILED(result))
      return fail("WGC session start failed", result);
    pin_graphics_capture_runtime();

    pool_size_ = size;
    started_ = true;
    error_.clear();
    return true;
  }

  void stop() override {
    started_ = false;
    close_inspectable(session_);
    close_inspectable(pool_);
    item_.Reset();
    direct3d_device_.Reset();
    readback_.reset();
    last_frame_.reset();
    d3d_ = {};
    runtime_.reset();
    black_frames_ = 0;
    pool_size_ = {};
  }

  CapturePollResult poll(const core::TimePoint now, bool) override {
    if (!started_ || !IsWindow(window_))
      return {.availability = core::CaptureAvailability::unavailable,
              .backend = kind(),
              .diagnostic =
                  IsWindow(window_) ? error_ : "capture target closed",
              .frame = {}};

    ComPtr<wgc_abi::CaptureFrame> newest;
    for (;;) {
      ComPtr<wgc_abi::CaptureFrame> frame;
      const auto result = pool_->TryGetNextFrame(frame.ReleaseAndGetAddressOf());
      if (FAILED(result)) {
        error_ = "WGC frame retrieval failed: " + hresult_message(result);
        return {.availability = core::CaptureAvailability::unavailable,
                .backend = kind(),
                .diagnostic = error_,
                .frame = {}};
      }
      if (!frame)
        break;
      newest = std::move(frame);
    }

    if (newest) {
      ComPtr<wgc_abi::Direct3DSurface> surface;
      ComPtr<wgc_abi::DxgiInterfaceAccess> access;
      ComPtr<ID3D11Texture2D> texture;
      wgc_abi::SizeInt32 content{};
      auto result = newest->get_Surface(surface.ReleaseAndGetAddressOf());
      if (SUCCEEDED(result))
        result = surface->QueryInterface(wgc_abi::iid_dxgi_interface_access,
                                         &access);
      if (SUCCEEDED(result))
        result = access->GetInterface(IID_PPV_ARGS(&texture));
      if (SUCCEEDED(result))
        result = newest->get_ContentSize(&content);
      if (FAILED(result) || content.width <= 0 || content.height <= 0) {
        error_ = "WGC frame surface failed: " + hresult_message(result);
        return {.availability = core::CaptureAvailability::unavailable,
                .backend = kind(),
                .diagnostic = error_,
                .frame = {}};
      }
      if (content.width != pool_size_.width ||
          content.height != pool_size_.height) {
        texture.Reset();
        access.Reset();
        surface.Reset();
        newest.Reset();
        readback_.reset();
        const auto resized =
            pool_->Recreate(direct3d_device_.Get(),
                            wgc_abi::pixel_format_bgra8_unorm, 1, content);
        if (FAILED(resized)) {
          error_ = "WGC frame-pool resize failed: " + hresult_message(resized);
          return {.availability = core::CaptureAvailability::unavailable,
                  .backend = kind(),
                  .diagnostic = error_,
                  .frame = {}};
        }
        pool_size_ = content;
        last_frame_.reset();
        black_frames_ = 0;
        return {.availability = core::CaptureAvailability::starting,
                .backend = kind(),
                .diagnostic = "WGC target size changed; frame pool recreated",
                .frame = {}};
      }

      D3D11_TEXTURE2D_DESC description{};
      texture->GetDesc(&description);
      const auto width = std::min<std::uint32_t>(
          description.Width, static_cast<std::uint32_t>(content.width));
      const auto height = std::min<std::uint32_t>(
          description.Height, static_cast<std::uint32_t>(content.height));
      const D3D11_BOX source{0, 0, 0, width, height, 1};
      auto captured =
          readback_.read(d3d_, texture.Get(), source, width, height, 0, 0,
                         {0.0, 0.0, 1.0, 1.0}, OutputRotation::identity,
                         quality_, now, error_);
      if (!captured)
        return {.availability = core::CaptureAvailability::unavailable,
                .backend = kind(),
                .diagnostic = error_,
                .frame = {}};
      if (frame_is_nearly_black(*captured))
        ++black_frames_;
      else
        black_frames_ = 0;
      last_frame_ = std::move(captured);
    }

    const auto availability =
        black_frames_ >= 30 ? core::CaptureAvailability::protected_or_black
        : last_frame_       ? core::CaptureAvailability::available
                            : core::CaptureAvailability::starting;
    return {.availability = availability,
            .backend = kind(),
            .diagnostic =
                availability == core::CaptureAvailability::protected_or_black
                    ? "WGC returned persistent black frames"
                    : std::string{},
            .frame = last_frame_};
  }

  void set_excluded_screen_rects(std::span<const RECT>) override {}
  void set_quality(const core::CaptureQuality quality) override {
    if (quality_ == quality)
      return;
    quality_ = quality;
    last_frame_.reset();
    black_frames_ = 0;
  }
  core::CaptureBackendKind kind() const noexcept override {
    return core::CaptureBackendKind::windows_graphics_capture;
  }
  std::string gpu_description() const override { return d3d_.description; }
  std::string last_error() const override { return error_; }

private:
  bool fail(const char *prefix, const HRESULT result) {
    error_ = std::string(prefix) + ": " + hresult_message(result);
    stop();
    return false;
  }

  HWND window_{};
  std::unique_ptr<WinRtRuntime> runtime_;
  D3dDevice d3d_;
  ComPtr<wgc_abi::Direct3DDevice> direct3d_device_;
  ComPtr<wgc_abi::CaptureItem> item_;
  ComPtr<wgc_abi::CaptureFramePool> pool_;
  ComPtr<wgc_abi::CaptureSession> session_;
  TextureReadback readback_;
  std::shared_ptr<CaptureFrame> last_frame_;
  std::string error_;
  std::uint32_t black_frames_{};
  wgc_abi::SizeInt32 pool_size_{};
  core::CaptureQuality quality_{core::CaptureQuality::balanced};
  bool started_{};
};

} // namespace

std::unique_ptr<CaptureBackend> make_wgc_backend(const HWND window) {
  return std::make_unique<WgcBackend>(window);
}

} // namespace layerc::capture
