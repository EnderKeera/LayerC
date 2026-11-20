#include "linux_atspi_interfaces_fixture.hpp"

#include <FL/Fl_Widget.H>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>

namespace layerc::test::atspi_fixture {

// --- shared lookups -------------------------------------------------------

std::optional<std::string> node_path_named(Harness &harness,
                                           const std::string_view name) {
  std::vector<std::string> queue{std::string{atspi::kRootPath}};
  for (std::size_t i = 0; i < queue.size(); ++i) {
    const std::string path = queue[i];
    const auto label = variant_text(
        harness.property(path, kAccessibleInterface, "Name").reply);
    if (label && *label == name) return path;
    for (const auto &child : children_of(harness, path))
      queue.push_back(child.path);
  }
  return std::nullopt;
}

std::vector<Reference> children_of(Harness &harness, const std::string &path) {
  return reply_references(
      harness.get(path, kAccessibleInterface, "GetChildren").reply);
}

bool embed(Harness &harness, const atspi::Adaptor &adaptor) {
  (void)adaptor;
  return harness.spin_until(
      [&] { return harness.embeds() > 0 && harness.registered_calls() > 0; },
      ms(3000));
}

namespace {

// Interfaces a node announces, read over D-Bus like a client reads them.
std::vector<std::string> interfaces_of(Harness &harness,
                                       const std::string_view name) {
  const auto path = node_path_named(harness, name);
  if (!path) return {};
  return reply_strings(
      harness.get(*path, kAccessibleInterface, "GetInterfaces").reply);
}

bool interface_named(const std::vector<std::string> &interfaces,
                     const std::string_view wanted) {
  return contains(interfaces, wanted);
}

// The (name, description, keybinding) rows of GetActions.
std::vector<std::tuple<std::string, std::string, std::string>>
actions_of(Harness &harness, const std::string &path) {
  std::vector<std::tuple<std::string, std::string, std::string>> rows;
  // The call result owns the reply message: keep it alive while reading.
  const auto call = harness.get(path, "org.a11y.atspi.Action", "GetActions");
  sd_bus_message *reply = call.reply;
  if (reply == nullptr ||
      sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(sss)") < 0)
    return rows;
  while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "sss") > 0) {
    const char *name = nullptr;
    const char *description = nullptr;
    const char *binding = nullptr;
    if (sd_bus_message_read(reply, "sss", &name, &description, &binding) < 0)
      break;
    rows.emplace_back(name ? name : "", description ? description : "",
                      binding ? binding : "");
    sd_bus_message_exit_container(reply);
  }
  sd_bus_message_exit_container(reply);
  return rows;
}

} // namespace

// --- case a: which interfaces each role announces -------------------------

