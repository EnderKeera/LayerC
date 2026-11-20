#include "platform/services.hpp"

#include <FL/Fl.H>

#include <utility>

namespace layerc::editor {
namespace {

class LinuxStandaloneLiveDrafts final : public StandaloneLiveDrafts {
public:
  explicit LinuxStandaloneLiveDrafts(std::function<void()> ready)
      : ready_(std::move(ready)), fd_(client_.notify_fd()) {
    Fl::add_fd(fd_, FL_READ, readable, this);
  }

  ~LinuxStandaloneLiveDrafts() override { Fl::remove_fd(fd_); }

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
  int fd_;
};

} // namespace

bool standalone_live_drafts() noexcept { return true; }

StandaloneLiveDraftsResult
make_standalone_live_drafts(std::function<void()> ready) {
  std::unique_ptr<StandaloneLiveDrafts> service =
      std::make_unique<LinuxStandaloneLiveDrafts>(std::move(ready));
  return service;
}

} // namespace layerc::editor
