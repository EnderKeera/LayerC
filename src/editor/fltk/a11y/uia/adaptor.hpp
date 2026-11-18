#pragma once

#include "../model.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Fl_Widget;

namespace layerc::editor::a11y {

class FltkTree;

namespace uia {

// Whether UI Automation could publish the editor's tree, and, when it could
// not, why.
struct AdaptorError {
  std::string message;
};

// Publishes the FLTK tree to UI Automation on demand: no tree walk happens
// until a client asks for the root object, and walking stops again once no
// client talks to the editor. One FltkTree walk feeds every hosted window, so
// the whole application pays for one snapshot rather than one per window.
// Actions run through the shared toolkit action layer; none of them runs an
// application callback inside the call.
class Adaptor {
public:
  explicit Adaptor(FltkTree &tree);
  ~Adaptor();
  Adaptor(const Adaptor &) = delete;
  Adaptor &operator=(const Adaptor &) = delete;

  // Walks and publishes now, hosting every shown window: startup and tests.
  [[nodiscard]] std::optional<AdaptorError> sync();
  [[nodiscard]] std::optional<AdaptorError> error() const;
  // Raises a UI Automation notification on the source widget's window; every
  // call is its own occurrence, including a repeated identical message.
  void announce(Fl_Widget *source, std::string text);

  // Test seams: the number of tree walks, and the number of hosted windows.
  [[nodiscard]] std::uint64_t walks_for_testing() const;
  [[nodiscard]] std::size_t hosted_windows_for_testing() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace uia
} // namespace layerc::editor::a11y