void interfaces_follow_each_roles_capabilities() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto button = interfaces_of(harness, "Apply");
  expect(interface_named(button, kAccessibleInterface),
         "a button is an accessible");
  expect(interface_named(button, "org.a11y.atspi.Component"),
         "a button has geometry");
  expect(interface_named(button, "org.a11y.atspi.Action"),
         "a button carries an action");
  expect(!interface_named(button, "org.a11y.atspi.Text") &&
             !interface_named(button, "org.a11y.atspi.Value") &&
             !interface_named(button, "org.a11y.atspi.Selection"),
         "a button carries no text, value or selection");

  const auto field = interfaces_of(harness, "Name");
  expect(interface_named(field, "org.a11y.atspi.Text"),
         "an input carries text");
  expect(interface_named(field, "org.a11y.atspi.EditableText"),
         "an editable input carries editable text");
  expect(!interface_named(field, "org.a11y.atspi.Action"),
         "an input has no action of its own");

  const auto fixed = interfaces_of(harness, "Reference");
  expect(interface_named(fixed, "org.a11y.atspi.Text"),
         "a read-only input still carries text");
  expect(!interface_named(fixed, "org.a11y.atspi.EditableText"),
         "a read-only input is not editable");

  const auto masked = interfaces_of(harness, "Passcode");
  expect(!interface_named(masked, "org.a11y.atspi.Text") &&
             !interface_named(masked, "org.a11y.atspi.EditableText"),
         "a masked input publishes no text");

  const auto notes = interfaces_of(harness, "Notes");
  expect(interface_named(notes, "org.a11y.atspi.Text") &&
             interface_named(notes, "org.a11y.atspi.EditableText"),
         "a multi-line input carries editable text");

  const auto slider = interfaces_of(harness, "Level");
  expect(interface_named(slider, "org.a11y.atspi.Value"),
         "a valuator carries a value");
  expect(!interface_named(slider, "org.a11y.atspi.Text"),
         "a valuator carries no text");

  const auto list = interfaces_of(harness, "Items");
  expect(interface_named(list, "org.a11y.atspi.Selection"),
         "a list carries a selection");
  expect(!interface_named(list, "org.a11y.atspi.Value"),
         "a list has no value");

  const auto radios = interfaces_of(harness, "Mode");
  expect(interface_named(radios, "org.a11y.atspi.Selection"),
         "a group of radio buttons carries a selection");
  const auto mode_id = fixture.tree.id_for(*fixture.mode);
  const auto mode_snapshot = fixture.tree.snapshot();
  const auto mode_node = mode_snapshot.nodes.find(mode_id);
  expect(mode_node != mode_snapshot.nodes.end() &&
             layerc::editor::a11y::radio_group(mode_snapshot, mode_node->second),
         "a group of plain radio-typed buttons derives a radio group");

  const auto label = interfaces_of(harness, "Static");
  expect(interface_named(label, "org.a11y.atspi.Component"),
         "a label has geometry");
  expect(!interface_named(label, "org.a11y.atspi.Action") &&
             !interface_named(label, "org.a11y.atspi.Text") &&
             !interface_named(label, "org.a11y.atspi.Value") &&
             !interface_named(label, "org.a11y.atspi.Selection"),
         "a label carries only geometry");

  // A path that does not expose the interface refuses its methods: a client
  // never reaches a member the cache did not announce for that node.
  const auto label_path = node_path_named(harness, "Static");
  if (!expect(label_path.has_value(), "the label has a path")) return;
  const auto absent = harness.ask(harness.embed_name(), *label_path,
                                  "org.a11y.atspi.Text", "GetText",
                                  [](sd_bus_message *call) {
                                    return sd_bus_message_append(call, "ii", 0,
                                                                 -1);
                                  });
  expect(absent.failed, "a missing interface refuses its methods");
  expect(absent.error == "org.freedesktop.DBus.Error.UnknownInterface" ||
             absent.error == "org.freedesktop.DBus.Error.UnknownMethod" ||
             absent.error == "org.freedesktop.DBus.Error.InvalidArgs",
         "the refusal names the missing interface");
}

// --- case b: Component ----------------------------------------------------

