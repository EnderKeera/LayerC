#include "platform/services.hpp"

namespace layerc::editor {

bool standalone_live_drafts() noexcept { return false; }

StandaloneLiveDraftsResult
make_standalone_live_drafts(std::function<void()>) {
  return StandaloneLiveDraftsUnavailable{
      "Live drafts on Windows are published during the on-window edit session."};
}

} // namespace layerc::editor
