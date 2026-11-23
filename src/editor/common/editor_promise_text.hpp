#pragma once

#include <string_view>

namespace layerc::editor {

// About's input and network promise in the README's terms (Non-negotiable
// guarantees). The Windows editors name the two modes that take input; the
// Linux editor has none.
[[nodiscard]] constexpr std::string_view about_input_promise() noexcept {
#ifdef _WIN32
  return "LayerC reads input passively and never changes or sends it. Only two editor modes "
         "that you start yourself take mouse input. Pick window captures the mouse for your "
         "next left click; pressing a key other than a modifier, losing mouse capture or the "
         "end of the edit cancels the pick. Direct drag, "
         "once you agree, lets LayerD's visible panels take the clicks that land on them while "
         "you position them on the game window. The editor never accesses the network.";
#else
  return "LayerC reads input passively: it never blocks, changes or sends input, and the "
         "editor never accesses the network.";
#endif
}

} // namespace layerc::editor
