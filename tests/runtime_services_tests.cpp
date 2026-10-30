#include "layerc/runtime/daemon_kernel.hpp"
#include "layerc/runtime/diagnostics.hpp"
#include "test_support.hpp"

#include <iostream>
#include <limits>
#include <string>
#include <vector>

void diagnostics_display_contracts();
void diagnostics_overlay_fault_contracts();

namespace {

using layerc::test::expect;
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::IRuntimeService;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::RuntimeServiceRegistry;
using layerc::runtime::ServiceReconcileResult;

class FakeService final : public IRuntimeService {
public:
  explicit FakeService(const Capability capability) noexcept
      : capability_(capability) {}

  [[nodiscard]] Capability capability() const noexcept { return capability_; }

  [[nodiscard]] ServiceReconcileResult
  reconcile(const CapabilityDemand &demand) override {
    ++reconciled;
    saw_demand = demand.consumers(capability_);
    if (fail) return ServiceReconcileResult::failed("test fault");
    return diagnostic.empty() ? ServiceReconcileResult{}
                              : ServiceReconcileResult::degraded(diagnostic);
  }

  void quiesce() noexcept override {
    ++quiesced;
    if (quiesce_order != nullptr) quiesce_order->push_back(capability_);
  }

  [[nodiscard]] RuntimeResourceStats resources() const noexcept override {
    return {.threads = 1, .capture_sessions = 1};
  }

