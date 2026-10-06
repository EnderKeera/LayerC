#pragma once

#include "layerc/platform/profile_file_guard.hpp"

namespace layerc::platform {

// Linux-only shared owner/mode and component-symlink gate for store and
// control listener directory setup. `repair` creates missing parents and
// tightens an owned directory to 0700; `require_private` creates only the
// leaf (0700) and refuses an existing directory already open to others, so
// a pre-planted permissive runtime directory is never adopted.
enum class OwnedDirectoryPolicy { repair, require_private };
[[nodiscard]] bool ensure_owned_directory(
    const std::filesystem::path &path,
    OwnedDirectoryPolicy policy = OwnedDirectoryPolicy::repair);

} // namespace layerc::platform
