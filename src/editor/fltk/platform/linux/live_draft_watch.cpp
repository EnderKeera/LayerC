#include "platform/services.hpp"
#include "fltk_fd.hpp"

#include <FL/Fl.H>

#include <utility>

namespace layerc::editor {
namespace {

class LinuxStandaloneLiveDrafts final : public StandaloneLiveDrafts {
public:
  explicit LinuxStandaloneLiveDrafts(std::function<void()> ready) : ready_(std::move(ready)) {}

  ~LinuxStandaloneLiveDrafts() override {
    if (fd_ >= 0) Fl::remove_fd(fd_);
  }

  [[nodiscard]] std::optional<StandaloneLiveDraftsUnavailable> watch() {
    const int fd = client_.notify_fd();
    if (fd < 0) return StandaloneLiveDraftsUnavailable{"the live draft notification could not be created"};
    if (!fltk_fd_watchable(fd))
      return StandaloneLiveDraftsUnavailable{"the live draft notification is outside the range LayerE can watch"};
    fd_ = fd;
    Fl::add_fd(fd_, FL_READ, readable, this);
    return std::nullopt;
  }

  [[nodiscard]] std::uint64_t publish(std::string profile_id,
                                      std::string profile_json) override {
    return client_.publish(std::move(profile_id), std::move(profile_json));
  }
  void end() override { client_.end(); }
  [[nodiscard]] std::optional<LiveDraftUpdate> take_update() override {
    return client_.take_update();
  }

private:
  static void readable(int, void *data) {
    auto &self = *static_cast<LinuxStandaloneLiveDrafts *>(data);
    if (self.ready_) self.ready_();
  }

  LiveDraftClient client_;
  std::function<void()> ready_;
  int fd_{-1};
};

} // namespace

bool standalone_live_drafts() noexcept { return true; }

StandaloneLiveDraftsResult
make_standalone_live_drafts(std::function<void()> ready) {
  auto service = std::make_unique<LinuxStandaloneLiveDrafts>(std::move(ready));
  if (auto unavailable = service->watch()) return std::move(*unavailable);
  return std::unique_ptr<StandaloneLiveDrafts>(std::move(service));
}

} // namespace layerc::editor
