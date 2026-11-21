#include "platform/hid_binding_capture.hpp"

namespace layerc::editor {

bool hid_binding_capture_available() noexcept { return false; }

std::unique_ptr<HidBindingCapture> make_hid_binding_capture() { return nullptr; }

} // namespace layerc::editor
