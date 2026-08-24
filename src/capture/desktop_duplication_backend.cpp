#include "capture_backend.hpp"
#include "d3d_capture_support.hpp"

#include <algorithm>
#include <vector>

namespace layerc::capture {
namespace {

constexpr std::string_view fallback_warning =
    "Desktop Duplication fallback is active; capture is limited to the "
    "foreground window's largest-intersection monitor and third-party "
    "overlays may affect pixel rules";

RECT intersect_rect(const RECT &left, const RECT &right) {
  RECT result{std::max(left.left, right.left), std::max(left.top, right.top),
              std::min(left.right, right.right),
              std::min(left.bottom, right.bottom)};
  if (result.right < result.left)
    result.right = result.left;
  if (result.bottom < result.top)
    result.bottom = result.top;
  return result;
}

OutputRotation output_rotation(const DXGI_MODE_ROTATION rotation) {
  switch (rotation) {
  case DXGI_MODE_ROTATION_ROTATE90:
    return OutputRotation::rotate90;
  case DXGI_MODE_ROTATION_ROTATE180:
    return OutputRotation::rotate180;
  case DXGI_MODE_ROTATION_ROTATE270:
    return OutputRotation::rotate270;
  case DXGI_MODE_ROTATION_IDENTITY:
  case DXGI_MODE_ROTATION_UNSPECIFIED:
  default:
    return OutputRotation::identity;
  }
}

class DesktopDuplicationBackend final : public CaptureBackend {
public:
  explicit DesktopDuplicationBackend(const HWND window) : window_(window) {}
  ~DesktopDuplicationBackend() override { stop(); }

  bool start() override {
    stop();
    RECT window_bounds{};
    if (!IsWindow(window_) || !capture_window_bounds(window_, window_bounds)) {
      error_ = "capture target is no longer available";
      return false;
    }
    monitor_ = MonitorFromRect(&window_bounds, MONITOR_DEFAULTTONEAREST);

    ComPtr<IDXGIFactory1> factory;
    auto result = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(result))
      return fail("DXGI factory creation failed", result);
    for (UINT adapter_index = 0; !output_; ++adapter_index) {
      ComPtr<IDXGIAdapter1> candidate_adapter;
      result = factory->EnumAdapters1(adapter_index, candidate_adapter.put());
      if (result == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(result))
        continue;
      for (UINT output_index = 0;; ++output_index) {
        ComPtr<IDXGIOutput> candidate_output;
        result = candidate_adapter->EnumOutputs(output_index,
                                                candidate_output.put());
        if (result == DXGI_ERROR_NOT_FOUND)
          break;
        if (FAILED(result))
          continue;
        DXGI_OUTPUT_DESC description{};
        if (SUCCEEDED(candidate_output->GetDesc(&description)) &&
            description.Monitor == monitor_) {
          adapter_ = std::move(candidate_adapter);
          output_ = std::move(candidate_output);
          output_bounds_ = description.DesktopCoordinates;
          output_rotation_ = output_rotation(description.Rotation);
          break;
        }
      }
    }
    if (!adapter_ || !output_) {
      error_ = "the target monitor has no DXGI output";
      stop_resources();
      return false;
    }
    if (!create_hardware_device(adapter_.get(), d3d_, error_)) {
      stop_resources();
      return false;
    }
    ComPtr<IDXGIOutput1> output1;
    result = output_->QueryInterface(IID_PPV_ARGS(output1.put()));
    if (SUCCEEDED(result))
      result = output1->DuplicateOutput(d3d_.device.get(), duplication_.put());
    if (FAILED(result))
      return fail("Desktop Duplication start failed", result);
    started_ = true;
    error_.clear();
    return true;
  }

  void stop() override {
    started_ = false;
    last_frame_.reset();
    black_frames_ = 0;
    stop_resources();
  }

