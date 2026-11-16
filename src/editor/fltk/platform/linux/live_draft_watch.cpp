#include "platform/services.hpp"

#include <FL/Fl.H>

#include <utility>

namespace layerc::editor {

// The client's eventfd becomes readable only for publish replies and failures.
struct LiveDraftWatch::Impl {
  int fd{-1};
  std::function<void()> ready;
  static void readable(int, void *data) { static_cast<Impl *>(data)->ready(); }
};

LiveDraftWatch::LiveDraftWatch(const LiveDraftClient &client, std::function<void()> ready)
    : impl_(std::make_unique<Impl>()) {
  impl_->fd = client.notify_fd();
  impl_->ready = std::move(ready);
  Fl::add_fd(impl_->fd, FL_READ, Impl::readable, impl_.get());
}

LiveDraftWatch::~LiveDraftWatch() { Fl::remove_fd(impl_->fd); }

} // namespace layerc::editor
