#pragma once

#include "../model.hpp"

#include <memory>
#include <optional>
#include <string>

class Fl_Widget;
class Fl_Window;

namespace layerc::editor::a11y::uia {

struct BridgeError { std::string message; };

// Attach after show(); destroy before the Fl_Window. A hidden/destroyed HWND
// disconnects providers permanently; create a fresh bridge after re-showing.
class Bridge {
public:
  explicit Bridge(Fl_Window &window);
  ~Bridge();
  Bridge(const Bridge &) = delete;
  Bridge &operator=(const Bridge &) = delete;
  void bind(Fl_Widget &widget, Role role, std::string name,
            std::string description = {});
  [[nodiscard]] std::optional<BridgeError> sync();
  [[nodiscard]] std::optional<BridgeError> error() const;
  void announce(std::string text);
private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace layerc::editor::a11y::uia