  CapturePollResult poll(const core::TimePoint now,
                         const bool foreground) override {
    if (!started_ || !IsWindow(window_))
      return unavailable(IsWindow(window_) ? error_ : "capture target closed");
    if (!foreground)
      return {.availability = core::CaptureAvailability::inactive,
              .backend = kind(),
              .diagnostic = std::string(fallback_warning),
              .frame = {}};

    RECT window_bounds{};
    if (!capture_window_bounds(window_, window_bounds))
      return unavailable("capture target bounds are unavailable");
    const auto monitor =
        MonitorFromRect(&window_bounds, MONITOR_DEFAULTTONEAREST);
    if (monitor != monitor_) {
      if (!start())
        return unavailable(error_);
      if (!capture_window_bounds(window_, window_bounds))
        return unavailable("capture target bounds are unavailable");
    }

    DXGI_OUTDUPL_FRAME_INFO information{};
    ComPtr<IDXGIResource> resource;
    auto result =
        duplication_->AcquireNextFrame(0, &information, resource.put());
    if (result == DXGI_ERROR_WAIT_TIMEOUT)
      return current_result();
    if (result == DXGI_ERROR_ACCESS_LOST) {
      if (!start())
        return unavailable(error_);
      return {.availability = core::CaptureAvailability::starting,
              .backend = kind(),
              .diagnostic = std::string(fallback_warning),
              .frame = {}};
    }
    if (FAILED(result)) {
      const auto failure =
          "Desktop Duplication frame failed: " + hresult_message(result);
      if (!start())
        return unavailable(failure + "; restart failed: " + error_);
      return {.availability = core::CaptureAvailability::starting,
              .backend = kind(),
              .diagnostic = failure + "; capture session restarted; " +
                            std::string(fallback_warning),
              .frame = {}};
    }

    ComPtr<ID3D11Texture2D> texture;
    result = resource->QueryInterface(IID_PPV_ARGS(texture.put()));
    if (FAILED(result)) {
      (void)duplication_->ReleaseFrame();
      return unavailable("Desktop Duplication texture failed: " +
                         hresult_message(result));
    }

    const auto intersection = intersect_rect(window_bounds, output_bounds_);
    const auto window_width =
        static_cast<std::uint32_t>(window_bounds.right - window_bounds.left);
    const auto window_height =
        static_cast<std::uint32_t>(window_bounds.bottom - window_bounds.top);
    if (intersection.right <= intersection.left ||
        intersection.bottom <= intersection.top) {
      (void)duplication_->ReleaseFrame();
      return {.availability = core::CaptureAvailability::outside_output,
              .backend = kind(),
              .diagnostic = std::string(fallback_warning),
              .frame = {}};
    }

    const auto logical_left =
        static_cast<UINT>(intersection.left - output_bounds_.left);
    const auto logical_top =
        static_cast<UINT>(intersection.top - output_bounds_.top);
    const auto logical_copy_width =
        static_cast<UINT>(intersection.right - intersection.left);
    const auto logical_copy_height =
        static_cast<UINT>(intersection.bottom - intersection.top);
    const auto logical_output_width =
        static_cast<std::uint32_t>(output_bounds_.right - output_bounds_.left);
    const auto logical_output_height =
        static_cast<std::uint32_t>(output_bounds_.bottom - output_bounds_.top);
    const auto transformed = transform_output_crop(
        {logical_left, logical_top, logical_left + logical_copy_width,
         logical_top + logical_copy_height},
        logical_output_width, logical_output_height, output_rotation_);
    const D3D11_BOX source{transformed.left,  transformed.top,    0,
                           transformed.right, transformed.bottom, 1};
    const auto destination_x =
        static_cast<std::uint32_t>(intersection.left - window_bounds.left);
    const auto destination_y =
        static_cast<std::uint32_t>(intersection.top - window_bounds.top);
    const core::NormalizedRegion available{
        static_cast<double>(destination_x) / window_width,
        static_cast<double>(destination_y) / window_height,
        static_cast<double>(logical_copy_width) / window_width,
        static_cast<double>(logical_copy_height) / window_height};
    auto captured = readback_.read(
        d3d_, texture.get(), source, window_width, window_height, destination_x,
        destination_y, available, output_rotation_, quality_, now, error_);
    (void)duplication_->ReleaseFrame();
    if (!captured)
      return unavailable(error_);

    // Detect protected/black capture before masking LayerC's own pixels.
    if (frame_is_nearly_black(*captured))
      ++black_frames_;
    else
      black_frames_ = 0;
    apply_excluded_rectangles(*captured, window_bounds);
    last_frame_ = std::move(captured);
    return current_result();
  }

