#include "capture_backend.hpp"
#include "d3d_capture_support.hpp"
#include "wgc_abi.hpp"

#include <algorithm>
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

  template <typename T>
  HRESULT get_factory(const wchar_t *class_name, REFIID iid,
                      ComPtr<T> &output) const {
    HSTRING name{};
    auto result = create_string_(
        class_name, static_cast<UINT32>(wcslen(class_name)), &name);
    if (SUCCEEDED(result))
      result = get_factory_(name, iid, &output);
    if (name != nullptr)
      (void)delete_string_(name);
    return result;
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
      slot_.set_error("capture target is no longer available");
      return false;
    }
    runtime_ = std::make_unique<WinRtRuntime>();
    if (std::string error; !runtime_->initialize(error) ||
                           !create_hardware_device(nullptr, d3d_, error)) {
      slot_.set_error(std::move(error));
      stop();
      return false;
    }
    // A refusal returns only after open_session() has released its WinRT
    // temporaries: fail() uninitializes the runtime, and the last
    // RoUninitialize can unload GraphicsCapture.dll under a held factory.
    if (const auto opened = open_session(); opened.failure != nullptr)
      return fail(opened.failure, opened.result);
    pin_graphics_capture_runtime();

    slot_.set_started(true);
    slot_.clear_error();
    return true;
  }

  void stop() override {
    slot_.set_started(false);
    close_inspectable(session_);
    close_inspectable(pool_);
    item_.Reset();
    direct3d_device_.Reset();
    readback_.reset();
    slot_.clear_frame();
    d3d_ = {};
    runtime_.reset();
    pool_size_ = {};
  }

  CapturePollResult poll(const core::TimePoint now, bool) override {
    if (!slot_.started() || !IsWindow(window_))
      return slot_.unavailable(IsWindow(window_) ? slot_.last_error() : "capture target closed");

    ComPtr<wgc_abi::CaptureFrame> newest;
    for (;;) {
      ComPtr<wgc_abi::CaptureFrame> frame;
      const auto result = pool_->TryGetNextFrame(frame.ReleaseAndGetAddressOf());
      if (FAILED(result)) {
        slot_.set_error("WGC frame retrieval failed: " + hresult_message(result));
        return slot_.unavailable(slot_.last_error());
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
      if (FAILED(result)) {
        slot_.set_error("WGC frame surface failed: " + hresult_message(result));
        return slot_.unavailable(slot_.last_error());
      }
      if (content.width <= 0 || content.height <= 0) {
        slot_.set_error("WGC frame has no content size");
        return slot_.unavailable(slot_.last_error());
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
          slot_.set_error("WGC frame-pool resize failed: " + hresult_message(resized));
          return slot_.unavailable(slot_.last_error());
        }
        pool_size_ = content;
        slot_.clear_frame();
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
      std::string error;
      auto captured =
          readback_.read(d3d_, texture.Get(), source, width, height, 0, 0,
                         {0.0, 0.0, 1.0, 1.0}, OutputRotation::identity,
                         slot_.quality(), now, error);
      if (!captured) {
        slot_.set_error(std::move(error));
        return slot_.unavailable(slot_.last_error());
      }
      slot_.publish(std::move(captured));
    }

    return slot_.current();
  }

  void set_excluded_screen_rects(std::span<const RECT>) override {}
  void set_quality(const core::CaptureQuality quality) override {
    slot_.set_quality(quality);
  }
  core::CaptureBackendKind kind() const noexcept override {
    return core::CaptureBackendKind::windows_graphics_capture;
  }
  std::string gpu_description() const override { return d3d_.description; }
  std::string last_error() const override {
    return slot_.last_error();
  }

private:
  struct OpenResult {
    const char *failure{};
    HRESULT result{S_OK};
  };

  // Creates the item, frame pool and session. Every WinRT temporary is
  // local here, so each is released before the caller can tear down the
  // runtime; only the members that stop() releases outlive this call.
  OpenResult open_session() {
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
      return {"D3D11 WinRT interop failed", result};

    ComPtr<wgc_abi::CaptureItemInterop> item_factory;
    result = runtime_->get_factory(
        L"Windows.Graphics.Capture.GraphicsCaptureItem",
        wgc_abi::iid_capture_item_interop, item_factory);
    if (SUCCEEDED(result))
      result = item_factory->CreateForWindow(window_, wgc_abi::iid_capture_item,
                                             &item_);
    if (FAILED(result))
      return {"WGC window item creation failed", result};

    wgc_abi::SizeInt32 size{};
    result = item_->get_Size(&size);
    if (FAILED(result))
      return {"WGC target size is unavailable", result};
    if (size.width <= 0 || size.height <= 0)
      return {"WGC target window has no capturable size"};

    ComPtr<wgc_abi::CaptureFramePoolStatics2> pool_factory;
    result = runtime_->get_factory(
        L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",
        wgc_abi::iid_capture_frame_pool_statics2, pool_factory);
    if (SUCCEEDED(result))
      // LayerC consumes only the newest frame at 5-15 Hz. One pool buffer is
      // sufficient for that drop-old-frames contract and avoids retaining a
      // second full-size game texture in the daemon's capture session.
      result = pool_factory->CreateFreeThreaded(
          direct3d_device_.Get(), wgc_abi::pixel_format_bgra8_unorm, 1, size,
          pool_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result))
      result = pool_->CreateCaptureSession(item_.Get(), session_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) {
      limit_session_updates();
      result = session_->StartCapture();
    }
    if (FAILED(result))
      return {"WGC session start failed", result};
    pool_size_ = size;
    return {};
  }

  // Optional session settings; older Windows builds lack the interfaces and
  // keep their defaults. The cursor is not part of what rules match, and the
  // worker samples at most 15 Hz, so copying every game frame is wasted work.
  void limit_session_updates() {
    ComPtr<wgc_abi::CaptureSession2> cursor_session;
    if (SUCCEEDED(session_->QueryInterface(wgc_abi::iid_capture_session2,
                                           &cursor_session)))
      (void)cursor_session->put_IsCursorCaptureEnabled(0);
    ComPtr<wgc_abi::CaptureSession5> interval_session;
    if (SUCCEEDED(session_->QueryInterface(wgc_abi::iid_capture_session5,
                                           &interval_session)))
      // TimeSpan counts 100 ns ticks; 1/30 s stays under the 1/15 s sample
      // period so the newest frame is never older than half a sample.
      (void)interval_session->put_MinUpdateInterval(
          {.duration = 10'000'000 / 30});
  }

  bool fail(const char *prefix, const HRESULT result) {
    // A successful HRESULT has no system text to append.
    slot_.set_error(SUCCEEDED(result)
                        ? std::string(prefix)
                        : std::string(prefix) + ": " + hresult_message(result));
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
  BackendFrameSlot slot_{core::CaptureBackendKind::windows_graphics_capture};
  wgc_abi::SizeInt32 pool_size_{};
};

} // namespace

std::unique_ptr<CaptureBackend> make_wgc_backend(const HWND window) {
  return std::make_unique<WgcBackend>(window);
}

} // namespace layerc::capture
