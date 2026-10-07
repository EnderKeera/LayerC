#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace layerc::editor {

// Exclusive keyboard binding-capture mode. Capture is either idle or holds
// the ordered chord pressed so far; no second binding can be captured
// concurrently.
class BindingCapture final {
public:
  [[nodiscard]] bool capturing_binding() const noexcept {
    return capturing_binding_;
  }
  [[nodiscard]] const std::vector<std::uint16_t> &
  pending_binding_codes() const noexcept {
    return pending_binding_codes_;
  }
  void begin_binding_capture() {
    capturing_binding_ = true;
    pending_binding_codes_.clear();
  }
  void finish_binding_capture() noexcept {
    capturing_binding_ = false;
    pending_binding_codes_.clear();
  }
  void record_binding_key(const std::uint16_t code) {
    if (std::ranges::find(pending_binding_codes_, code) ==
        pending_binding_codes_.end())
      pending_binding_codes_.push_back(code);
  }

private:
  bool capturing_binding_{};
  std::vector<std::uint16_t> pending_binding_codes_;
};

} // namespace layerc::editor
