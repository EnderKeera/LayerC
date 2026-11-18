#include "linux_atspi_test_fixture.hpp"

namespace layerc::test::atspi_fixture {
void a_change_inside_the_throttle_window_still_reaches_clients();
void a_lost_accessibility_bus_is_reported_and_recovered();
void a_refused_embed_is_reported_and_recovered();
void listeners_from_two_buses_are_tracked_separately();
void a_listener_signal_before_the_reply_keeps_the_known_listeners();
void a_failed_bus_open_names_its_cause_and_recovers();
void a_missing_launcher_waits_and_is_not_activated();
void a_launcher_that_names_no_bus_is_unavailable();
void no_at_bridge_disables_the_adaptor();
void no_at_bridge_zero_behaves_normally();
void interfaces_follow_each_roles_capabilities();
void an_action_reads_back_in_the_generation_it_made();
void reads_without_a_listener_stay_within_the_walk_budget();
void component_geometry_follows_the_coordinate_type();
void action_names_and_activation_reach_the_widget();
void value_and_selection_mutations_reach_the_widgets();
void text_offsets_are_characters_and_geometry_is_real();
void edits_run_through_the_fields_own_path_and_report_text_changes();
} // namespace

namespace layerc::test::atspi_fixture {

// --- case a ---------------------------------------------------------------

void accessibility_is_off_until_a_consumer_listens() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  expect(environment.applied(), "the environment can be cleared");
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.spin(ms(400));
  expect(adaptor.status().state == atspi::StatusState::off,
         "nothing listens, so accessibility is off");
  expect(adaptor.status().reason.empty(), "off carries no fault reason");
  expect(!adaptor.connected(), "off keeps no accessibility connection");
  expect(adaptor.snapshot_count() == 0, "off takes no snapshots");
  expect(harness.embeds() == 0, "off never embeds the application");
  expect(harness.signals().empty(), "off broadcasts nothing");
}

// --- case b ---------------------------------------------------------------

void enabling_accessibility_embeds_and_keeps_one_application() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.spin(ms(200));
  expect(harness.embeds() == 0, "nothing embedded while off");

  harness.set_flags(true, false);
  expect(harness.spin_until([&] { return harness.embeds() > 0; }, ms(3000)),
         "an enabled flag embeds the application");
  expect(adaptor.status().state == atspi::StatusState::on,
         "the status reports on");
  expect(adaptor.connected(), "the accessibility connection is up");
  expect(harness.embed_path() == atspi::kRootPath,
         "the application embeds at the fixed root path");
  expect(!harness.embed_name().empty() && harness.embed_name()[0] == ':',
         "the embed names the editor's own connection");
  expect(harness.registered_calls() >= 1,
         "the editor asks the registry for its listeners");

  // A screen reader switching on is not a second application.
  harness.set_flags(true, true);
  harness.spin(ms(300));
  expect(harness.embeds() == 1, "a screen reader alone does not re-embed");
  expect(adaptor.status().state == atspi::StatusState::on,
         "the reports stay on");

  const std::string name = harness.embed_name();
  harness.set_flags(false, false);
  expect(harness.spin_until([&] { return harness.owner_left(name); }, ms(3000)),
         "switching off drops the accessibility connection");
  expect(adaptor.status().state == atspi::StatusState::off,
         "the status returns to off");
  expect(harness.deregistrations() == 1,
         "the editor deregisters before it leaves");
}

// --- case c ---------------------------------------------------------------

