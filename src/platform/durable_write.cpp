#include "layerc/platform/durable_file.hpp"

#include <atomic>
#include <string>

namespace layerc::platform {
namespace {

std::atomic_uint64_t stage_sequence{};

} // namespace

DurablePublishOutcome write_durable_file(
    const std::filesystem::path &destination,
    const std::span<const std::byte> contents) {
  const auto stage = unique_durable_stage_path(destination);
  if (!create_durable_file(stage, contents))
    return DurablePublishOutcome::failed;
  const auto outcome = publish_durable_file_checked(stage, destination);
  if (outcome == DurablePublishOutcome::failed)
    (void)remove_durable_file(stage);
  return outcome;
}

bool fsync_parent_checked(const std::filesystem::path &path) noexcept {
  return fsync_directory_checked(path.parent_path());
}

bool flush_directory_hierarchy(std::filesystem::path directory) {
  while (!directory.empty()) {
    if (!fsync_directory_checked(directory)) return false;
    const auto parent = directory.parent_path();
    if (parent == directory) break;
    directory = parent;
  }
  return true;
}

std::filesystem::path
unique_durable_stage_path(const std::filesystem::path &destination) {
  auto stage = destination;
  stage += "." + std::to_string(current_process_id()) + "." +
           std::to_string(
               stage_sequence.fetch_add(1, std::memory_order_relaxed)) +
           ".tmp";
  return stage;
}

} // namespace layerc::platform
