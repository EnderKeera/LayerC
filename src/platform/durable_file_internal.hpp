#pragma once

#include <cstdint>

// Private seam between the shared durable_write.cpp and the per-platform
// durable_file.cpp translation units.

namespace layerc::platform::detail {

// Current process id, used to keep concurrent writers' stage names apart.
[[nodiscard]] std::uint64_t durable_process_id() noexcept;

} // namespace layerc::platform::detail