void clients_read_the_tree_over_dbus() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until([&] { return harness.embeds() > 0; },
                                 ms(3000)),
             "the application embeds"))
    return;
  harness.spin(ms(300));
  const std::string root{atspi::kRootPath};

  const auto root_name =
      variant_text(harness.property(root, kAccessibleInterface, "Name").reply);
  expect(root_name && *root_name == "LayerE",
         "the application keeps the legacy root name");
  expect(reply_u32(harness.get(root, kAccessibleInterface, "GetRole").reply) ==
             75,
         "the root is ATK_ROLE_APPLICATION");
  expect(reply_text(
             harness.get(root, kAccessibleInterface, "GetRoleName").reply) ==
             "application",
         "the root role name is application");
  const auto root_parent =
      reply_variant_reference(harness.property(root, kAccessibleInterface, "Parent")
                                  .reply);
  expect(root_parent && root_parent->name == kRegistryName &&
             root_parent->path == root,
         "the root's parent is the desktop the registry answered with");
  const auto interfaces = reply_strings(
      harness.get(root, kAccessibleInterface, "GetInterfaces").reply);
  expect(contains(interfaces, kAccessibleInterface) &&
             contains(interfaces, kApplicationInterface),
         "the root exposes the accessible and application interfaces");
  expect(variant_text(harness.property(root, kApplicationInterface,
                                       "ToolkitName")
                          .reply) == "FLTK",
         "the toolkit name is FLTK");
  expect(variant_text(
             harness.property(root, kApplicationInterface, "Version").reply) ==
             "1.4",
         "the toolkit version is the FLTK release");
  expect(variant_text(harness.property(root, kApplicationInterface,
                                       "AtspiVersion")
                          .reply) == "2.1",
         "the AT-SPI version is 2.1");

  // Walk to the button the way an AT client does.
  const auto children = reply_references(
      harness.get(root, kAccessibleInterface, "GetChildren").reply);
  expect(!children.empty(), "the application has windows");
  expect(children.front().name == harness.embed_name(),
         "children are named with the connection that owns them");

  std::string button_path;
  std::string window_path;
  for (const auto &child : children) {
    for (const auto &node : reply_references(
             harness.get(child.path, kAccessibleInterface, "GetChildren")
                 .reply)) {
      const auto name = variant_text(
          harness.property(node.path, kAccessibleInterface, "Name").reply);
      if (name && *name == "Run") {
        button_path = node.path;
        window_path = child.path;
      }
    }
  }
  expect(!button_path.empty(), "the Run button is exported");
  if (button_path.empty()) return;
  expect(reply_text(harness.get(button_path, kAccessibleInterface,
                                "GetRoleName")
                        .reply) == "push button",
         "the Run button is a push button");
  const auto window_children = reply_references(
      harness.get(window_path, kAccessibleInterface, "GetChildren").reply);
  const auto index =
      reply_i32(harness.get(button_path, kAccessibleInterface,
                            "GetIndexInParent")
                    .reply);
  expect(index && *index >= 0 &&
             static_cast<std::size_t>(*index) < window_children.size() &&
             window_children[static_cast<std::size_t>(*index)].path ==
                 button_path,
         "GetIndexInParent matches the child list");
  const auto parent = reply_variant_reference(
      harness.property(button_path, kAccessibleInterface, "Parent").reply);
  expect(parent && parent->name == harness.embed_name() &&
             parent->path == window_path,
         "the button's parent is the window that owns it");
  const auto states =
      reply_u32_array(harness.get(button_path, kAccessibleInterface, "GetState")
                          .reply);
  expect(states.size() == 2, "states come as two 32-bit words");
  expect(has_bit(states, atspi::State::enabled) &&
             has_bit(states, atspi::State::sensitive),
         "the button is enabled and sensitive");
  // The FLTK tree exports a widget's label as the name property and creates
  // no separate label node, so the relation set answers empty; the call must
  // still take the documented signature.
  const auto relations =
      harness.get(button_path, kAccessibleInterface, "GetRelationSet");
  expect(!relations.failed && reply_relation_kinds(relations.reply) == 0,
         "GetRelationSet answers with the model's relation links");
  const auto child_at =
      harness.ask(harness.embed_name(), window_path, kAccessibleInterface,
                  "GetChildAtIndex", [](sd_bus_message *call) {
                    std::int32_t index = 0;
                    return sd_bus_message_append(call, "i", index);
                  });
  const auto first_child = reply_reference(child_at.reply);
  expect(first_child.has_value() && !window_children.empty() &&
             first_child->path == window_children.front().path,
         "GetChildAtIndex(0) is the first child");
  const auto out_of_range =
      harness.ask(harness.embed_name(), window_path, kAccessibleInterface,
                  "GetChildAtIndex", [](sd_bus_message *call) {
                    std::int32_t index = 99;
                    return sd_bus_message_append(call, "i", index);
                  });
  expect(out_of_range.failed &&
             out_of_range.error == SD_BUS_ERROR_INVALID_ARGS,
         "an out-of-range child index is rejected");
  const auto unknown =
      harness.get("/org/a11y/atspi/accessible/424242", kAccessibleInterface,
                  "GetRole");
  expect(unknown.failed && unknown.error == SD_BUS_ERROR_UNKNOWN_OBJECT,
         "an unknown object path is an UnknownObject error");

  const auto cache_items =
      harness.ask(harness.embed_name(), std::string{atspi::kCachePath},
                  kCacheInterface, "GetItems");
  const std::size_t cached =
      cache_items.failed ? 0U : reply_cache_items(cache_items.reply);
  expect(cached > 0, "the cache answers GetItems");
  expect(cached == reachable_objects(harness),
         "the cache mirrors every reachable object");
  expect(adaptor.snapshot_count() > 0, "requests came from a published tree");
}

