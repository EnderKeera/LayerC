#pragma once

// Minimal stable ABI declarations used instead of compiler-specific WinRT
// projections. Method order and IIDs come from the Windows SDK metadata.

#include <inspectable.h>
#include <unknwn.h>
#include <windows.h>

namespace layerc::capture::wgc_abi {

struct SizeInt32 {
  std::int32_t width;
  std::int32_t height;
};

struct TimeSpan {
  std::int64_t duration;
};

struct EventRegistrationToken {
  std::int64_t value;
};

struct Direct3DDevice : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Trim() = 0;
};

struct Direct3DSurface : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Description(void *value) = 0;
};

struct CaptureFrame : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Surface(Direct3DSurface **value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(TimeSpan *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32 *value) = 0;
};

struct CaptureItem : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32 *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Closed(
      IUnknown *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventRegistrationToken token) = 0;
};

struct CaptureSession : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};

// Windows 10 2004+: IGraphicsCaptureSession2.
struct CaptureSession2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE
  get_IsCursorCaptureEnabled(unsigned char *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE
  put_IsCursorCaptureEnabled(unsigned char value) = 0;
};

// Windows 11 24H2+: IGraphicsCaptureSession5.
struct CaptureSession5 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(TimeSpan *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(TimeSpan value) = 0;
};

struct CaptureFramePool : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Recreate(Direct3DDevice *device,
                                              std::int32_t pixel_format,
                                              std::int32_t number_of_buffers,
                                              SizeInt32 size) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(CaptureFrame **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(
      IUnknown *handler, EventRegistrationToken *token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(
      EventRegistrationToken token) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(
      CaptureItem *item, CaptureSession **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable **value) = 0;
};

struct CaptureFramePoolStatics2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(
      Direct3DDevice *device, std::int32_t pixel_format,
      std::int32_t number_of_buffers, SizeInt32 size,
      CaptureFramePool **result) = 0;
};

struct CaptureItemInterop : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid,
                                                     void **result) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor,
                                                      REFIID riid,
                                                      void **result) = 0;
};

struct DxgiInterfaceAccess : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **value) = 0;
};

struct Closable : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

inline constexpr GUID iid_direct3d_device{
    0xa37624ab, 0x8d5f, 0x4650, {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};
inline constexpr GUID iid_capture_frame_pool_statics2{
    0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
inline constexpr GUID iid_capture_item{
    0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
inline constexpr GUID iid_capture_session2{
    0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
inline constexpr GUID iid_capture_session5{
    0x67c0ea62, 0x1f85, 0x5061, {0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb}};
inline constexpr GUID iid_capture_item_interop{
    0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
inline constexpr GUID iid_dxgi_interface_access{
    0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
inline constexpr GUID iid_closable{
    0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};

inline constexpr std::int32_t pixel_format_bgra8_unorm = 87;

} // namespace layerc::capture::wgc_abi
