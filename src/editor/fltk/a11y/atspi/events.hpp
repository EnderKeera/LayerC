#pragma once

#include "a11y/atspi/loop_bus.hpp"
#include "a11y/atspi/objects.hpp"
#include "a11y/changes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor::a11y::atspi {

// One registration: an assistive technology on a bus asked for an event type.
// The bus is part of the identity so deregistering one listener leaves another
// registration of the same event intact.
struct Listener {
  std::string bus;
  std::string event;
};

// The spelling at-spi2-core compares event names in. The registry rewrites
// every registration (ensure_proper_format): each ':'-separated component is
// capitalized and a '-' is dropped with the letter after it capitalized, then
// the name is compared up to its first empty component. "object:text-changed"
// and the registry's own "Object:TextChanged:" are both "Object:TextChanged".
[[nodiscard]] std::string canonical_event(std::string_view name);

// Whether an event type falls under a registration pattern, in either
// spelling: every component of the pattern equals the event's, so a missing
// trailing component is a wildcard. "object:" and "Object:StateChanged" both
// cover "object:state-changed:focused"; an empty pattern covers every event,
// which is how the registry withdraws all of a departed listener's events.
[[nodiscard]] bool covers(std::string_view pattern, std::string_view event);

// Whether a registered listener covers an event type. A registration that
// names nothing selects nothing.
[[nodiscard]] bool listens(const std::vector<Listener> &listeners,
                           std::string_view event);

// Emits the protocol events for one refresh: the AT-SPI event stream from the
// given differences, and the cache's own announcements. When listeners is null
// the registry has not answered yet and every event is sent.
void emit_events(LoopBus &loop, const ObjectSource &source,
                 const Snapshot &before, const Snapshot &after,
                 const std::vector<Change> &changes,
                 const std::vector<Listener> *listeners);

// Sent unconditionally: the cache mirrors the tree for every client.
void emit_cache_added(LoopBus &loop, const ObjectSource &source, NodeId id);
void emit_cache_removed(LoopBus &loop, const ObjectSource &source, NodeId id);

} // namespace layerc::editor::a11y::atspi
