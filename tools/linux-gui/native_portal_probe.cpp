// Test-only native portal client. The runner owns its bus/compositor and token store.
#include "portal_screencast.hpp"
#include "layerc/core/json.hpp"
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 2) return 64;
  const auto *owned = std::getenv("LAYERC_TEST_PRIVATE_NATIVE_SESSION");
  const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!owned || std::string_view{owned} != "1" || !runtime ||
      std::string_view{runtime}.find("/layerc-consent.") == std::string_view::npos ||
      std::getenv("DISPLAY")) return 64;
  using namespace layerc::platform;
  const std::filesystem::path root{argv[1]};
  EventLoop loop;
  BusConnection bus;
  if (!loop.valid() || !bus.open(loop)) return 2;
  ScreencastTokenStore tokens{root / "tokens.json"};
  PortalScreencast portal{bus, tokens};
  if (!portal.start("native-consent-test")) return 3;
  std::string previous;
  bool was_ready{};
  const auto end = layerc::core::MonotonicClock::now() + std::chrono::seconds{100};
  while (layerc::core::MonotonicClock::now() < end) {
    (void)loop.poll_once(50);
    bus.dispatch();
    portal.sync();
    if (previous != portal.reason() || portal.ready() != was_ready) {
      std::string state{"{\"ready\":"};
      layerc::core::json::append_bool(state, portal.ready());
      state += ",\"reason\":";
      layerc::core::json::append_quoted(state, portal.reason());
      state += "}";
      std::cout << state << std::endl;
      previous = portal.reason();
      was_ready = portal.ready();
    }
    if (std::filesystem::exists(root / "cancel")) {
      portal.stop();
      for (int i{}; i != 10; ++i) { (void)loop.poll_once(20); bus.dispatch(); }
      std::cout << "{\"clientCancelled\":true,\"ready\":false}" << std::endl;
      return 0;
    }
    if (std::filesystem::exists(root / "finish")) return 0;
  }
  return 4;
}
