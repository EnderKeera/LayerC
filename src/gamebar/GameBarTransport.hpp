#pragma once

#include "layerc/core/companion.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::gamebar {

struct GameBarTransportState;
struct GameBarPreferenceUpdate {
  std::uint64_t request_id{};
  std::string json;
};

// Demand-owned App Service client used only by the packaged daemon. It sends
// immutable bounded snapshots to the in-process UWP provider and receives
// bounded presentation-preference documents. The widget never gains cooldown
// authority.
class GameBarTransport final {
public:
  GameBarTransport();
  ~GameBarTransport();
  GameBarTransport(const GameBarTransport &) = delete;
  GameBarTransport &operator=(const GameBarTransport &) = delete;

  [[nodiscard]] bool start();
  void stop() noexcept;
  [[nodiscard]] std::vector<GameBarPreferenceUpdate> poll() noexcept;
  [[nodiscard]] bool publish(std::string_view snapshot_json);
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] HANDLE wait_handle() const noexcept;
  [[nodiscard]] std::wstring last_error() const;
  [[nodiscard]] std::size_t resource_handle_count() const noexcept;
  void complete_preference_update(std::uint64_t request_id,
                                  bool accepted) noexcept;

  inline static constexpr std::wstring_view app_service_name =
      L"LayerC.GameBar.v1";
  inline static constexpr std::wstring_view package_name = L"LayerC.GameBar.v1";
  // Keep in sync with Package.appxmanifest Identity (Name, Publisher) and
  // the family derived by Windows from that identity.
  inline static constexpr std::wstring_view package_publisher = L"CN=LayerC";
  inline static constexpr std::wstring_view package_family_name =
      L"LayerC.GameBar.v1_bs8jpev27s1ay";
  inline static constexpr std::uint16_t package_major_version = 2;
  inline static constexpr std::uint16_t package_minor_version = 4;

  [[nodiscard]] static bool is_expected_package_identity(
      std::wstring_view name, std::wstring_view publisher,
      std::wstring_view family_name, std::uint16_t major,
      std::uint16_t minor) noexcept;
  [[nodiscard]] static bool current_process_is_expected_package() noexcept;

private:
  std::shared_ptr<GameBarTransportState> state_;
};

} // namespace layerc::gamebar