  int reconciled{};
  int quiesced{};
  std::uint32_t saw_demand{};
  bool fail{};
  std::string diagnostic;
  std::vector<Capability> *quiesce_order{};

private:
  Capability capability_;
};

template <typename... Services>
[[nodiscard]] RuntimeServiceRegistry registry(Services &...services) {
  RuntimeServiceRegistry result{};
  const auto add = [&](auto &service) {
    result[static_cast<std::size_t>(service.capability())] = &service;
  };
  (add(services), ...);
  return result;
}

void demand_counts_saturate() {
  CapabilityDemand demand;
  demand.add(Capability::input, 2);
  demand.add(Capability::input, 3);
  expect(demand.consumers(Capability::input) == 5,
         "capability consumers should accumulate");
  expect(demand.required(Capability::input),
         "non-zero demand should be required");
  expect(!demand.empty(), "non-zero demand should not be empty");
  demand.set(Capability::input, 0);
  expect(demand.empty(), "cleared demand should be empty");
}

void host_is_lazy_and_idempotent() {
  FakeService service(Capability::visual);
  layerc::runtime::RuntimeServiceHost host(registry(service));
  CapabilityDemand demand;

  expect(host.reconcile(demand), "empty demand should reconcile");
  expect(service.reconciled == 0, "empty demand must not run a service");

  demand.set(Capability::visual, 2);
  expect(host.reconcile(demand), "active demand should reconcile");
  expect(service.reconciled == 1 && service.saw_demand == 2,
         "first demand should reach the typed service");

  service.diagnostic = "degraded but available";
  expect(host.reconcile(demand), "repeated demand should reconcile");
  expect(service.reconciled == 2 &&
             host.stats(Capability::visual).activation_count == 1,
         "repeated demand must reuse the registered service");

  const auto stats = host.stats(Capability::visual);
  expect(stats.state == layerc::runtime::ServiceState::active &&
             stats.consumers == 2 && stats.resources.threads == 1 &&
             stats.last_error == "degraded but available",
         "active statistics should retain non-fatal diagnostics");

  demand.set(Capability::visual, 0);
  expect(host.reconcile(demand), "zero demand should quiesce successfully");
  expect(service.quiesced == 1 && host.find(Capability::visual) == nullptr,
         "zero demand must quiesce and deactivate the service");
  expect(host.stats(Capability::visual).state ==
             layerc::runtime::ServiceState::absent,
         "quiesced service should report absent");
}

void faults_release_the_service() {
  FakeService service(Capability::visual);
  service.fail = true;
  layerc::runtime::RuntimeServiceHost host(registry(service));
  CapabilityDemand demand;
  demand.set(Capability::visual, 1);

  expect(!host.reconcile(demand),
         "service failure should fail reconciliation");
  const auto stats = host.stats(Capability::visual);
  expect(service.quiesced == 1 && host.find(Capability::visual) == nullptr,
         "failed service should be quiesced and deactivated");
  expect(stats.state == layerc::runtime::ServiceState::faulted &&
             stats.fault_count == 1 && stats.last_error == "test fault",
         "failure diagnostics should be retained");
}

void later_fault_keeps_healthy_capabilities_active() {
  FakeService input(Capability::input);
  FakeService visual(Capability::visual);
  visual.fail = true;
  layerc::runtime::RuntimeServiceHost host(registry(input, visual));
  CapabilityDemand demand;
  demand.set(Capability::input, 1);
  demand.set(Capability::visual, 1);

  expect(!host.reconcile(demand),
         "a later service fault should fail the capability transition");
  expect(input.quiesced == 0 && visual.quiesced == 1 &&
             host.find(Capability::input) != nullptr &&
             host.find(Capability::visual) == nullptr,
         "an unrelated healthy capability must survive an optional fault");

  visual.fail = false;
  expect(host.reconcile(demand) &&
             host.stats(Capability::input).activation_count == 1 &&
             host.stats(Capability::visual).activation_count == 2,
         "repair should reactivate only the failed capability");
}

void unavailable_isolated_capture_leaves_input_active() {
  FakeService input(Capability::input);
  layerc::runtime::RuntimeServiceHost host(registry(input));
  CapabilityDemand demand;
  demand.set(Capability::input, 1);
  demand.set(Capability::isolated_visual, 1);

  expect(!host.reconcile(demand),
         "a missing isolated-capture service should fail the transition");
  expect(host.find(Capability::input) != nullptr &&
             host.stats(Capability::input).state ==
                 layerc::runtime::ServiceState::active,
         "unavailable isolated capture must not tear down input");
  expect(host.stats(Capability::isolated_visual).state ==
             layerc::runtime::ServiceState::faulted,
         "the unavailable capability should report its own fault");
}

void teardown_is_reverse_ordered() {
  std::vector<Capability> order;
  FakeService input(Capability::input);
  input.quiesce_order = &order;
  FakeService visual(Capability::visual);
  visual.quiesce_order = &order;
  layerc::runtime::RuntimeServiceHost host(registry(input, visual));
  CapabilityDemand demand;
  demand.set(Capability::input, 1);
  demand.set(Capability::visual, 1);
  expect(host.reconcile(demand), "multiple services should activate");
  demand = {};
  expect(host.reconcile(demand), "multiple services should quiesce");
  expect(order.size() == 2 && order[0] == Capability::visual &&
             order[1] == Capability::input,
         "services should quiesce in reverse dependency order");
}

void repeated_activation_has_bounded_ownership() {
  FakeService service(Capability::visual);
  layerc::runtime::RuntimeServiceHost host(registry(service));
  CapabilityDemand demand;
  for (int cycle = 0; cycle < 100; ++cycle) {
    demand.set(Capability::visual, 1);
    expect(host.reconcile(demand), "activation cycle should succeed");
    demand.set(Capability::visual, 0);
    expect(host.reconcile(demand), "quiescence cycle should succeed");
  }
  const auto stats = host.stats(Capability::visual);
  expect(service.reconciled == 100 && service.quiesced == 100,
         "every activation should have one matching quiescence");
  expect(stats.activation_count == 100 &&
             stats.state == layerc::runtime::ServiceState::absent &&
             stats.resources == RuntimeResourceStats{},
         "inactive lifecycle statistics retain counts but no resources");
}

void resource_ledger_covers_non_lifecycle_resources() {
  layerc::runtime::RuntimeServiceHost host;
  host.set_resource_ledger({.handles = 2, .objects = 3});
  const auto resources = host.aggregate_resources();
  expect(resources.handles == 2 && resources.objects == 3,
         "auxiliary resources should not require a fake lifecycle service");
}

void demand_matrix_is_explicit() {
  using layerc::runtime::SessionCapabilityRequest;
  const auto calculate = [](std::initializer_list<SessionCapabilityRequest> s,
                            const bool edit = false) {
    return layerc::runtime::calculate_capability_demand({s.begin(), s.size()},
                                                        edit);
  };

  expect(calculate({}).empty(), "no game should demand no service");
  expect(calculate({{.window = {1}, .active = false, .profile_id = "idle"}}).empty(),
         "a background non-global game should demand no service");
  auto background = calculate({{.window = {1},
                                .active = false,
                                .automation = true,
                                .companion = true,
                                .profile_id = "background"}});
  expect(background.consumers(Capability::automation) == 1 &&
             background.consumers(Capability::gamebar) == 1 &&
             !background.required(Capability::input) &&
             !background.required(Capability::overlay),
         "background automation and companion sessions keep only their "
         "own services demanded");

  auto demand =
      calculate({{.window = {1}, .active = true, .profile_id = "foreground"}});
  expect(demand.consumers(Capability::input) == 1 &&
             demand.consumers(Capability::overlay) == 1 &&
             !demand.required(Capability::visual),
         "input-only foreground game should own input and overlay only");

  demand = calculate({{.window = {1},
                       .active = true,
                       .global = true,
                       .visual = true,
                       .profile_id = "global"}});
  expect(demand.required(Capability::input) &&
             !demand.required(Capability::visual),
         "global profiles should not activate capture");

  demand = calculate({{.window = {1},
                       .active = true,
                       .visual = true,
                       .generic_hid = true,
                       .profile_id = "visual"}});
  expect(demand.required(Capability::visual) &&
             demand.required(Capability::generic_hid),
         "visual and HID consumers should activate their lazy services");

  demand = calculate({
      {.window = {9}, .active = true, .visual = true, .profile_id = "inline"},
      {.window = {9}, .active = true, .visual = true, .isolated_visual = true,
       .profile_id = "isolated"},
  });
  expect(demand.consumers(Capability::isolated_visual) == 2 &&
             !demand.required(Capability::visual),
         "one isolated profile should route every profile on its window "
         "through one isolated tier");
  const auto routed = demand.visual_consumers();
  expect(routed.size() == 2 && routed[0].profile_id == "inline" &&
             routed[1].profile_id == "isolated" &&
             routed[0].runtime == layerc::core::VisualRuntime::isolated &&
             routed[1].runtime == layerc::core::VisualRuntime::isolated,
         "capture owners receive the kernel's selected consumers and modes");

  demand = calculate({
      {.window = {9}, .active = true, .visual = true, .profile_id = "inline"},
      {.window = {9}, .active = false, .visual = true, .isolated_visual = true,
       .profile_id = "inactive"},
      {.window = {9}, .active = true, .global = true, .visual = true,
       .isolated_visual = true, .profile_id = "global"},
  });
  expect(demand.visual_consumers().size() == 1 &&
             demand.visual_consumers()[0].runtime ==
                 layerc::core::VisualRuntime::inline_process,
         "inactive or global consumers cannot switch a window to isolation");

  const auto changed_profile = calculate({
      {.window = {9}, .active = true, .visual = true, .profile_id = "replacement"},
      {.window = {9}, .active = false, .visual = true, .isolated_visual = true,
       .profile_id = "inactive"},
      {.window = {9}, .active = true, .global = true, .visual = true,
       .isolated_visual = true, .profile_id = "global"},
  });
  expect(demand != changed_profile,
         "demand identity includes consumers even when resource counts match");

  expect(calculate({}, false).empty(),
         "an editor connection alone owns no runtime lifecycle");
  demand = calculate({}, true);
  expect(demand.required(Capability::overlay),
         "an active edit overlay should demand presentation");
}

class ReentrantService final : public IRuntimeService {
public:
  layerc::runtime::DaemonKernel *kernel{};
  bool notified{};
  int quiesced{};