void component_geometry_follows_the_coordinate_type() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto button_path = node_path_named(harness, "Apply");
  const auto window_path = children_of(harness, std::string{atspi::kRootPath});
  if (!expect(button_path.has_value(), "the button has a path")) return;
  if (!expect(!window_path.empty(), "the application has a window")) return;
  const std::string window = window_path.front().path;
  const std::string path = *button_path;

  const auto extent = [&](const std::uint32_t coordinate) {
    return parse_rect(
        harness.ask(harness.embed_name(), path, "org.a11y.atspi.Component",
                    "GetExtents", [&](sd_bus_message *call) {
                      return sd_bus_message_append(call, "u", coordinate);
                    })
            .reply, "(iiii)");
  };
  const auto screen = extent(0);
  const auto window_space = extent(1);
  const auto parent_space = extent(2);
  const auto window_extents = parse_rect(harness.ask(
      harness.embed_name(), window, "org.a11y.atspi.Component", "GetExtents",
      [](sd_bus_message *call) {
        return sd_bus_message_append(call, "u", 0U);
      }).reply, "(iiii)");
  if (!expect(screen && window_space && parent_space && window_extents,
              "every coordinate type answers extents"))
    return;
  expect(screen->width > 0.0 && screen->height > 0.0,
         "a shown button has a size");
  expect(window_space->left == screen->left - window_extents->left &&
             window_space->top == screen->top - window_extents->top,
         "window extents are relative to the toplevel");
  expect(parent_space->left == window_space->left &&
             parent_space->top == window_space->top,
         "the button's parent is its window");

  const auto position = harness.ask(
      harness.embed_name(), path, "org.a11y.atspi.Component", "GetPosition",
      [](sd_bus_message *call) { return sd_bus_message_append(call, "u", 0U); });
  const auto size = harness.get(path, "org.a11y.atspi.Component", "GetSize");
  const auto origin = parse_pair(position.reply);
  const auto area = parse_pair(size.reply);
  expect(origin && *origin == std::pair{static_cast<std::int32_t>(screen->left),
                                        static_cast<std::int32_t>(screen->top)},
         "GetPosition matches the extents origin");
  expect(area && *area == std::pair{static_cast<std::int32_t>(screen->width),
                                    static_cast<std::int32_t>(screen->height)},
         "GetSize matches the extents size");

  const auto window_position = parse_pair(
      harness
          .ask(harness.embed_name(), path, "org.a11y.atspi.Component",
               "GetPosition",
               [](sd_bus_message *call) {
                 return sd_bus_message_append(call, "u", 1U);
               })
          .reply);
  expect(window_position &&
             *window_position == std::pair{static_cast<std::int32_t>(window_space->left),
                                           static_cast<std::int32_t>(window_space->top)},
         "GetPosition follows the coordinate type like GetExtents");

  const auto center_x = static_cast<std::int32_t>(screen->left + screen->width / 2);
  const auto center_y = static_cast<std::int32_t>(screen->top + screen->height / 2);
  const auto contains_point = harness.ask(
      harness.embed_name(), path, "org.a11y.atspi.Component", "Contains",
      [&](sd_bus_message *call) {
        return sd_bus_message_append(call, "iiu", center_x, center_y, 0U);
      });
  expect(parse_bool(contains_point.reply) == std::optional<bool>{true},
         "the center of a button is inside it");
  const auto outside = harness.ask(
      harness.embed_name(), path, "org.a11y.atspi.Component", "Contains",
      [&](sd_bus_message *call) {
        return sd_bus_message_append(call, "iiu",
                                     static_cast<std::int32_t>(screen->left) - 5,
                                     static_cast<std::int32_t>(screen->top) - 5,
                                     0U);
      });
  expect(parse_bool(outside.reply) == std::optional<bool>{false},
         "a point outside a button is outside it");

  // A hit test answers the deepest showing node under the point.
  const auto hit = harness.ask(
      harness.embed_name(), window, "org.a11y.atspi.Component",
      "GetAccessibleAtPoint", [&](sd_bus_message *call) {
        return sd_bus_message_append(call, "iiu", center_x, center_y, 0U);
      });
  const auto node = reply_reference(hit.reply);
  expect(node && node->path == path,
         "a hit test on the window answers the button");
  const auto missed = harness.ask(
      harness.embed_name(), window, "org.a11y.atspi.Component",
      "GetAccessibleAtPoint", [](sd_bus_message *call) {
        return sd_bus_message_append(call, "iiu", 4000, 4000, 0U);
      });
  const auto nothing = reply_reference(missed.reply);
  expect(nothing && nothing->path == std::string{atspi::kNullPath},
         "a hit test outside every widget answers the null reference");

  const auto layer = reply_u32(
      harness.get(path, "org.a11y.atspi.Component", "GetLayer").reply);
  expect(layer == std::optional<std::uint32_t>{3U}, "a button is a widget layer");
  const auto window_layer = reply_u32(
      harness.get(window, "org.a11y.atspi.Component", "GetLayer").reply);
  expect(window_layer == std::optional<std::uint32_t>{7U},
         "a toplevel is a window layer");
  const auto mdi =
      harness.get(path, "org.a11y.atspi.Component", "GetMDIZOrder");
  std::int16_t order = 0;
  expect(mdi.reply != nullptr && sd_bus_message_read(mdi.reply, "n", &order) >= 0 &&
             order == -1,
         "no widget sits in an MDI stack");
  const auto alpha =
      harness.get(path, "org.a11y.atspi.Component", "GetAlpha");
  expect(parse_double(alpha.reply) == std::optional<double>{1.0},
         "widgets are fully opaque");

  // Geometry is read-only: every setter and scroller refuses.
  const auto refused = [&](const std::string &member,
                           const std::function<int(sd_bus_message *)> &build) {
    return harness.ask(harness.embed_name(), path, "org.a11y.atspi.Component",
                       member, build);
  };
  expect(parse_bool(refused("SetExtents", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "iiiiu", 0, 0, 10, 10, 0U);
           }).reply) == std::optional<bool>{false},
         "SetExtents is refused");
  expect(parse_bool(refused("SetPosition", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "iiu", 0, 0, 0U);
           }).reply) == std::optional<bool>{false},
         "SetPosition is refused");
  expect(parse_bool(refused("SetSize", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "ii", 10, 10);
           }).reply) == std::optional<bool>{false},
         "SetSize is refused");
  expect(parse_bool(refused("ScrollTo", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "u", 1U);
           }).reply) == std::optional<bool>{false},
         "ScrollTo is refused");

  // Programmatic focus runs through the action layer, never the X server.
  expect(fixture.name->take_focus() != 0, "the field takes focus first");
  const auto grab = harness.get(path, "org.a11y.atspi.Component", "GrabFocus");
  expect(parse_bool(grab.reply) == std::optional<bool>{true},
         "GrabFocus on a focusable widget is accepted");
  expect(harness.spin_until([&] { return Fl::focus() == fixture.button; },
                            ms(2000)),
         "the widget holds keyboard focus");
}

