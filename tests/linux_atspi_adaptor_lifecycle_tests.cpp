#include "linux_atspi_test_fixture.hpp"

namespace layerc::test::atspi_fixture {

// --- case g ---------------------------------------------------------------

void a_change_inside_the_throttle_window_still_reaches_clients() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"object:accessible-name"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(
          harness.spin_until([&] { return harness.embeds() > 0; }, ms(3000)),
          "the application embeds"))
    return;
  // Land the change inside the window: the last refresh is at most a few
  // milliseconds old, so the next check is throttled.
  harness.spin(ms(150));
  const auto mark = harness.signals().size();
  struct Rename {
    Fl_Input *input;
    const char *label;
    Clock::time_point changed{};
  } rename{fixture.input, "Renamed by a timer"};
  Fl::add_timeout(
      0.0,
      +[](void *data) {
        auto *rename = static_cast<Rename *>(data);
        rename->input->label(rename->label);
        rename->changed = Clock::now();
      },
      &rename);

  // Only blocking waits: with nothing armed the poll sleeps the whole second,
  // so no check can publish a skipped change inside the window. The throttled
  // check arms its one-shot timer, the poll wakes on it, and the next wait
  // publishes the change.
  const EventQuery query{"org.a11y.atspi.Event.Object", "PropertyChange",
                         "accessible-name"};
  std::optional<Signal> named;
  const auto deadline = Clock::now() + ms(5000);
  while (Clock::now() < deadline) {
    harness.drain();
    named = find_event(harness, mark, query);
    if (named) break;
    if (rename.changed != Clock::time_point{} &&
        Clock::now() - rename.changed > ms(400))
      break;
    Fl::wait(1.0);
  }
  expect(named && named->text == "Renamed by a timer",
         "a change inside the throttle window reaches the client without "
         "input");
}

// --- case h ---------------------------------------------------------------

void a_lost_accessibility_bus_is_reported_and_recovered() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"focus:"});
  Daemon a11y;
  if (!expect(a11y.is_running(), "a private accessibility bus starts")) return;
  RegistryHost host{a11y.address()};
  if (!expect(host.is_open(), "the private bus hosts a registry")) return;
  harness.pump([&host] { host.drain(); });
  harness.set_reported_address(a11y.address());
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until([&] { return host.embeds() > 0; }, ms(3000)),
              "the application embeds on the private bus"))
    return;
  expect(adaptor.status().state == atspi::StatusState::on,
         "the private bus reports on");
  expect(adaptor.connected(), "the accessibility connection is up");
  expect(adaptor.watching(), "the loop watches the tree");

  // The bus dies: the connection reports the loss and nothing keeps watching.
  a11y.stop();
  // The status can turn unavailable inside dispatch, where a pending reply
  // handler re-reports it, while the connection itself closes on the next loop
  // iteration: the required end state is both, so wait for both.
  if (!expect(harness.spin_until(
                  [&] {
                    return adaptor.status().state ==
                               atspi::StatusState::unavailable &&
                           !adaptor.connected() && !adaptor.watching();
                  },
                  ms(5000)),
              "a dead accessibility bus is reported"))
    return;
  expect(adaptor.status().state == atspi::StatusState::unavailable,
         "the lost bus is unavailable");
  expect(!adaptor.status().reason.empty(),
         "the fault explains the lost bus");
  expect(!adaptor.connected(), "the dead connection is closed");
  expect(!adaptor.watching(), "no check keeps watching a dead bus");

  // A new bus and registry: the owner change reconnects and re-embeds.
  Daemon replacement;
  if (!expect(replacement.is_running(), "a replacement bus starts")) return;
  RegistryHost again{replacement.address()};
  if (!expect(again.is_open(), "the replacement hosts a registry")) return;
  harness.pump([&again] { again.drain(); });
  harness.set_reported_address(replacement.address());
  harness.set_flags(false, false);
  harness.set_flags(true, false);
  expect(harness.spin_until([&] { return again.embeds() > 0; }, ms(5000)),
         "the replacement bus is used");
  expect(harness.spin_until(
             [&] { return adaptor.status().state == atspi::StatusState::on; },
             ms(3000)),
         "the status returns to on");
  expect(adaptor.connected(), "the connection is replaced");
  expect(adaptor.watching(), "the loop watches the replacement tree");
}

// --- case i ---------------------------------------------------------------