  [[nodiscard]] static Capability capability() noexcept {
    return Capability::visual;
  }
  [[nodiscard]] ServiceReconcileResult
  reconcile(const CapabilityDemand &) override {
    if (!notified) {
      notified = true;
      (void)kernel->reconcile({});
    }
    return {};
  }
  void quiesce() noexcept override { ++quiesced; }
  [[nodiscard]] RuntimeResourceStats resources() const noexcept override {
    return {.threads = 1};
  }
};

void kernel_serializes_reentrant_demand() {
  ReentrantService service;
  layerc::runtime::DaemonKernel kernel(registry(service));
  service.kernel = &kernel;
  CapabilityDemand demand;
  demand.set(Capability::visual, 1);
  expect(kernel.reconcile(demand),
         "kernel should serialize demand raised during reconcile");
  expect(service.notified && service.quiesced == 1 &&
             kernel.demand().empty() &&
             kernel.services().find(Capability::visual) == nullptr &&
             kernel.demand_generation() == 2,
         "latest reentrant demand should win and release the active service");
}

void diagnostics_contract_round_trips() {
  using namespace std::chrono_literals;
  layerc::runtime::RuntimeDiagnostics diagnostics;
  diagnostics.package_version = "test-package";
  diagnostics.process = {.pid = 41,
                         .private_bytes = 42,
                         .working_set_bytes = 43,
                         .handles = 44,
                         .threads = 45,
                         .kernel_time_100ns = 46,
                         .user_time_100ns = 47};
  diagnostics.services.push_back({
      .capability = Capability::visual,
      .state = layerc::runtime::ServiceState::faulted,
      .consumers = 2,
      .resources = {.threads = 1,
                    .handles = 2,
                    .capture_sessions = 3,
                    .decoded_assets = 4,
                    .cache_bytes = std::numeric_limits<std::uint64_t>::max()},
      .activation_count = 5,
      .fault_count = 6,
      .last_activation = layerc::core::TimePoint{} + 17ns,
      .last_quiescence = std::nullopt,
      .last_error = "capture fault",
  });
  diagnostics.gamebar = {.connected = true, .last_error = "transport warning"};
  diagnostics.settings = {.capture_exclusion = true,
                          .visual_recognition = false,
                          .last_error = "settings warning"};
  diagnostics.aggregate_resources = {.threads = 1,
                                     .timers = 2,
                                     .handles = 3,
                                     .windows = 4,
                                     .objects = 5,
                                     .capture_sessions = 6,
                                     .queues = 7,
                                     .decoded_assets = 8,
                                     .cache_bytes =
                                         std::numeric_limits<std::uint64_t>::max()};
  diagnostics.sessions.push_back({
      .profile_id = "profile",
      .source_key = "source",
      .window = std::numeric_limits<std::uint64_t>::max(),
      .foreground = true,
      .overlay_windows = 2,
      .last_input_match = "key E",
  });
  diagnostics.helpers.push_back({
      .window = std::numeric_limits<std::uint64_t>::max() - 1,
      .pid = 99,
      .running = true,
      .quarantined = true,
      .fault_count = 7,
      .last_error = "helper fault",
  });

  diagnostics.automation_faults = {
      "automation checkpoint failed (profile store I/O failed): game.json",
      "game.json: automation fault: tick: faulted during event"};
  const auto json =
      layerc::runtime::serialize_runtime_diagnostics_json(diagnostics);
  const auto parsed = layerc::runtime::parse_runtime_diagnostics_json(json);
  const auto *round_trip =
      std::get_if<layerc::runtime::RuntimeDiagnostics>(&parsed);
  expect(round_trip != nullptr && *round_trip == diagnostics,
         "typed diagnostics should round-trip every contract section and uint64 boundary");
  expect(json.find("\"packageVersion\":\"test-package\"") != std::string::npos &&
             json.find("\"window\":\"18446744073709551615\"") !=
                 std::string::npos &&
             json.find("\"cacheBytes\":\"18446744073709551615\"") !=
                 std::string::npos,
         "diagnostics JSON should carry the model version and lossless large counters");

  auto unknown_state = json;
  const auto state = unknown_state.find("\"state\":\"faulted\"");
  unknown_state.replace(state, std::string_view("\"state\":\"faulted\"").size(),
                        "\"state\":\"mystery\"");
  const auto invalid =
      layerc::runtime::parse_runtime_diagnostics_json(unknown_state);
  const auto *error =
      std::get_if<layerc::core::json::FieldError>(&invalid);
  expect(error != nullptr && error->path == "$.services[0].state",
         "typed diagnostics should diagnose invalid nested enum values");

  auto duplicate = diagnostics;
  duplicate.services.push_back(duplicate.services.front());
  const auto duplicated = layerc::runtime::parse_runtime_diagnostics_json(
      layerc::runtime::serialize_runtime_diagnostics_json(duplicate));
  const auto *duplicate_error =
      std::get_if<layerc::core::json::FieldError>(&duplicated);
  expect(duplicate_error != nullptr &&
             duplicate_error->path == "$.services[1].capability",
         "typed diagnostics should reject duplicate capability rows");

  expect(layerc::runtime::RuntimeDiagnostics{}.package_version ==
             layerc::core::product_version,
         "diagnostics should source the package version from the build definition");
}

layerc::runtime::LinuxDiagnostics linux_diagnostics_fixture() {
  using namespace layerc::runtime;
  LinuxDiagnostics linux;
  linux.x11.connected = true;
  linux.x11.xi2 = {.available = true, .major = 2, .minor = 3};
  linux.x11.shape = {.available = true, .major = 1, .minor = 1};
  linux.x11.randr = {.available = true, .major = 1, .minor = 6};
  linux.x11.composite.reason = "capture unavailable: \"target\"\nretry";
  linux.x11.damage.reason = "";
  linux.x11.xfixes = {.available = true, .major = 6, .minor = 0};
  linux.x11.ewmh_reason = "window manager unavailable";
  linux.capabilities = {
      {.capability = Capability::input, .availability = CapabilityAvailability::active,
       .reasons = {}, .diagnostics = {}},
      {.capability = Capability::visual, .availability = CapabilityAvailability::degraded,
       .reasons = {"capture_fallback"},
       .diagnostics = {"target \"window\" lost\nretry \\ UTF-8: \xC3\xA9"}},
      {.capability = Capability::generic_hid, .availability = CapabilityAvailability::unavailable,
       .reasons = {"generic_hid_unsupported"}, .diagnostics = {}},
      {.capability = Capability::overlay, .availability = CapabilityAvailability::inactive,
       .reasons = {"no_consumers"}, .diagnostics = {}},
  };
  return linux;
}

void diagnostics_linux_extension_round_trips() {
  using namespace layerc::runtime;
  RuntimeDiagnostics diagnostics;
  diagnostics.process.pid = 41;
  diagnostics.sessions.push_back({.profile_id = "linux", .source_key = "source",
                                  .window = UINT64_MAX, .foreground = true,
                                  .last_input_match = "key E"});
  diagnostics.automation_faults = {"state write failed"};
  diagnostics.linux_extension = linux_diagnostics_fixture();
  for (const bool ewmh : {false, true}) {
    diagnostics.linux_extension->x11.ewmh = ewmh;
    if (ewmh) diagnostics.linux_extension->x11.ewmh_reason.reset();
    const auto wire = serialize_runtime_diagnostics_json(diagnostics);
    const auto parsed = parse_runtime_diagnostics_json(wire);
    const auto *value = std::get_if<RuntimeDiagnostics>(&parsed);
    expect(value && *value == diagnostics && serialize_runtime_diagnostics_json(*value) == wire,
           "shared diagnostics preserve Linux probes, enums, reasons, faults and sessions");
    const auto document = layerc::core::json::parse(wire);
    const auto *root = std::get_if<layerc::core::json::Value>(&document);
    expect(root && root->find("x11") && root->find("capabilities") && !root->find("linux"),
           "the typed extension retains the existing top-level Linux wire fields");
  }
  diagnostics.linux_extension.reset();
  const auto parsed = parse_runtime_diagnostics_json(serialize_runtime_diagnostics_json(diagnostics));
  const auto *value = std::get_if<RuntimeDiagnostics>(&parsed);
  expect(value && *value == diagnostics && !value->linux_extension,
         "shared diagnostics without Linux fields still round-trip unchanged");
}

void diagnostics_linux_adapter_preserves_shared_fields() {
  using namespace layerc::runtime;
  RuntimeDiagnostics diagnostics;
  diagnostics.sessions.push_back({.profile_id = "shared", .source_key = "source",
                                  .window = UINT64_MAX, .foreground = true,
                                  .last_input_match = "key E"});
  diagnostics.settings.last_error = "settings unavailable";
  diagnostics.automation_faults = {"checkpoint failed"};
  diagnostics.linux_extension = linux_diagnostics_fixture();
  LinuxStatusSnapshot native;
  native.x11 = diagnostics.linux_extension->x11;
  native.capabilities = diagnostics.linux_extension->capabilities;
  native.x11.composite.reason.reset();
  native.x11.damage.reason.reset();
  native.x11.ewmh_reason.reset();
  native.sessions.push_back({.profile_id = "unused-native-row", .source_key = "source",
                            .last_input_match = ""});
  const auto original = diagnostics;
  for (const bool connected : {false, true}) {
    native.x11.connected = connected;
    native.x11.reason = connected ? "" : "display_unavailable";
    const auto wire = serialize_linux_status_json(diagnostics, native);
    const auto parsed = parse_runtime_diagnostics_json(wire);
    const auto *value = std::get_if<RuntimeDiagnostics>(&parsed);
    auto expected = diagnostics;
    expected.linux_extension = LinuxDiagnostics{native.x11, native.capabilities};
    const std::string reason = connected ? "extension_missing" : native.x11.reason;
    expected.linux_extension->x11.composite.reason = reason;
    expected.linux_extension->x11.damage.reason = reason;
    expected.linux_extension->x11.ewmh_reason = reason;
    expect(value && *value == expected && serialize_runtime_diagnostics_json(*value) == wire,
           "native status retains the existing unavailable reason fallback and shared fields");
    expect(diagnostics == original,
           "native serialization neither changes the caller nor replaces its shared session rows");
  }
}

void diagnostics_linux_parser_failure_paths() {
  namespace json = layerc::core::json;
  using namespace layerc::runtime;
  RuntimeDiagnostics diagnostics;
  diagnostics.linux_extension = linux_diagnostics_fixture();
  const auto parsed = json::parse(serialize_runtime_diagnostics_json(diagnostics));
  const auto baseline = std::get<json::Value>(parsed).as_object();
  const auto rejects = [&](const std::string_view path, auto mutate) {
    auto root = baseline;
    mutate(root);
    std::string wire;
    json::append_value(wire, json::Value(std::move(root)));
    const auto result = parse_runtime_diagnostics_json(wire);
    const auto *error = std::get_if<json::FieldError>(&result);
    expect(error && error->path == path && !error->message.empty(),
           "invalid Linux diagnostics retain the exact nested field path");
  };
  rejects("$.x11", [](auto &root) { root.erase("x11"); });
  rejects("$.capabilities", [](auto &root) { root.erase("capabilities"); });
  for (const auto field : {"x11", "capabilities"})
    rejects(std::string("$.") + field, [&](auto &root) { root[field] = json::Value(nullptr); });
  rejects("$.x11.connected", [](auto &root) {
    auto x11 = root.at("x11").as_object();
    x11["connected"] = json::Value(std::string("yes"));
    root["x11"] = json::Value(std::move(x11));
  });
  rejects("$.x11.shape", [](auto &root) {
    auto x11 = root.at("x11").as_object();
    x11.erase("shape");
    root["x11"] = json::Value(std::move(x11));
  });
  const auto rejects_probe = [&](const std::string_view probe, const std::string_view path,
                                  auto mutate) {
    rejects(path, [&](auto &root) {
      auto x11 = root.at("x11").as_object();
      auto fields = x11.at(std::string(probe)).as_object();
      mutate(fields);
      x11[std::string(probe)] = json::Value(std::move(fields));
      root["x11"] = json::Value(std::move(x11));
    });
  };
  rejects_probe("xi2", "$.x11.xi2.available", [](auto &fields) {
    fields["available"] = json::Value(std::int64_t{1});
  });
  rejects_probe("xi2", "$.x11.xi2.major", [](auto &fields) { fields.erase("major"); });
  for (const auto &number : {json::Value(std::int64_t{-1}), json::Value(1.5),
                             json::Value(std::int64_t{INT64_MAX})})
    rejects_probe("xi2", "$.x11.xi2.major", [&](auto &fields) { fields["major"] = number; });
  rejects_probe("xi2", "$.x11.xi2.minor", [](auto &fields) {
    fields["minor"] = json::Value(std::string("3"));
  });
  for (const auto probe : {"damage", "ewmh"})
    rejects_probe(probe, std::string("$.x11.") + probe + ".reason",
                  [](auto &fields) { fields.erase("reason"); });
  rejects_probe("composite", "$.x11.composite.reason", [](auto &fields) {
    fields["reason"] = json::Value(false);
  });
  rejects_probe("xi2", "$.x11.xi2.extra", [](auto &fields) { fields["extra"] = json::Value(nullptr); });
  const auto rejects_capability = [&](const std::string_view path, auto mutate) {
    rejects(path, [&](auto &root) {
      auto rows = root.at("capabilities").as_array();
      auto fields = rows.front().as_object();
      mutate(fields);
      rows.front() = json::Value(std::move(fields));
      root["capabilities"] = json::Value(std::move(rows));
    });
  };
  for (const auto field : {"capability", "availability"})
    rejects_capability(std::string("$.capabilities[0].") + field, [&](auto &fields) {
      fields[field] = json::Value(std::string("mystery"));
    });
  rejects_capability("$.capabilities[0].reasons", [](auto &fields) { fields.erase("reasons"); });
  for (const auto field : {"reasons", "diagnostics"})
    rejects_capability(std::string("$.capabilities[0].") + field + "[0]", [&](auto &fields) {
      fields[field] = json::Value(json::Value::Array{json::Value(false)});
    });
  rejects_capability("$.capabilities[0].extra", [](auto &fields) { fields["extra"] = json::Value(nullptr); });
  rejects("$.capabilities[0]", [](auto &root) {
    root["capabilities"] = json::Value(json::Value::Array{json::Value(nullptr)});
  });

  std::string deep(200, '[');
  deep += '0';
  deep.append(200, ']');
  for (const std::string &source : {std::string("{broken"), std::string("[1,]"), deep}) {
    const auto shared = json::parse_bounded(source, layerc::core::max_live_message_bytes);
    const auto result = parse_runtime_diagnostics_json(source);
    const auto *expected = std::get_if<json::FieldError>(&shared);
    const auto *error = std::get_if<json::FieldError>(&result);
    expect(expected && error && error->path == expected->path && error->message == expected->message,
           "diagnostics syntax errors use the shared bounded-reader conversion");
  }
  const std::string oversized(layerc::core::max_live_message_bytes + 1, ' ');
  const auto shared_limit =
      json::parse_bounded(oversized, layerc::core::max_live_message_bytes);
  const auto too_large = parse_runtime_diagnostics_json(oversized);
  const auto *expected_limit = std::get_if<json::FieldError>(&shared_limit);
  const auto *limit_error = std::get_if<json::FieldError>(&too_large);
  expect(expected_limit && limit_error && limit_error->path == "$" &&
             limit_error->message == expected_limit->message,
         "diagnostics size admission is owned by the shared bounded reader");
  const auto wrong_root = parse_runtime_diagnostics_json("[]");
  const auto *root_error = std::get_if<json::FieldError>(&wrong_root);
  expect(root_error && root_error->path == "$", "diagnostics still reject a non-object root");
}

} // namespace

