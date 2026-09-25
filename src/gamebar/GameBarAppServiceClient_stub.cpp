// Inert App Service client for toolchains without C++/WinRT (MinGW). The
// transport stays stopped with a visible reason; every other entry point
// observes the stopped link and does nothing.
#include "GameBarTransportState.hpp"

namespace layerc::gamebar::detail {

bool start_app_service(const std::shared_ptr<GameBarTransportState> &state) {
  std::lock_guard lock(state->mutex);
  state->enter(GameBarTransportState::Link::stopped,
               L"Game Bar App Service requires the MSVC package build");
  return false;
}

void stop_app_service(const std::shared_ptr<GameBarTransportState> &) noexcept {}

std::vector<GameBarPreferenceUpdate>
poll_app_service(const std::shared_ptr<GameBarTransportState> &) noexcept {
  return {};
}

void complete_preference_update(const std::shared_ptr<GameBarTransportState> &,
                                std::uint64_t, bool) noexcept {}

} // namespace layerc::gamebar::detail
