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

// Whether a registered listener covers an event type. Listener strings name
// ':'-separated components ("object", "state-changed", "focused"); a missing
// trailing component is a wildcard, and empty components are ignored, so
// "object:" and "object:state-changed" both cover "object:state-changed:focused".
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
