#pragma once

namespace layerc::editor {

// Starts the sibling layerd.exe without a console window. True only when the
// process was created; whether it then serves the pipe is LayerD's to report.
[[nodiscard]] bool start_daemon();

} // namespace layerc::editor