// --- case d ---------------------------------------------------------------

void changes_reach_listeners_as_protocol_events() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"focus:", "object:"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until(
                 [&] { return harness.embeds() > 0 && harness.registered_calls() > 0; },
                 ms(3000)),
             "the application embeds and reads the listeners"))
    return;
  harness.spin(ms(250));

  const auto wait_for = [&](const std::size_t mark, const std::string &iface,
                            const std::string &member,
                            const std::string &name) {
    // find() below reports what did not arrive.
    (void)harness.spin_until(
        [&] {
          const auto &signals = harness.signals();
          for (std::size_t i = mark; i < signals.size(); ++i) {
            const auto &signal = signals[i];
            if (signal.interface == iface && signal.member == member &&
                (name.empty() || signal.name == name))
              return true;
          }
          return false;
        },
        ms(3000));
  };
  const auto find = [&](const std::size_t mark, const std::string &iface,
                        const std::string &member, const std::string &name)
      -> std::optional<Signal> {
    const auto &signals = harness.signals();
    for (std::size_t i = mark; i < signals.size(); ++i) {
      const auto &signal = signals[i];
      if (signal.interface == iface && signal.member == member &&
          (name.empty() || signal.name == name))
        return signal;
    }
    return std::nullopt;
  };
  const std::string object{"org.a11y.atspi.Event.Object"};
  const std::string focus{"org.a11y.atspi.Event.Focus"};

  // Name change.
  std::size_t mark = harness.signals().size();
  fixture.input->label("Full name");
  wait_for(mark, object, "PropertyChange", "accessible-name");
  const auto named = find(mark, object, "PropertyChange", "accessible-name");
  expect(named && named->text == "Full name",
         "renaming a field reports the new accessible name");

  // Focus. A window opens with its first focusable widget focused, and in a
  // headless session that is this button, so move the focus away first: the
  // step below has to be a real change on every desktop.
  expect(fixture.input->take_focus(), "the field takes keyboard focus");
  harness.spin(ms(250));
  mark = harness.signals().size();
  expect(fixture.button->take_focus(), "the button takes keyboard focus back");
  wait_for(mark, focus, "Focus", {});
  expect(find(mark, focus, "Focus", {}).has_value(),
         "moving focus reports a focus event");
  expect(find(mark, object, "StateChanged", "focused").has_value(),
         "moving focus reports the focused state");

  // A widget appearing and disappearing.
  mark = harness.signals().size();
  auto *extra = new Fl_Button(240, 20, 120, 28, "Extra");
  fixture.window->add(extra);
  extra->show();
  fixture.window->redraw();
  wait_for(mark, kCacheInterface, "AddAccessible", {});
  expect(find(mark, kCacheInterface, "AddAccessible", {}).has_value(),
         "a new widget enters the cache");
  const auto added = find(mark, object, "ChildrenChanged", "add");
  expect(added.has_value(), "a new widget reports its parent's new child");
  mark = harness.signals().size();
  Fl::delete_widget(extra);
  wait_for(mark, kCacheInterface, "RemoveAccessible", {});
  expect(find(mark, kCacheInterface, "RemoveAccessible", {}).has_value(),
         "a deleted widget leaves the cache");
  expect(find(mark, object, "ChildrenChanged", "remove").has_value(),
         "a deleted widget reports its parent's lost child");

  // Text insertion.
  fixture.input->value("h\xc3\xa9");
  harness.spin(ms(200));
  mark = harness.signals().size();
  fixture.input->value("h\xc3\xa9llo");
  wait_for(mark, object, "TextChanged", "insert");
  const auto inserted = find(mark, object, "TextChanged", "insert");
  expect(inserted && inserted->text == "llo" && inserted->detail1 == 2 &&
             inserted->detail2 == 3,
         "inserted text reports the character offset and count");

  // Announcement.
  mark = harness.signals().size();
  adaptor.announce(fixture.button, "Saved");
  wait_for(mark, object, "Announcement", {});
  const auto announced = find(mark, object, "Announcement", {});
  expect(announced && announced->text == "Saved" && announced->detail1 == 1,
         "an announcement reaches a listening client");
}