// --- case c: Action -------------------------------------------------------

void action_names_and_activation_reach_the_widget() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto path = node_path_named(harness, "Apply");
  if (!expect(path.has_value(), "the button has a path")) return;

  expect(variant_int(harness.property(*path, "org.a11y.atspi.Action", "NActions")
                         .reply) == std::optional<std::int32_t>{1},
         "a button has one action");
  const auto named = [&](const std::string &member, const std::int32_t index) {
    return harness.ask(harness.embed_name(), *path, "org.a11y.atspi.Action",
                       member, [&](sd_bus_message *call) {
                         return sd_bus_message_append(call, "i", index);
                       });
  };
  expect(reply_text(named("GetName", 0).reply) == std::optional<std::string>{"activate"},
         "the action is named activate");
  expect(reply_text(named("GetLocalizedName", 0).reply) ==
             std::optional<std::string>{"activate"},
         "the localized name is activate");
  expect(reply_text(named("GetDescription", 0).reply) ==
             std::optional<std::string>{"Activate this control"},
         "the description matches the ATK adaptor's words");
  expect(reply_text(named("GetKeyBinding", 0).reply) ==
             std::optional<std::string>{""},
         "a labelled button has no key binding");

  const auto rows = actions_of(harness, *path);
  expect(rows.size() == 1 && std::get<0>(rows.front()) == "activate" &&
             std::get<1>(rows.front()) == "Activate this control",
         "GetActions lists the same action");

  int activations = 0;
  fixture.button->callback(+[](Fl_Widget *, void *data) {
    ++*static_cast<int *>(data);
  }, &activations);
  expect(parse_bool(named("DoAction", 0).reply) == std::optional<bool>{true},
         "DoAction is accepted");
  expect(harness.spin_until([&] { return activations == 1; }, ms(2000)),
         "the widget's own callback ran");
  expect(named("DoAction", 5).failed, "an out of range action is refused");

  // A disabled widget accepts no activation.
  fixture.button->deactivate();
  harness.spin(ms(250));
  expect(parse_bool(named("DoAction", 0).reply) == std::optional<bool>{false},
         "a disabled button refuses activation");
  fixture.button->activate();
  harness.spin(ms(250));
}

// --- case f: Value and Selection -----------------------------------------