void a_refused_embed_is_reported_and_recovered() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  harness.set_embed_error("org.a11y.atspi.Error.NotSupported");
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  const bool embedded =
      harness.spin_until([&] { return harness.embeds() > 0; }, ms(3000));
  if (!expect(embedded, "the embed is attempted"))
    return;
  const auto refused = [&] {
    const auto status = adaptor.status();
    return status.state == atspi::StatusState::unavailable &&
           status.reason.find("org.a11y.atspi.Error.NotSupported") !=
               std::string::npos;
  };
  expect(harness.spin_until(refused, ms(3000)),
         "a refused Embed is unavailable and names the refusal");

  // A registry that accepts clears the fault and accepts the application.
  harness.set_embed_error({});
  harness.release_registry_name();
  harness.request_registry_name();
  expect(harness.spin_until(
             [&] { return adaptor.status().state == atspi::StatusState::on; },
             ms(4000)),
         "a registry that accepts clears the refusal");
  expect(adaptor.connected(), "the connection stays up");
}

// --- case j ---------------------------------------------------------------

void listeners_from_two_buses_are_tracked_separately() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until(
                  [&] {
                    return harness.embeds() > 0 &&
                           harness.registered_calls() > 0;
                  },
                  ms(3000)),
              "the application embeds and reads the listeners"))
    return;

  const EventQuery announcement{"org.a11y.atspi.Event.Object", "Announcement",
                                {}};
  harness.announce_listener_from(":1.100", "object:announcement");
  harness.announce_listener_from(":1.101", "object:announcement");
  harness.spin(ms(200));

  // One bus leaving keeps the other listener.
  harness.deregister_listener(":1.100", "object:announcement");
  harness.spin(ms(200));
  auto mark = harness.signals().size();
  adaptor.announce(fixture.button, "Saved");
  expect(wait_event(harness, mark, announcement, ms(3000)),
         "the remaining bus still receives the announcement");

  // The last listener leaving suppresses the announcement.
  harness.deregister_listener(":1.101", "object:announcement");
  harness.spin(ms(200));
  mark = harness.signals().size();
  adaptor.announce(fixture.button, "Saved again");
  harness.spin(ms(400));
  expect(!find_event(harness, mark, announcement).has_value(),
         "the announcement is suppressed once every listener is gone");
}

// --- case k ---------------------------------------------------------------

void a_listener_signal_before_the_reply_keeps_the_known_listeners() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"object:announcement"});
  harness.hold_registered_replies(true);
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until(
                  [&] {
                    return harness.embeds() > 0 &&
                           harness.registered_calls() > 0;
                  },
                  ms(3000)),
              "the application asks for the listeners"))
    return;

  // A listener announces itself while the answer is still outstanding.
  harness.announce_listener_from(":1.200", "focus:");
  harness.spin(ms(200));
  if (!expect(harness.release_registered_reply(), "the answer is released"))
    return;
  harness.spin(ms(300));

  const EventQuery announcement{"org.a11y.atspi.Event.Object", "Announcement",
                                {}};
  const auto mark = harness.signals().size();
  adaptor.announce(fixture.button, "Saved");
  expect(wait_event(harness, mark, announcement, ms(3000)),
         "a listener from the answer is not hidden by the earlier signal");
}

// --- case l ---------------------------------------------------------------

// The address the bus hands out cannot be connected to: the status must name
// that cause instead of the generic "not connected" that a teardown leaves.
void a_failed_bus_open_names_its_cause_and_recovers() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  harness.set_reported_address(
      "unix:path=" +
      std::string{(std::filesystem::temp_directory_path() /
                   "layerc-atspi-missing.sock")
                      .string()});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  const auto named = [&] {
    const auto status = adaptor.status();
    return status.state == atspi::StatusState::unavailable &&
           status.reason.starts_with("the accessibility bus is unavailable:");
  };
  expect(harness.spin_until(named, ms(3000)),
         "a failed bus open names its cause");
  expect(!adaptor.connected(), "nothing stays connected");
  expect(harness.embeds() == 0, "nothing is embedded when the open failed");

  // A usable address from the same watch clears the fault.
  harness.set_flags(false, false);
  expect(harness.spin_until(
             [&] { return adaptor.status().state == atspi::StatusState::off; },
             ms(3000)),
         "disabling clears the failed open fault");
  harness.set_reported_address(std::string{harness.session_address()});
  harness.set_flags(true, false);
  expect(harness.spin_until(
             [&] { return adaptor.status().state == atspi::StatusState::on; },
             ms(4000)),
         "a usable address clears the fault");
  expect(adaptor.connected(), "the connection is up after the retry");
}

} // namespace layerc::test::atspi_fixture