void announcements_are_suppressed_without_a_listener() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"focus:"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until(
                 [&] { return harness.embeds() > 0 && harness.registered_calls() > 0; },
                 ms(3000)),
             "the application embeds and reads the listeners"))
    return;
  harness.spin(ms(250));
  const std::string object{"org.a11y.atspi.Event.Object"};

  std::size_t mark = harness.signals().size();
  adaptor.announce(fixture.button, "Saved");
  harness.spin(ms(400));
  bool seen = false;
  for (std::size_t i = mark; i < harness.signals().size(); ++i)
    if (harness.signals()[i].interface == object &&
        harness.signals()[i].member == "Announcement")
      seen = true;
  expect(!seen, "no listener means no announcement");

  mark = harness.signals().size();
  harness.announce_listener("object:announcement");
  harness.spin(ms(200));
  adaptor.announce(fixture.button, "Saved again");
  expect(harness.spin_until(
             [&] {
               for (std::size_t i = mark; i < harness.signals().size(); ++i)
                 if (harness.signals()[i].interface == object &&
                     harness.signals()[i].member == "Announcement")
                   return true;
               return false;
             },
             ms(3000)),
         "a later listener receives the announcement");
}

// --- case e ---------------------------------------------------------------

void disabling_stops_events_and_enabling_resumes() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"object:"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  Fixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(harness.spin_until([&] { return harness.embeds() > 0; },
                                 ms(3000)),
             "the application embeds"))
    return;
  const std::string first = harness.embed_name();
  const auto snapshots = adaptor.snapshot_count();
  expect(snapshots > 0, "the tree was published");

  const std::string object{"org.a11y.atspi.Event.Object"};
  std::size_t mark = harness.signals().size();
  harness.set_flags(false, false);
  expect(harness.spin_until([&] { return harness.owner_left(first); },
                            ms(3000)),
         "switching off drops the connection");
  expect(!adaptor.connected(), "no connection remains");
  expect(adaptor.status().state == atspi::StatusState::off,
         "the status is off");
  const std::string second = harness.embed_name();
  expect(!second.empty(), "the first embed is remembered");

  const auto stopped = adaptor.snapshot_count();
  mark = harness.signals().size();
  fixture.button->label("Go");
  harness.spin(ms(500));
  expect(adaptor.snapshot_count() == stopped,
         "off takes no further snapshots");
  bool event_seen = false;
  for (std::size_t i = mark; i < harness.signals().size(); ++i)
    if (harness.signals()[i].interface == object) event_seen = true;
  expect(!event_seen, "off broadcasts no events");

  harness.set_flags(true, false);
  expect(harness.spin_until([&] { return harness.embeds() == 2; }, ms(3000)),
         "enabling again embeds a new application");
  expect(harness.embed_path() == atspi::kRootPath,
         "the new application embeds at the root path");
  expect(adaptor.status().state == atspi::StatusState::on,
         "the status is on again");
  mark = harness.signals().size();
  fixture.button->label("Run");
  expect(harness.spin_until(
             [&] {
               for (std::size_t i = mark; i < harness.signals().size(); ++i)
                 if (harness.signals()[i].interface == object)
                   return true;
               return false;
             },
             ms(3000)),
         "events resume after enabling");
  expect(adaptor.snapshot_count() > stopped, "snapshots resume");
}

// --- case f ---------------------------------------------------------------

