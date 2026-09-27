// Writes the launch-time updater manifest for one install tree.
//
// Usage: layerc_manifest --root <install-dir> --channel <name>
//            --sequence <n> --commit <40-hex> --output <manifest.json>
//
// Every regular file under --root becomes one entry with its size and SHA-256.
// The output is the exact byte sequence CI signs; the launcher verifies the
// detached signature over these bytes before trusting any entry.

#include "layerc/core/hash.hpp"
#include "layerc/core/update_manifest.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using layerc::core::UpdateFile;
using layerc::core::UpdateManifest;

struct Options {
  fs::path root;
  fs::path output;
  std::string channel;
  std::string commit;
  std::uint64_t sequence{};
};

int usage() {
  std::fputs("Usage: layerc_manifest --root <dir> --channel <name> "
             "--sequence <n> --commit <40-hex> --output <file>\n",
             stderr);
  return 64;
}

bool parse_options(const int argc, char **argv, Options &options) {
  bool have_sequence = false;
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::string_view name = argv[index];
    const std::string_view value = argv[index + 1];
    if (name == "--root") {
      options.root = fs::path(value);
    } else if (name == "--output") {
      options.output = fs::path(value);
    } else if (name == "--channel") {
      options.channel = value;
    } else if (name == "--commit") {
      options.commit = value;
    } else if (name == "--sequence") {
      const auto *end = value.data() + value.size();
      const auto result =
          std::from_chars(value.data(), end, options.sequence);
      if (result.ec != std::errc{} || result.ptr != end) return false;
      have_sequence = true;
    } else {
      return false;
    }
  }
  return argc % 2 == 1 && have_sequence && !options.root.empty() &&
         !options.output.empty();
}

// Groups files for diagnostics: each program is its own module, DLLs are the
// shared runtime, licenses stay together, and everything else is docs.
std::string module_for(const std::string &path) {
  if (path.starts_with("licenses/")) return "licenses";
  const auto dot = path.rfind('.');
  std::string extension = dot == std::string::npos ? "" : path.substr(dot);
  std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (extension == ".dll") return "runtime";
  if (extension != ".exe") return "docs";
  const auto slash = path.rfind('/');
  const auto start = slash == std::string::npos ? 0 : slash + 1;
  std::string stem = path.substr(start, dot - start);
  std::ranges::transform(stem, stem.begin(), [](unsigned char value) {
    return value == '_' ? '-' : static_cast<char>(std::tolower(value));
  });
  return layerc::core::is_update_name(stem) ? stem : "app";
}

bool read_bytes(const fs::path &path, std::vector<std::byte> &bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  const std::vector<char> data((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
  if (input.bad()) return false;
  bytes.resize(data.size());
  std::ranges::transform(data, bytes.begin(),
                         [](char value) { return static_cast<std::byte>(value); });
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) return usage();

  UpdateManifest manifest;
  manifest.channel = options.channel;
  manifest.sequence = options.sequence;
  manifest.commit = options.commit;

  std::error_code error;
  for (fs::recursive_directory_iterator it(options.root, error), end;
       !error && it != end; it.increment(error)) {
    if (it->is_symlink()) {
      std::fprintf(stderr, "Refusing symlink in install tree: %s\n",
                   it->path().generic_string().c_str());
      return 1;
    }
    if (!it->is_regular_file()) continue;
    const auto relative = it->path().lexically_relative(options.root);
    const auto path_text = relative.generic_u8string();
    UpdateFile file;
    file.path.assign(path_text.begin(), path_text.end());
    file.module = module_for(file.path);
    std::vector<std::byte> bytes;
    if (!read_bytes(it->path(), bytes)) {
      std::fprintf(stderr, "Could not read %s\n", file.path.c_str());
      return 1;
    }
    file.size = bytes.size();
    file.sha256 = layerc::core::sha256(bytes);
    manifest.files.push_back(std::move(file));
  }
  if (error) {
    std::fprintf(stderr, "Could not walk %s: %s\n",
                 options.root.generic_string().c_str(), error.message().c_str());
    return 1;
  }
  std::ranges::sort(manifest.files, {}, &UpdateFile::path);

  if (const auto invalid = layerc::core::validate_update_manifest(manifest)) {
    std::fprintf(stderr, "Invalid manifest at %s: %s\n", invalid->path.c_str(),
                 invalid->message.c_str());
    return 1;
  }
  const auto json = layerc::core::serialize_update_manifest_json(manifest);
  std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
  output.write(json.data(), static_cast<std::streamsize>(json.size()));
  output.close();
  if (!output) {
    std::fprintf(stderr, "Could not write %s\n",
                 options.output.generic_string().c_str());
    return 1;
  }
  std::printf("%zu files, sequence %llu\n", manifest.files.size(),
              static_cast<unsigned long long>(manifest.sequence));
  return 0;
}
