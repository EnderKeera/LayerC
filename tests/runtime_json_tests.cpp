#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/hash.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/runtime_instances.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace layerc::test {
void test_sha256_helpers() {
  const auto digest = [](const std::string_view value) {
    return layerc::core::sha256(std::span(
        reinterpret_cast<const std::byte *>(value.data()), value.size()));
  };
  expect(digest({}) ==
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "SHA-256 should match the standard empty-input vector");
  expect(digest("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA-256 should match the standard abc vector");
  const std::string exact_block(64, 'a');
  expect(digest(exact_block) ==
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
          "SHA-256 should clear the padding block after exactly 64 input bytes");
  // Independent .NET SHA256 known answers for padding/block boundaries.
  const std::pair<std::size_t, std::string_view> boundaries[]{
      {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
      {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
      {57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
      {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
      {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
  };
  for (const auto &[length, expected] : boundaries)
    expect(digest(std::string(length, 'a')) == expected,
           "SHA-256 must preserve length and padding across block boundaries");
  constexpr std::string_view multi_block =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  expect(digest(multi_block) ==
             "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
         "SHA-256 should match the standard multi-block vector");
  const auto empty_bytes = layerc::core::sha256(std::span<const std::byte>{});
  expect(empty_bytes == digest({}),
         "the byte-span overload should handle an empty span safely");
  const auto valid_digest = digest("abc");
  expect(layerc::core::is_lowercase_sha256(valid_digest) &&
             !layerc::core::is_lowercase_sha256(std::string(64, 'A')) &&
             !layerc::core::is_lowercase_sha256(std::string(63, 'a')),
         "the shared hash validator should accept only 64 lowercase hex characters");
}

void test_runtime_instance_tracking() {
  using namespace layerc::core;
  RuntimeInstanceTracker tracker;
  const RuntimeInstanceKey first{.profile_key = "game-profile", .window = {101}};
  const RuntimeInstanceKey second{.profile_key = "game-profile", .window = {202}};
  const RuntimeInstanceKey same_window_other_profile{
      .profile_key = "game-profile-copy", .window = {202}};

  const auto added =
      tracker.reconcile({first, second, same_window_other_profile});
  expect(added.added.size() == 3 && added.removed.empty() &&
             tracker.size() == 3,
         "matching profiles and windows should create independent runtime instances");
  const auto foreground_second = tracker.foreground({202});
  expect(foreground_second.size() == 2 &&
             std::all_of(foreground_second.begin(), foreground_second.end(),
                         [](const RuntimeInstanceKey &key) {
                           return key.window == WindowId{202};
                         }),
         "foreground input should route to every profile matching the foreground window");

  const std::vector<CooldownDefinition> cooldowns{{.id = "dash", .name = "Dash",
                                                    .duration = std::chrono::seconds(5)}};
  CooldownEngine first_engine(cooldowns);
  CooldownEngine second_engine(cooldowns);
  expect(first_engine.trigger("dash", TimePoint{}),
         "first instance should accept its own cooldown trigger");
  expect(second_engine.trigger("dash", TimePoint{} + std::chrono::seconds(2)),
         "second instance should accept a separate later trigger");
  expect(first_engine.remaining("dash", TimePoint{} + std::chrono::seconds(2)) ==
             std::chrono::seconds(3) &&
             second_engine.remaining("dash", TimePoint{} + std::chrono::seconds(2)) ==
                 std::chrono::seconds(5),
         "two matching instances should retain separate cooldown timelines");

  const auto removed = tracker.reconcile({second, same_window_other_profile});
  expect(removed.removed.size() == 1 && removed.removed.front() == first &&
             tracker.size() == 2 && tracker.foreground({101}).empty(),
         "closing or minimizing a matched window should retire its runtime instance");

  const auto launcher_exit = tracker.reconcile({});
  expect(launcher_exit.removed.size() == 2 && tracker.size() == 0,
         "a launcher transition with no matching windows should retire all instances");
  const RuntimeInstanceKey restarted{.profile_key = "game-profile",
                                     .window = {303}};
  const auto game_restart = tracker.reconcile({restarted});
  expect(game_restart.added == std::vector<RuntimeInstanceKey>{restarted} &&
             tracker.foreground({303}) ==
                 std::vector<RuntimeInstanceKey>{restarted},
         "a restarted game process should create a fresh routable instance");
  RuntimeInstanceTracker daemon_restart;
  expect(daemon_restart.reconcile({restarted}).added ==
             std::vector<RuntimeInstanceKey>{restarted},
         "a daemon restart should reconstruct instances from current matches");
}
void test_json_unicode_and_errors() {
  const std::string source = R"({"n":9223372036854775807,"a":[true,null,"x\\ny"],"empty":{}})";
  const auto pretty_document = layerc::core::json::format(source);
  expect(pretty_document.find("\r\n  ") != std::string::npos &&
             pretty_document.find("9223372036854775807") != std::string::npos &&
             layerc::core::json::format(pretty_document) == pretty_document,
         "JSON formatting preserves exact integers and is idempotent");
  expect(layerc::core::json::format("{broken") == "{broken",
         "JSON formatting must preserve invalid editable source");
  using namespace layerc::core::json;

  auto result = parse(R"json({"name":"Layer\u0043","value":1.5})json");
  expect(std::holds_alternative<Value>(result),
         "valid unicode JSON should parse");
  if (const auto *value = std::get_if<Value>(&result)) {
    expect(value->find("name") != nullptr &&
               value->find("name")->as_string() == "LayerC",
           "unicode escape should decode to UTF-8");
  }

  result = parse("[1,]");
  expect(std::holds_alternative<ParseError>(result),
         "trailing array comma should fail");
  const auto leading_zero = parse(R"({"value":01})");
  const auto *leading_zero_error = std::get_if<ParseError>(&leading_zero);
  expect(leading_zero_error != nullptr && leading_zero_error->offset == 10,
         "RFC 8259 leading-zero numbers should fail at the extra digit");

  const std::string special = "quote\" slash\\ controls\n\t\x01 UTF-8: \xC3\xA9";
  std::string formatted = "{\"text\":";
  append_quoted(formatted, special);
  formatted += ",\"maximum\":";
  append_unsigned(formatted, std::numeric_limits<std::uint64_t>::max());
  formatted += ",\"decimal\":";
  append_number(formatted, 0.1);
  formatted += '}';
  std::string quoted_special;
  append_quoted(quoted_special, special);
  const auto formatted_result = parse(formatted);
  const auto *formatted_value = std::get_if<Value>(&formatted_result);
  expect(formatted_value != nullptr &&
             formatted_value->find("text") != nullptr &&
             formatted_value->find("text")->as_string() == special &&
             quoted_special == formatted.substr(
                                   std::string_view("{\"text\":").size(),
                                   quoted_special.size()),
         "shared JSON formatting should escape strings and preserve UTF-8");

  const auto rejects_number = [](const double value) {
    std::string output = "prefix";
    try {
      append_number(output, value);
    } catch (const std::invalid_argument &) {
      return output == "prefix";
    }
    return false;
  };
  expect(rejects_number(std::numeric_limits<double>::quiet_NaN()) &&
             rejects_number(std::numeric_limits<double>::infinity()),
         "non-finite values should be rejected without mutating JSON output");

  std::string nested_non_finite = "prefix";
  bool nested_rejected = false;
  try {
    append_value(nested_non_finite,
                 Value(Value::Array{Value(1.0),
                                    Value(std::numeric_limits<double>::infinity())}));
  } catch (const std::invalid_argument &) {
    nested_rejected = true;
  }
  expect(nested_rejected && nested_non_finite == "prefix",
         "generic JSON emission should roll back a partially emitted invalid tree");

  // Integral JSON tokens keep exact int64 storage: values beyond the 2^53
  // double mantissa round-trip exactly instead of silently rounding.
  const auto exact = parse(R"json({"big":9007199254740993,"neg":-42,"real":1.5})json");
  const auto *exact_value = std::get_if<Value>(&exact);
  expect(exact_value != nullptr && exact_value->find("big")->is_integer() &&
             exact_value->find("big")->as_integer() ==
                 std::int64_t{9'007'199'254'740'993} &&
             exact_value->find("neg")->is_integer() &&
             exact_value->find("neg")->as_integer() == -42 &&
             exact_value->find("real")->is_number() &&
             !exact_value->find("real")->is_integer() &&
             exact_value->find("real")->as_number() == 1.5 &&
             exact_value->find("big")->is_number() &&
             exact_value->find("big")->as_number() == 9'007'199'254'740'993.0,
         "integral JSON should parse to exact int64 storage usable as number");
}

void test_json_field_reader() {
  using namespace layerc::core::json;
  const auto document = parse(
      R"({"empty":"","text":"four","enabled":true,"color":"#aB09fF","badColor":"#12xy45","integer":9223372036854775807,"real":1.0,"fraction":1.5,"overflow":9223372036854775808.0,"unsigned":"18446744073709551615","negative":-1,"extra":null})");
  const auto &root = std::get<Value>(document);
  Reader reader;
  std::string text = "default";
  bool enabled = false;
  expect(reader.string(root, "missing", "$", text, Required::no, EmptyString::reject) &&
             text == "default" &&
             reader.string(root, "empty", "$", text, Required::yes, EmptyString::allow) &&
             text.empty() &&
             reader.boolean(root, "enabled", "$", enabled, Required::yes) && enabled &&
             reader.integer(root, "missing", "$", 0, 10, Required::no, 3) == 3 &&
             reader.color(root, "color", "$", Required::yes) == 0xAB09FFU,
         "explicit optional and empty policies preserve defaults and shared color syntax");
  expect(reader.integer(root, "integer", "$", 0, INT64_MAX, Required::yes) == INT64_MAX &&
             reader.integer(root, "real", "$", 0, 2, Required::yes) == 1,
         "numeric policy preserves exact int64 tokens and accepts small integral decimals");
  const auto failure = [&](const std::string_view key, auto operation) {
    Reader invalid;
    expect(!operation(invalid), "invalid field must fail validation");
    const auto first = invalid.error().describe();
    invalid.fail("$.later", "must not replace the first error");
    expect(invalid.error().path == "$.nested." + std::string(key) &&
               !first.empty() && invalid.error().describe() == first,
           "field failures retain the first complete nested path");
  };
  failure("empty", [&](Reader &r) {
    return r.string(root, "empty", "$.nested", text, Required::yes, EmptyString::reject);
  });
  failure("missing", [&](Reader &r) {
    return r.string(root, "missing", "$.nested", text, Required::yes, EmptyString::allow);
  });
  failure("text", [&](Reader &r) {
    return r.string(root, "text", "$.nested", text, Required::yes, EmptyString::allow, 3);
  });
  for (const auto key : {"fraction", "overflow"})
    failure(key, [&](Reader &r) {
      return r.integer(root, key, "$.nested", INT64_MIN, INT64_MAX, Required::yes);
    });
  failure("real", [&](Reader &r) {
    return r.integer(root, "real", "$.nested", 0, 2, Required::yes, 0,
                     IntegerEncoding::integer_token);
  });
  failure("badColor", [&](Reader &r) {
    return r.color(root, "badColor", "$.nested", Required::yes);
  });
  failure("negative", [&](Reader &r) {
    return r.unsigned_integer(root, "negative", "$.nested", UINT64_MAX, Required::yes);
  });
  expect(read_unsigned(*root.find("unsigned"), IntegerEncoding::decimal_string) == UINT64_MAX &&
             !read_unsigned(*root.find("unsigned"), IntegerEncoding::number) &&
             !read_unsigned(*root.find("integer"), IntegerEncoding::decimal_string) &&
             !read_integer(Value(std::numeric_limits<double>::infinity())) &&
             !read_integer(Value(std::numeric_limits<double>::quiet_NaN())),
         "integer encodings are explicit and reject lossy or non-finite conversions");
  Reader unknown;
  expect(!unknown.exact_fields(root, {"empty"}, "$") && !unknown.error().path.empty(),
         "strict schemas diagnose unknown keys");
  Reader wrong_root;
  expect(!wrong_root.object(Value(nullptr), "child", "$", Required::no) &&
             wrong_root.error().path == "$",
         "an optional field still requires an object parent");
}

void test_json_nesting_limit() {
  using namespace layerc::core::json;

  std::string excessive(200, '[');
  excessive += '0';
  excessive.append(200, ']');
  expect(std::holds_alternative<ParseError>(parse(excessive)),
         "excessively nested JSON arrays should return a parse error");

  std::string excessive_objects;
  for (int depth = 0; depth < 200; ++depth) excessive_objects += R"json({"x":)json";
  excessive_objects += '0';
  excessive_objects.append(200, '}');
  expect(std::holds_alternative<ParseError>(parse(excessive_objects)),
         "excessively nested JSON objects should return a parse error");

  std::string ordinary(32, '[');
  ordinary += '0';
  ordinary.append(32, ']');
  expect(std::holds_alternative<Value>(parse(ordinary)),
         "ordinary nested JSON should remain supported");
}

void test_json_malformed_corpus() {
  using namespace layerc::core;
  std::uint32_t state = 0xC0FFEEU;
  std::size_t rejected{};
  std::size_t survived_as_valid{};
  const std::string seed =
      R"json({"schemaVersion":1,"id":"game","name":"Game","game":{"executableName":"game.exe"},"cooldowns":[{"id":"dash","name":"Dash","durationMs":1000}],"panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":69}]}]})json";
  for (int iteration = 0; iteration < 512; ++iteration) {
    auto candidate = seed;
    state = state * 1664525U + 1013904223U;
    const std::size_t changes = 1U + state % 8U;
    for (std::size_t change = 0; change < changes; ++change) {
      state = state * 1664525U + 1013904223U;
      const auto offset = static_cast<std::size_t>(state) % candidate.size();
      candidate[offset] = static_cast<char>((state >> 16U) & 0xFFU);
    }
    const auto result = parse_profile_json(candidate);
    if (const auto *profile = std::get_if<GameProfile>(&result)) {
      ++survived_as_valid;
      expect(std::holds_alternative<GameProfile>(
                 parse_profile_json(serialize_profile_json(*profile))),
             "valid corpus survivors should remain serializable");
    } else {
      ++rejected;
    }
  }
  expect(rejected > 450 && rejected + survived_as_valid == 512,
         "the deterministic corrupt-profile corpus should be rejected safely");
}

} // namespace layerc::test