void the_status_watch_reports_why_accessibility_is_unavailable() {
  layerc::test::require_x11();
  require_session_bus();

  // A Flatpak-style fixed address needs neither org.a11y.Bus nor a flag query.
  {
    Harness harness;
    require_private_bus(harness);
    harness.set_registered_events({});
    const layerc::test::ScopedEnvironment environment{
        "AT_SPI_BUS_ADDRESS",
        std::filesystem::path{harness.session_address()}};
    Fixture fixture;
    atspi::Adaptor adaptor{fixture.tree};
    expect(harness.spin_until([&] { return harness.embeds() > 0; }, ms(3000)),
           "a fixed accessibility address is used without the bus");
    expect(adaptor.status().state == atspi::StatusState::on,
           "the fixed address reports on");
  }

  // org.a11y.Bus answers GetAddress with a failure: the fault is reported.
  {
    Harness harness;
    require_private_bus(harness);
    harness.set_registered_events({});
    harness.set_address_error(true);
    const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                      std::nullopt};
    Fixture fixture;
    atspi::Adaptor adaptor{fixture.tree};
    harness.set_flags(true, false);
    expect(harness.spin_until(
               [&] {
                 return adaptor.status().state ==
                        atspi::StatusState::unavailable;
               },
               ms(3000)),
           "a failed address request is unavailable");
    expect(!adaptor.status().reason.empty(),
           "the fault names the failed address request");
    expect(!adaptor.connected(), "no connection is opened");
    expect(harness.embeds() == 0, "nothing is embedded without an address");
    harness.set_flags(false, false);
    expect(harness.spin_until(
               [&] {
                 return adaptor.status().state == atspi::StatusState::off;
               },
               ms(3000)),
           "disabling clears the failed address fault");
  }

  // A bus that answers with an unusable address.
  {
    Harness harness;
    require_private_bus(harness);
    harness.set_registered_events({});
    harness.set_reported_address("unix:path=/nonexistent/layerc-atspi.sock");
    const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                      std::nullopt};
    Fixture fixture;
    atspi::Adaptor adaptor{fixture.tree};
    harness.set_flags(true, false);
    expect(harness.spin_until(
               [&] {
                 return adaptor.status().state ==
                        atspi::StatusState::unavailable;
               },
               ms(3000)),
           "an unusable address is reported");
    expect(!adaptor.status().reason.empty(),
           "the fault explains the failed connection");
    expect(!adaptor.connected(), "nothing stays connected");
    expect(harness.embeds() == 0, "nothing is embedded on a broken bus");
    harness.set_flags(false, false);
    expect(harness.spin_until(
               [&] {
                 return adaptor.status().state == atspi::StatusState::off;
               },
               ms(3000)),
           "disabling clears the fault");
  }
}

} // namespace layerc::test::atspi_fixture

using namespace layerc::test::atspi_fixture;

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(accessibility_is_off_until_a_consumer_listens),
    LAYERC_TEST_CASE(enabling_accessibility_embeds_and_keeps_one_application),
    LAYERC_TEST_CASE(clients_read_the_tree_over_dbus),
    LAYERC_TEST_CASE(changes_reach_listeners_as_protocol_events),
    LAYERC_TEST_CASE(announcements_are_suppressed_without_a_listener),
    LAYERC_TEST_CASE(disabling_stops_events_and_enabling_resumes),
    LAYERC_TEST_CASE(the_status_watch_reports_why_accessibility_is_unavailable),
    LAYERC_TEST_CASE(a_change_inside_the_throttle_window_still_reaches_clients),
    LAYERC_TEST_CASE(a_lost_accessibility_bus_is_reported_and_recovered),
    LAYERC_TEST_CASE(a_refused_embed_is_reported_and_recovered),
    LAYERC_TEST_CASE(listeners_from_two_buses_are_tracked_separately),
    LAYERC_TEST_CASE(a_listener_signal_before_the_reply_keeps_the_known_listeners),
    LAYERC_TEST_CASE(a_failed_bus_open_names_its_cause_and_recovers),
    LAYERC_TEST_CASE(a_missing_launcher_waits_and_is_not_activated),
    LAYERC_TEST_CASE(a_launcher_that_names_no_bus_is_unavailable),
    LAYERC_TEST_CASE(no_at_bridge_disables_the_adaptor),
    LAYERC_TEST_CASE(no_at_bridge_zero_behaves_normally),
    LAYERC_TEST_CASE(interfaces_follow_each_roles_capabilities),
    LAYERC_TEST_CASE(an_action_reads_back_in_the_generation_it_made),
    LAYERC_TEST_CASE(reads_without_a_listener_stay_within_the_walk_budget),
    LAYERC_TEST_CASE(component_geometry_follows_the_coordinate_type),
    LAYERC_TEST_CASE(action_names_and_activation_reach_the_widget),
    LAYERC_TEST_CASE(value_and_selection_mutations_reach_the_widgets),
    LAYERC_TEST_CASE(text_offsets_are_characters_and_geometry_is_real),
    LAYERC_TEST_CASE(edits_run_through_the_fields_own_path_and_report_text_changes))