  void
  set_excluded_screen_rects(const std::span<const RECT> rectangles) override {
    excluded_.assign(rectangles.begin(), rectangles.end());
  }
  void set_quality(const core::CaptureQuality quality) override {
    if (quality_ == quality)
      return;
    quality_ = quality;
    last_frame_.reset();
    black_frames_ = 0;
  }
  core::CaptureBackendKind kind() const noexcept override {
    return core::CaptureBackendKind::desktop_duplication;
  }
  std::string gpu_description() const override { return d3d_.description; }
  std::string last_error() const override { return error_; }

private:
  void stop_resources() {
    duplication_.reset();
    output_.reset();
    adapter_.reset();
    readback_.reset();
    d3d_ = {};
    monitor_ = nullptr;
    output_bounds_ = {};
    output_rotation_ = OutputRotation::identity;
  }

  bool fail(const char *prefix, const HRESULT result) {
    error_ = std::string(prefix) + ": " + hresult_message(result);
    started_ = false;
    stop_resources();
    return false;
  }

  CapturePollResult unavailable(std::string diagnostic) const {
    if (!diagnostic.empty())
      diagnostic += "; ";
    diagnostic += fallback_warning;
    return {.availability = core::CaptureAvailability::unavailable,
            .backend = kind(),
            .diagnostic = std::move(diagnostic),
            .frame = {}};
  }

  CapturePollResult current_result() const {
    const auto availability =
        black_frames_ >= 30 ? core::CaptureAvailability::protected_or_black
        : last_frame_       ? core::CaptureAvailability::available
                            : core::CaptureAvailability::starting;
    auto diagnostic = std::string(fallback_warning);
    if (availability == core::CaptureAvailability::protected_or_black)
      diagnostic += "; persistent black frames detected";
    return {.availability = availability,
            .backend = kind(),
            .diagnostic = std::move(diagnostic),
            .frame = last_frame_};
  }

  void apply_excluded_rectangles(CaptureFrame &frame,
                                 const RECT &window_bounds) const {
    const auto window_width =
        std::max<LONG>(1, window_bounds.right - window_bounds.left);
    const auto window_height =
        std::max<LONG>(1, window_bounds.bottom - window_bounds.top);
    for (const auto &excluded : excluded_) {
      const auto overlap = intersect_rect(excluded, window_bounds);
      if (overlap.right <= overlap.left || overlap.bottom <= overlap.top)
        continue;
      frame.unavailable_regions.push_back(
          {static_cast<double>(overlap.left - window_bounds.left) /
               window_width,
           static_cast<double>(overlap.top - window_bounds.top) / window_height,
           static_cast<double>(overlap.right - overlap.left) / window_width,
           static_cast<double>(overlap.bottom - overlap.top) / window_height});
      const auto left = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
          static_cast<std::int64_t>(overlap.left - window_bounds.left) *
              frame.width / window_width,
          0, frame.width));
      const auto top = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
          static_cast<std::int64_t>(overlap.top - window_bounds.top) *
              frame.height / window_height,
          0, frame.height));
      const auto right = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
          (static_cast<std::int64_t>(overlap.right - window_bounds.left) *
               frame.width +
           window_width - 1) /
              window_width,
          0, frame.width));
      const auto bottom = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
          (static_cast<std::int64_t>(overlap.bottom - window_bounds.top) *
               frame.height +
           window_height - 1) /
              window_height,
          0, frame.height));
      for (auto y = top; y < bottom; ++y)
        std::fill(frame.pixels.begin() +
                      static_cast<std::size_t>(y) * frame.width + left,
                  frame.pixels.begin() +
                      static_cast<std::size_t>(y) * frame.width + right,
                  core::RgbPixel{});
    }
  }

  HWND window_{};
  HMONITOR monitor_{};
  RECT output_bounds_{};
  OutputRotation output_rotation_{OutputRotation::identity};
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<IDXGIOutput> output_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  D3dDevice d3d_;
  TextureReadback readback_;
  std::shared_ptr<CaptureFrame> last_frame_;
  std::vector<RECT> excluded_;
  std::string error_;
  std::uint32_t black_frames_{};
  core::CaptureQuality quality_{core::CaptureQuality::balanced};
  bool started_{};
};

} // namespace

std::unique_ptr<CaptureBackend>
make_desktop_duplication_backend(const HWND window) {
  return std::make_unique<DesktopDuplicationBackend>(window);
}

} // namespace layerc::capture
