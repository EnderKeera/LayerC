#include "layerc/platform/durable_file.hpp"

namespace layerc::platform {

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

} // namespace layerc::platform
