// A controlled X11 window for installed layercal smoke tests.
#include <X11/Xlib.h>

#include <poll.h>
#include <signal.h>

#include <cstdio>

namespace {
volatile sig_atomic_t stopping = 0;

void stop(int) { stopping = 1; }
} // namespace

int main() {
  Display *display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    std::fputs("fixture: cannot open DISPLAY\n", stderr);
    return 1;
  }
  const int screen = DefaultScreen(display);
  const Window root = RootWindow(display, screen);
  const Window window = XCreateSimpleWindow(
      display, root, 40, 60, 400, 300, 0, BlackPixel(display, screen),
      BlackPixel(display, screen));
  XStoreName(display, window, "LayerC Automation Fixture");
  XSelectInput(display, window, ExposureMask | StructureNotifyMask);
  XMapWindow(display, window);
  const GC gc = XCreateGC(display, window, 0, nullptr);
  XSetForeground(display, gc, WhitePixel(display, screen));
  XFillRectangle(display, window, gc, 0, 0, 400, 300);
  XSetForeground(display, gc, BlackPixel(display, screen));
  XFillRectangle(display, window, gc, 0, 0, 100, 100);
  XSync(display, False);
  std::printf("%lu\n", window);
  std::fflush(stdout);

  (void)::signal(SIGTERM, stop);
  (void)::signal(SIGINT, stop);
  while (stopping == 0) {
    pollfd connection{ConnectionNumber(display), POLLIN, 0};
    (void)::poll(&connection, 1, 100);
    while (XPending(display) > 0) {
      XEvent event{};
      XNextEvent(display, &event);
      if (event.type == Expose && event.xexpose.count == 0) {
        XSetForeground(display, gc, WhitePixel(display, screen));
        XFillRectangle(display, window, gc, 0, 0, 400, 300);
        XSetForeground(display, gc, BlackPixel(display, screen));
        XFillRectangle(display, window, gc, 0, 0, 100, 100);
        XFlush(display);
      }
    }
  }
  XFreeGC(display, gc);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return 0;
}