void value_and_selection_mutations_reach_the_widgets() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto slider = node_path_named(harness, "Level");
  if (!expect(slider.has_value(), "the valuator has a path")) return;
  // The reply message lives in the CallResult, so the result is kept alive
  // until the value is parsed out of it.
  const auto value_property = [&](const std::string &name) {
    return harness.property(*slider, "org.a11y.atspi.Value", name);
  };
  expect(variant_double(value_property("CurrentValue").reply) ==
             std::optional<double>{4.0},
         "the current value is the widget's");
  expect(variant_double(value_property("MinimumValue").reply) ==
             std::optional<double>{0.0},
         "the minimum is the widget's");
  expect(variant_double(value_property("MaximumValue").reply) ==
             std::optional<double>{10.0},
         "the maximum is the widget's");
  expect(variant_double(value_property("MinimumIncrement").reply) ==
             std::optional<double>{0.5},
         "the increment is the widget's step");
  expect(variant_text(value_property("Text").reply) ==
             std::optional<std::string>{""},
         "a valuator exposes no value text");

  const auto set_value = [&](const double requested) {
    return harness.ask(harness.embed_name(), *slider, kPropertiesInterface,
                       "Set", [&](sd_bus_message *call) {
                         return sd_bus_message_append(
                             call, "ssv", "org.a11y.atspi.Value",
                             "CurrentValue", "d", requested);
                       });
  };
  expect(!set_value(7.5).failed, "a client sets the current value");
  expect(harness.spin_until(
             [&] { return std::abs(fixture.level->value() - 7.5) < 1e-9; },
             ms(2000)),
         "the widget took the value");
  expect(!set_value(99.0).failed, "an out of range value is clamped");
  expect(harness.spin_until(
             [&] { return std::abs(fixture.level->value() - 10.0) < 1e-9; },
             ms(2000)),
         "the widget stopped at its maximum");

  // Selection over a list: the published children are the list's rows.
  const auto list = node_path_named(harness, "Items");
  if (!expect(list.has_value(), "the list has a path")) return;
  const auto rows = children_of(harness, *list);
  if (!expect(rows.size() == 2, "the list publishes its two rows")) return;
  const auto selection_call = [&](const std::string &path,
                                  const std::string &member,
                                  const std::function<int(sd_bus_message *)> &build) {
    return harness.ask(harness.embed_name(), path, "org.a11y.atspi.Selection",
                       member, build);
  };
  expect(variant_int(harness.property(*list, "org.a11y.atspi.Selection",
                                      "NSelectedChildren")
                         .reply) == std::optional<std::int32_t>{0},
         "an untouched list selects nothing");
  expect(parse_bool(selection_call(*list, "SelectChild", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "i", 0);
           }).reply) == std::optional<bool>{true},
         "SelectChild is accepted");
  expect(harness.spin_until([&] { return fixture.items->value() == 1; },
                             ms(2000)),
         "the list chose the row");
  harness.spin(ms(250));
  expect(parse_bool(selection_call(*list, "IsChildSelected", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "i", 0);
           }).reply) == std::optional<bool>{true},
         "the row reports itself selected");
  expect(parse_bool(selection_call(*list, "IsChildSelected", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "i", 1);
           }).reply) == std::optional<bool>{false},
         "the other row is not selected");
  expect(variant_int(harness.property(*list, "org.a11y.atspi.Selection",
                                      "NSelectedChildren")
                         .reply) == std::optional<std::int32_t>{1},
         "the list reports one selected child");
  const auto chosen = reply_reference(
      selection_call(*list, "GetSelectedChild", [](sd_bus_message *call) {
        return sd_bus_message_append(call, "i", 0);
      }).reply);
  expect(chosen && chosen->path == rows.front().path,
         "the selected child is the row the list chose");
  expect(parse_bool(selection_call(*list, "SelectAll", {}).reply) ==
             std::optional<bool>{false},
         "a single-select list refuses SelectAll");
  expect(parse_bool(selection_call(*list, "ClearSelection", {}).reply) ==
             std::optional<bool>{false},
         "clearing a single selection is refused");
  expect(parse_bool(selection_call(*list, "DeselectChild", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "i", 0);
           }).reply) == std::optional<bool>{false},
         "deselecting one row is refused");

  // A radio group selects through its children's activation.
  const auto group = node_path_named(harness, "Mode");
  if (!expect(group.has_value(), "the radio group has a path")) return;
  expect(variant_int(harness.property(*group, "org.a11y.atspi.Selection",
                                      "NSelectedChildren")
                         .reply) == std::optional<std::int32_t>{1},
         "the radio group starts with its first button selected");
  expect(parse_bool(selection_call(*group, "SelectChild", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "i", 1);
           }).reply) == std::optional<bool>{true},
         "choosing another radio button is accepted");
  expect(harness.spin_until(
             [&] { return fixture.beta->value() == 1 && fixture.alpha->value() == 0; },
             ms(2000)),
         "the radio group moved its selection");
  harness.spin(ms(250));
  expect(parse_bool(selection_call(*group, "IsChildSelected", [](sd_bus_message *call) {
             return sd_bus_message_append(call, "i", 1);
           }).reply) == std::optional<bool>{true},
         "the chosen radio button reports itself selected");
  expect(parse_bool(selection_call(*group, "DeselectSelectedChild",
                                   [](sd_bus_message *call) {
                                     return sd_bus_message_append(call, "i", 1);
                                   }).reply) == std::optional<bool>{false},
         "a radio group cannot deselect without choosing another button");
}

