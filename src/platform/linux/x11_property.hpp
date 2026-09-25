#pragma once

// Owned XGetWindowProperty reads shared by EWMH window tracking and the
// display probe. Callers own error trapping; a failed read yields no data.
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace layerc::platform::x11_property {

inline constexpr unsigned long no_length = ~0UL;

struct RawProperty {
  Atom type{None};
  int format{};
  unsigned long count{};
  unsigned char *data{};

  ~RawProperty() {
    if (data != nullptr)
      XFree(data);
  }
  RawProperty() = default;
  RawProperty(const RawProperty &) = delete;
  RawProperty &operator=(const RawProperty &) = delete;
  RawProperty(RawProperty &&other) noexcept
      : type(other.type), format(other.format), count(other.count),
        data(other.data) {
    other.data = nullptr;
  }
  RawProperty &operator=(RawProperty &&) = delete;
};

[[nodiscard]] inline RawProperty read_property(Display *display,
                                               ::Window window, Atom atom,
                                               Atom type) noexcept {
  RawProperty property;
  unsigned long remaining{};
  if (atom == None)
    return property;
  if (XGetWindowProperty(display, window, atom, 0, no_length, False, type,
                         &property.type, &property.format, &property.count,
                         &remaining, &property.data) != Success)
    property.data = nullptr;
  return property;
}

[[nodiscard]] inline std::optional<std::string>
read_text_property(Display *display, ::Window window, Atom atom) noexcept {
  RawProperty property = read_property(display, window, atom, AnyPropertyType);
  if (property.data == nullptr || property.format != 8 || property.count == 0)
    return std::nullopt;
  return std::string(reinterpret_cast<const char *>(property.data),
                     static_cast<std::size_t>(property.count));
}

[[nodiscard]] inline std::optional<std::uint32_t>
read_cardinal(Display *display, ::Window window, Atom atom) noexcept {
  RawProperty property = read_property(display, window, atom, XA_CARDINAL);
  if (property.data == nullptr || property.format != 32 || property.count == 0)
    return std::nullopt;
  const auto *values = reinterpret_cast<const unsigned long *>(property.data);
  return static_cast<std::uint32_t>(values[0]);
}

[[nodiscard]] inline std::vector<std::uint64_t>
read_atom_list(Display *display, ::Window window, Atom atom) noexcept {
  std::vector<std::uint64_t> values;
  RawProperty property = read_property(display, window, atom, XA_ATOM);
  if (property.data == nullptr || property.format != 32)
    return values;
  const auto *raw = reinterpret_cast<const Atom *>(property.data);
  values.assign(raw, raw + property.count);
  return values;
}

[[nodiscard]] inline std::vector<::Window>
read_window_list(Display *display, ::Window window, Atom atom) noexcept {
  std::vector<::Window> values;
  RawProperty property = read_property(display, window, atom, XA_WINDOW);
  if (property.data == nullptr || property.format != 32)
    return values;
  const auto *raw = reinterpret_cast<const ::Window *>(property.data);
  values.assign(raw, raw + property.count);
  return values;
}

} // namespace layerc::platform::x11_property
