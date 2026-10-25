#include "editor_app.hpp"
#include "layerc/core/version.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <cstdio>
#include <cstdlib>
#include <string_view>

int main(const int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::puts("layere — Linux X11 profile editor\n"
              "Uses the normal XDG profile store. --help, --version.");
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::puts(layerc::core::product_version.data());
    return 0;
  }
  if (argc != 1) {
    std::fputs("Unknown option; use layere --help.\n", stderr);
    return 64;
  }
  const auto *display = std::getenv("DISPLAY");
  if (!display || !*display) {
    std::fputs("LayerE requires an X11 display; Wayland is not supported.\n", stderr);
    return 2;
  }
  // Select X11 before opening any window, regardless of ambient FLTK_BACKEND.
  // Native Wayland remains outside v2.6c.
  if (::setenv("FLTK_BACKEND", "x11", 1) != 0) {
    std::fputs("LayerE could not select its required X11 backend.\n", stderr);
    return 2;
  }
  // The window class names the installed desktop entry, so desktops pair the
  // window with its launcher and icon.
  Fl_Window::default_xclass("io.github.EnderKeera.LayerC");
  layerc::editor::EditorApp app;
  app.build();
  app.show();
  return Fl::run();
}