void selection_changes_use_the_right_bus_event_and_filter() {
  constexpr const char *kTextInterface = "org.a11y.atspi.Text";
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"Object:SelectionChanged:"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;

  const auto field = node_path_named(harness, "Name");
  const auto list = node_path_named(harness, "Items");
  if (!expect(field && list, "the field and the list have paths")) return;
  const auto count = [&](const std::size_t mark, const std::string &path,
                         const std::string_view member) {
    const auto &signals = harness.signals();
    return std::count_if(signals.begin() + static_cast<std::ptrdiff_t>(mark),
                         signals.end(), [&](const Signal &signal) {
      return signal.sender == harness.embed_name() && signal.path == path &&
             signal.interface == "org.a11y.atspi.Event.Object" &&
             signal.member == member;
    });
  };
  const auto wait_for = [&](const std::size_t mark, const std::string &path,
                            const std::string_view member, const char *why) {
    expect(harness.spin_until([&] { return count(mark, path, member) > 0; },
                              ms(2000)),
           why);
  };

  // Publish a real text selection first with only the container filter.
  std::size_t mark = harness.signals().size();
  fixture.name->insert_position(3, 0);
  expect(harness.spin_until([&] {
           return reply_i32(harness.get(*field, kTextInterface, "GetNSelections").reply) ==
                  std::optional<std::int32_t>{1};
         }, ms(2000)),
         "the app-side field change was published");
  harness.spin(ms(250));
  expect(count(mark, *field, "SelectionChanged") == 0 &&
             count(mark, *field, "TextSelectionChanged") == 0,
         "a container listener does not receive text selections");

  harness.announce_listener("Object:TextSelectionChanged");
  // A read on the same connection follows the registration signal, so the
  // editor has processed the new filter before the next app-side change.
  (void)harness.get(*field, kTextInterface, "GetNSelections");
  mark = harness.signals().size();
  fixture.name->insert_position(7, 1);
  wait_for(mark, *field, "TextSelectionChanged", "the field reports its new range");
  expect(count(mark, *field, "SelectionChanged") == 0,
         "the field does not send the container event as well");

  mark = harness.signals().size();
  const auto removed = harness.ask(
      harness.embed_name(), *field, kTextInterface, "RemoveSelection",
      [](sd_bus_message *call) { return sd_bus_message_append(call, "i", 0); });
  expect(parse_bool(removed.reply) == std::optional<bool>{true},
         "the client can remove a text selection");
  wait_for(mark, *field, "TextSelectionChanged",
           "the client-initiated text change reaches listeners");

  mark = harness.signals().size();
  fixture.items->value(1);
  wait_for(mark, *list, "SelectionChanged", "the list reports the app-side choice");

  mark = harness.signals().size();
  const auto selected = harness.ask(
      harness.embed_name(), *list, "org.a11y.atspi.Selection", "SelectChild",
      [](sd_bus_message *call) { return sd_bus_message_append(call, "i", 1); });
  expect(parse_bool(selected.reply) == std::optional<bool>{true},
         "the client can select the second row");
  wait_for(mark, *list, "SelectionChanged", "the list reports the client's choice");

  mark = harness.signals().size();
  fixture.items->remove(2);
  wait_for(mark, *list, "SelectionChanged", "removing the selected row reports a change");
  expect(variant_int(harness.property(*list, "org.a11y.atspi.Selection",
                                      "NSelectedChildren").reply) ==
             std::optional<std::int32_t>{0},
         "the removed row is no longer selected");
  expect(count(0, *list, "TextSelectionChanged") == 0,
         "list changes never send the text event");
}

