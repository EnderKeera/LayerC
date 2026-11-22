// LayerD has no on-window edit channel on Linux: positioning publishes the
// live draft, which moves the click-through overlay.
#include "platform/on_window_edit.hpp"

namespace layerc::editor {

bool on_window_edit_sessions() noexcept { return false; }

std::unique_ptr<OnWindowEdit> make_on_window_edit(std::function<void()>) { return nullptr; }

} // namespace layerc::editor