int main() {
  return layerc::test::run({
      {"demand_counts_saturate", demand_counts_saturate},
      {"host_is_lazy_and_idempotent", host_is_lazy_and_idempotent},
      {"faults_release_the_service", faults_release_the_service},
      {"later_fault_keeps_healthy_capabilities_active", later_fault_keeps_healthy_capabilities_active},
      {"unavailable_isolated_capture_leaves_input_active", unavailable_isolated_capture_leaves_input_active},
      {"teardown_is_reverse_ordered", teardown_is_reverse_ordered},
      {"repeated_activation_has_bounded_ownership", repeated_activation_has_bounded_ownership},
      {"resource_ledger_covers_non_lifecycle_resources", resource_ledger_covers_non_lifecycle_resources},
      {"demand_matrix_is_explicit", demand_matrix_is_explicit},
      {"kernel_serializes_reentrant_demand", kernel_serializes_reentrant_demand},
      {"diagnostics_contract_round_trips", diagnostics_contract_round_trips},
      {"diagnostics_linux_extension_round_trips", diagnostics_linux_extension_round_trips},
      {"diagnostics_linux_adapter_preserves_shared_fields", diagnostics_linux_adapter_preserves_shared_fields},
      {"diagnostics_linux_parser_failure_paths", diagnostics_linux_parser_failure_paths},
      {"diagnostics_display_contracts", diagnostics_display_contracts},
      {"diagnostics_overlay_fault_contracts", diagnostics_overlay_fault_contracts},
  });
}