void numeric_changes_report_the_value_once_over_dbus() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"Object:PropertyChange:accessible-value"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;

  const auto slider = node_path_named(harness, "Level");
  if (!expect(slider.has_value(), "the valuator has a path")) return;
  const auto events = [&](const std::size_t mark) {
    std::vector<std::optional<double>> numbers;
    const auto &signals = harness.signals();
    for (std::size_t i = mark; i < signals.size(); ++i) {
      const auto &signal = signals[i];
      if (signal.sender == harness.embed_name() && signal.path == *slider &&
          signal.interface == "org.a11y.atspi.Event.Object" &&
          signal.member == "PropertyChange" && signal.name == "accessible-value")
        numbers.push_back(signal.number);
    }
    return numbers;
  };
  const auto check = [&](const std::size_t mark, const double expected) {
    expect(harness.spin_until([&] { return !events(mark).empty(); }, ms(2000)),
           "the exact accessible-value subscription receives the event");
    expect(variant_double(harness.property(*slider, "org.a11y.atspi.Value",
                                          "CurrentValue").reply) ==
               std::optional<double>{expected},
           "the valuator's new value is readable");
    // Flush another sampling interval to catch repeat notifications.
    harness.spin(ms(250));
    expect(events(mark) == std::vector<std::optional<double>>{expected},
           "one notification carries the new value as a D-Bus double");
  };

  std::size_t mark = harness.signals().size();
  fixture.level->value(5.5);
  check(mark, 5.5);

  mark = harness.signals().size();
  const auto set = harness.ask(harness.embed_name(), *slider, kPropertiesInterface,
                               "Set", [](sd_bus_message *call) {
    return sd_bus_message_append(call, "ssv", "org.a11y.atspi.Value",
                                 "CurrentValue", "d", 7.5);
  });
  expect(!set.failed, "a client sets the valuator");
  check(mark, 7.5);
}

// --- case p: an action is read back with its own effect -------------------

// A client that activates a control and then reads sees the generation its own
// action produced, without waiting for the next periodic walk.
void an_action_reads_back_in_the_generation_it_made() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto button = node_path_named(harness, "Apply");
  const auto label = node_path_named(harness, "Static");
  if (!expect(button.has_value() && label.has_value(),
              "the button and the label have paths"))
    return;

  // The button's own callback renames another widget; its node keeps its path.
  struct Renamed {
    Fl_Box *label{};
    int applied{};
  } renamed{fixture.label, 0};
  fixture.button->callback(
      [](Fl_Widget *, void *data) {
        auto *state = static_cast<Renamed *>(data);
        state->label->label("Applied");
        state->label->redraw();
        ++state->applied;
      },
      &renamed);

  const auto activated = harness.ask(
      harness.embed_name(), *button, "org.a11y.atspi.Action", "DoAction",
      [](sd_bus_message *call) { return sd_bus_message_append(call, "i", 0); });
  expect(parse_bool(activated.reply) == std::optional<bool>{true},
         "DoAction is accepted");
  expect(harness.spin_until([&] { return renamed.applied == 1; }, ms(2000)),
         "the widget's own callback ran");
  // No wait: the request below walks the generation the callback produced.
  expect(variant_text(harness.property(*label, kAccessibleInterface, "Name")
                          .reply) == std::optional<std::string>{"Applied"},
         "the read right after the action names the new label");
}

// --- case q: the walk budget ----------------------------------------------

// A listener-less client that polls a large tree pays at most one walk per
// refresh interval; its own actions would add one walk each.
void reads_without_a_listener_stay_within_the_walk_budget() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                    std::nullopt};
  RichFixture fixture;
  for (int row = 1; row <= 2000; ++row) {
    const std::string text = "row " + std::to_string(row);
    fixture.items->add(text.c_str());
  }
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto items = node_path_named(harness, "Items");
  if (!expect(items.has_value(), "the browser has a path")) return;
  const auto rows = children_of(harness, *items);
  if (!expect(rows.size() >= 300, "the browser exports its rows")) return;

  const auto walks = adaptor.walks_for_testing();
  const auto started = Clock::now();
  for (int read = 0; read < 300; ++read) {
    const auto &node = rows[static_cast<std::size_t>(read) % rows.size()];
    (void)harness.property(node.path, kAccessibleInterface, "Name");
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started);
  // The production interval is kRefreshInterval in adaptor.cpp: 100 ms.
  const auto budget = static_cast<std::uint64_t>(elapsed.count() / 100000) + 2;
  const auto taken = adaptor.walks_for_testing() - walks;
  std::fprintf(stderr, "[walks] %llu walks in %lld ms (budget %llu)\n",
               static_cast<unsigned long long>(taken),
               static_cast<long long>(elapsed.count() / 1000),
               static_cast<unsigned long long>(budget));
  expect(taken <= budget,
         "300 reads stay within one walk per refresh interval");
}

