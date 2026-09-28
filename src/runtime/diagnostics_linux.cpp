#include "layerc/runtime/diagnostics.hpp"

#include "layerc/core/json.hpp"

namespace layerc::runtime {
namespace {
namespace json = core::json;

void append_extension(std::string &out, std::string_view name,
                      const X11ExtensionStatus &extension,
                      std::string_view connection_reason) {
  out += ",";
  json::append_quoted(out, name);
  out += ":{\"available\":";
  json::append_bool(out, extension.available);
  if (extension.available) {
    out += ",\"major\":";
    json::append_unsigned(out, static_cast<std::uint64_t>(extension.major));
    out += ",\"minor\":";
    json::append_unsigned(out, static_cast<std::uint64_t>(extension.minor));
  } else {
    out += ",\"reason\":";
    json::append_quoted(out, connection_reason.empty() ? "extension_missing"
                                                       : connection_reason);
  }
  out += "}";
}

} // namespace

std::string serialize_linux_status_json(const RuntimeDiagnostics &diagnostics,
                                        const LinuxStatusSnapshot &linux) {
  // Compose only the native probe/availability extension. Process, service,
  // resource, fault, and version fields use the same schema as Windows.
  std::string out = serialize_runtime_diagnostics_json(diagnostics);
  out.pop_back();
  out += ",\"x11\":{\"connected\":";
  json::append_bool(out, linux.x11.connected);
  if (!linux.x11.reason.empty()) {
    out += ",\"reason\":";
    json::append_quoted(out, linux.x11.reason);
  }
  append_extension(out, "xi2", linux.x11.xi2, linux.x11.reason);
  append_extension(out, "shape", linux.x11.shape, linux.x11.reason);
  append_extension(out, "randr", linux.x11.randr, linux.x11.reason);
  append_extension(out, "composite", linux.x11.composite, linux.x11.reason);
  append_extension(out, "damage", linux.x11.damage, linux.x11.reason);
  append_extension(out, "xfixes", linux.x11.xfixes, linux.x11.reason);
  out += ",\"ewmh\":{\"available\":";
  json::append_bool(out, linux.x11.ewmh);
  if (!linux.x11.ewmh) {
    out += ",\"reason\":";
    json::append_quoted(out, linux.x11.reason.empty() ? "extension_missing"
                                                      : linux.x11.reason);
  }
  out += "}},\"capabilities\":[";
  json::Join capabilities;
  for (const auto &status : linux.capabilities) {
    capabilities(out);
    out += "{\"capability\":";
    json::append_quoted(out, capability_name(status.capability));
    out += ",\"availability\":";
    json::append_quoted(out, availability_name(status.availability));
    out += ",\"reasons\":[";
    json::Join reasons;
    for (const auto &reason : status.reasons) {
      reasons(out);
      json::append_quoted(out, reason);
    }
    out += "]";
    if (!status.diagnostics.empty()) {
      out += ",\"diagnostics\":[";
      json::Join details_join;
      for (const auto &detail : status.diagnostics) {
        details_join(out);
        json::append_quoted(out, detail);
      }
      out += "]";
    }
    out += "}";
  }
  out += "]}";
  return out;
}

} // namespace layerc::runtime