// --- case r: an idle listener does not keep walking ------------------------

// The periodic check only exists to publish app-side changes; a walk that just
// ran is not followed by another one until something changes or a request asks.
void an_idle_listener_does_not_keep_walking() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"object:"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                     std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(400));

  // Two warm-up waits settle the interval the embed left armed; then an idle
  // loop must sleep through each whole slice instead of waking for our timer.
  const auto walks = adaptor.walks_for_testing();
  const auto started = Clock::now();
  for (int warm = 0; warm < 2; ++warm) (void)Fl::wait(0.5);
  long long shortest = 5000;
  for (int pass = 0; pass < 3; ++pass) {
    const auto before = Clock::now();
    (void)Fl::wait(0.5);
    const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(
                               Clock::now() - before)
                               .count();
    std::fprintf(stderr, "[idle] wait %d took %lld ms\n", pass, took);
    if (took < shortest) shortest = took;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - started);
  const auto taken = adaptor.walks_for_testing() - walks;
  // The check and the refresh timer share one cadence (kRefreshInterval, 100 ms
  // in adaptor.cpp), so an idle listener costs exactly one walk per interval
  // and never two in one pass.
  const auto budget = static_cast<std::uint64_t>(elapsed.count() / 100000) + 2;
  std::fprintf(stderr, "[idle] %llu walks in %lld ms with a listener\n",
               static_cast<unsigned long long>(taken),
               static_cast<long long>(elapsed.count() / 1000));
  expect(taken <= budget, "an idle loop walks at most once per interval");
  // A wait lasts at least its slice unless something wakes the loop early, and
  // slowness can only lengthen it, so this holds under load too.
  expect(shortest >= 400, "an idle loop is not woken by the accessibility refresh");
}

// --- case s: an app-side change reaches the listener -----------------------

// A widget changed from an application timeout is published without any client
// request and without input, so a listener sees it within a refresh interval.
void an_app_change_reaches_the_listener() {
  layerc::test::require_x11();
  require_session_bus();
  Harness harness;
  require_private_bus(harness);
  harness.set_registered_events({"object:"});
  const layerc::test::ScopedEnvironment environment{"AT_SPI_BUS_ADDRESS",
                                                     std::nullopt};
  RichFixture fixture;
  atspi::Adaptor adaptor{fixture.tree};
  harness.set_flags(true, false);
  if (!expect(embed(harness, adaptor), "the application embeds")) return;
  harness.spin(ms(250));

  const auto mark = harness.signals().size();
  const auto started = Clock::now();
  Fl_Widget *field = fixture.name;
  Fl::add_timeout(0.05, [](void *data) { static_cast<Fl_Widget *>(data)->label("Later"); },
                  field);
  const bool arrived = harness.spin_until(
      [&] {
        const auto &signals = harness.signals();
        for (std::size_t i = mark; i < signals.size(); ++i) {
          const auto &signal = signals[i];
          if (signal.interface == "org.a11y.atspi.Event.Object" &&
              signal.member == "PropertyChange" && signal.name == "accessible-name" &&
              signal.text == "Later")
            return true;
        }
        return false;
      },
      ms(1500));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - started);
  std::fprintf(stderr, "[activity] the change reached the listener in %lld ms\n",
               static_cast<long long>(elapsed.count()));
  expect(arrived, "an app-side change reaches the listener");
  // Two production refresh intervals (kRefreshInterval is 100 ms).
  expect(elapsed.count() <= 400, "the change arrives within two refresh intervals");
}

} // namespace layerc::test::atspi_fixture
