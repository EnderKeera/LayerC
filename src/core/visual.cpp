#include "layerc/core/visual.hpp"
#include "layerc/core/hash.hpp"

#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace layerc::core {
namespace {


class Reader {
public:
  Reader(const json::Value &root, const GameProfile &base)
      : root_(root), base_(base) {}

  VisualResult read() {
    if (!root_.is_object()) return error("$", "visual root must be an object");
    const auto version = fields_.integer(root_, "schemaVersion", "$", 1, 2);
    if (!version) return failure();
    if (*version == 1) {
      if (!fields_.exact_fields(root_, {"schemaVersion", "profileId", "sampleRateHz",
                          "quality", "rules", "triggers"}, "$"))
        return failure();
    } else if (!fields_.exact_fields(root_, {"schemaVersion", "profileId", "sampleRateHz",
                               "quality", "observations", "triggers"}, "$")) {
      return failure();
    }

    schema_version_ = *version;
    VisualProfile result;
    if (!fields_.string(root_, "profileId", "$", result.profile_id))
      return failure();
    // Schema 1 is migrated in memory. Every subsequent save emits schema 2.
    result.schema_version = 2;
    const auto rate = fields_.integer(root_, "sampleRateHz", "$", 5, 15);
    std::string quality;
    if (!rate || !fields_.string(root_, "quality", "$", quality)) return failure();
    // The 5/10/15 Hz set, reference counts, and cross-field rules are
    // semantic invariants owned by validate_visual_profile.
    result.sample_rate_hz = static_cast<std::uint8_t>(*rate);
    if (auto parsed = parse_enum_wire(capture_quality_wire, quality))
      result.quality = *parsed;
    else
      return error("$.quality", "expected low, balanced, or exact");

    if (*version == 1) {
      const auto *rules = fields_.array(root_, "rules", "$");
      if (rules == nullptr) return failure();
      if (rules->size() > max_pixel_rules)
        return error("$.rules", "at most four pixel rules are supported");
      for (std::size_t index = 0; index < rules->size(); ++index) {
        if (!read_legacy_rule((*rules)[index], index, result.rules))
          return failure();
      }
    } else {
      const auto *observations = fields_.array(root_, "observations", "$");
      if (observations == nullptr) return failure();
      if (observations->size() > max_pixel_rules + max_template_states)
        return error("$.observations",
                     "at most eight visual observations are supported");
      for (std::size_t index = 0; index < observations->size(); ++index) {
        if (!read_observation((*observations)[index], index, result))
          return failure();
      }
    }

    const auto *triggers = fields_.array(root_, "triggers", "$");
    if (triggers == nullptr) return failure();
    for (std::size_t index = 0; index < triggers->size(); ++index) {
      if (!read_trigger((*triggers)[index], index, *version, result.triggers))
        return failure();
    }
    const auto validation = validate_visual_profile(
        result, base_, [this](bool pixel, std::size_t index) {
          const auto &indices = pixel ? rule_indices_ : template_indices_;
          return (schema_version_ == 2 ? "$.observations[" : "$.rules[") +
                 std::to_string(indices[index]) + "]";
        });
    if (!validation.message.empty()) return validation;
    return result;
  }

private:
  bool read_legacy_rule(const json::Value &value, const std::size_t index,
                        std::vector<PixelRule> &output) {
    if (!read_rule(value, "$.rules[" + std::to_string(index) + "]", output,
                   false)) return false;
    rule_indices_.push_back(index);
    return true;
  }

  bool read_typed_rule(const json::Value &value, const std::size_t index,
                       std::vector<PixelRule> &output) {
    if (!read_rule(value, "$.observations[" + std::to_string(index) + "]",
                   output, true)) return false;
    rule_indices_.push_back(index);
    return true;
  }

  bool read_region(const json::Value &value, const std::string_view key,
                   const std::string &path, NormalizedRegion &out) {
    const auto *region = fields_.object(value, key, path);
    const auto region_path = path + "." + std::string(key);
    if (region == nullptr ||
        !fields_.exact_fields(*region, {"x", "y", "width", "height"}, region_path))
      return false;
    const auto x = fields_.number(*region, "x", region_path, 0.0, 1.0);
    const auto y = fields_.number(*region, "y", region_path, 0.0, 1.0);
    const auto width = fields_.number(*region, "width", region_path, 0.0, 1.0);
    const auto height = fields_.number(*region, "height", region_path, 0.0, 1.0);
    if (!x || !y || !width || !height) return false;
    out = {*x, *y, *width, *height};
    return true;
  }

  bool read_repeat_policy(const json::Value &value, const std::string &path,
                          PixelRepeatPolicy &out) {
    std::string repeat;
    if (!fields_.string(value, "repeat", path, repeat)) return false;
    const auto parsed = parse_enum_wire(pixel_repeat_wire, repeat);
    if (!parsed)
      return set(path + ".repeat", "expected edge or whenFullyReady");
    out = *parsed;
    return true;
  }

  bool read_predicate(const json::Value &value, const std::string &path,
                      PixelPredicate &out) {
    const auto *predicate = fields_.object(value, "predicate", path);
    if (predicate == nullptr) return false;
    const auto predicate_path = path + ".predicate";
    std::string kind;
    if (!fields_.string(*predicate, "type", predicate_path, kind)) return false;
    if (kind == "averageColor") {
      if (!fields_.exact_fields(*predicate, {"type", "target", "tolerance"}, predicate_path))
        return false;
      const auto target = fields_.color(*predicate, "target", predicate_path);
      const auto tolerance =
          fields_.integer(*predicate, "tolerance", predicate_path, 0, 255);
      if (!target || !tolerance) return false;
      out = AverageColorPredicate{*target, static_cast<std::uint8_t>(*tolerance)};
    } else if (kind == "brightness") {
      if (!fields_.exact_fields(*predicate, {"type", "comparison", "threshold"},
                  predicate_path))
        return false;
      std::string comparison;
      const auto threshold =
          fields_.integer(*predicate, "threshold", predicate_path, 0, 255);
      if (!threshold ||
          !fields_.string(*predicate, "comparison", predicate_path, comparison))
        return false;
      BrightnessComparison parsed{};
      if (const auto wire = parse_enum_wire(brightness_wire, comparison))
        parsed = *wire;
      else
        return set(predicate_path + ".comparison", "expected above or below");
      out = BrightnessPredicate{static_cast<std::uint8_t>(*threshold), parsed};
    } else if (kind == "colorCoverage") {
      if (!fields_.exact_fields(*predicate, {"type", "target", "tolerance", "minimumPercent"},
                  predicate_path))
        return false;
      const auto target = fields_.color(*predicate, "target", predicate_path);
      const auto tolerance =
          fields_.integer(*predicate, "tolerance", predicate_path, 0, 255);
      const auto minimum =
          fields_.number(*predicate, "minimumPercent", predicate_path, 0.0, 100.0);
      if (!target || !tolerance || !minimum) return false;
      out = ColorCoveragePredicate{*target, static_cast<std::uint8_t>(*tolerance),
                                  *minimum / 100.0};
    } else {
      return set(predicate_path + ".type", "unsupported pixel predicate");
    }
    return true;
  }

  bool read_rule(const json::Value &value, const std::string &path,
                 std::vector<PixelRule> &output, const bool includes_type) {
    if (!value.is_object()) return set(path, "pixel rule must be an object");
    if ((includes_type &&
         !fields_.exact_fields(value, {"type", "id", "name", "region", "predicate",
                         "stableSamples", "repeat"}, path)) ||
        (!includes_type &&
         !fields_.exact_fields(value, {"id", "name", "region", "predicate",
                         "stableSamples", "repeat"}, path)))
      return false;
    PixelRule rule;
    if (!fields_.string(value, "id", path, rule.id) ||
        !fields_.string(value, "name", path, rule.name))
      return false;
    if (!read_region(value, "region", path, rule.region)) return false;

    if (!read_predicate(value, path, rule.predicate)) return false;

    const auto stable = fields_.integer(value, "stableSamples", path, 1, 15);
    if (!stable) return false;
    rule.stable_samples = static_cast<std::uint8_t>(*stable);
    if (!read_repeat_policy(value, path, rule.repeat)) return false;
    output.push_back(std::move(rule));
    return true;
  }

  bool read_observation(const json::Value &value, const std::size_t index,
                        VisualProfile &output) {
    const auto path = "$.observations[" + std::to_string(index) + "]";
    if (!value.is_object())
      return set(path, "visual observation must be an object");
    std::string type;
    if (!fields_.string(value, "type", path, type)) return false;
    if (type == "pixel") {
      if (output.rules.size() >= max_pixel_rules)
        return set(path, "at most four pixel observations are supported");
      return read_typed_rule(value, index, output.rules);
    }
    if (type != "template")
      return set(path + ".type", "expected pixel or template");
    if (output.templates.size() >= max_template_states)
      return set(path, "at most four template observations are supported");
    if (!read_template(value, index, output.templates)) return false;
    template_indices_.push_back(index);
    return true;
  }

  bool read_template(const json::Value &value, const std::size_t index,
                     std::vector<TemplateState> &output) {
    const auto path = "$.observations[" + std::to_string(index) + "]";
    if (!fields_.exact_fields(value,
                {"type", "id", "name", "searchRegion", "expectedSize",
                 "references", "confidence", "scaleTolerancePercent",
                 "colorTolerance", "stableSamples", "repeat"},
                path))
      return false;
    TemplateState state;
    if (!fields_.string(value, "id", path, state.id) ||
        !fields_.string(value, "name", path, state.name))
      return false;

    if (!read_region(value, "searchRegion", path, state.search_region))
      return false;

    const auto *expected = fields_.object(value, "expectedSize", path);
    if (expected == nullptr)
      return false;
    if (!fields_.exact_fields(*expected, {"width", "height"}, path + ".expectedSize"))
      return false;
    const auto expected_width =
        fields_.number(*expected, "width", path + ".expectedSize", 0.000001, 1.0);
    const auto expected_height =
        fields_.number(*expected, "height", path + ".expectedSize", 0.000001, 1.0);
    if (!expected_width || !expected_height) return false;
    state.expected_width = *expected_width;
    state.expected_height = *expected_height;

    const auto *references = fields_.array(value, "references", path);
    if (references == nullptr) return false;
    for (std::size_t reference_index = 0;
         reference_index < references->size(); ++reference_index) {
      const auto reference_path =
          path + ".references[" + std::to_string(reference_index) + "]";
      const auto &reference = (*references)[reference_index];
      if (!reference.is_object() ||
          !fields_.exact_fields(reference, {"asset"}, reference_path))
        return set(reference_path, "template reference must be an object");
      TemplateReference parsed;
      if (!fields_.string(reference, "asset", reference_path, parsed.asset_sha256))
        return false;
      state.references.push_back(std::move(parsed));
    }

    const auto confidence =
        fields_.number(value, "confidence", path, 0.0, 1.0);
    const auto scale =
        fields_.number(value, "scaleTolerancePercent", path, 0.0, 20.0);
    const auto color_tolerance =
        fields_.integer(value, "colorTolerance", path, 0, 255);
    const auto stable = fields_.integer(value, "stableSamples", path, 1, 15);
    if (!confidence || !scale || !color_tolerance || !stable) return false;
    state.confidence_threshold = *confidence;
    state.scale_tolerance = *scale / 100.0;
    state.color_tolerance = static_cast<std::uint8_t>(*color_tolerance);
    state.stable_samples = static_cast<std::uint8_t>(*stable);
    if (!read_repeat_policy(value, path, state.repeat)) return false;
    output.push_back(std::move(state));
    return true;
  }

  bool read_trigger(const json::Value &value, const std::size_t index,
                    const std::int64_t schema_version,
                    std::vector<VisualTrigger> &output) {
    const auto path = "$.triggers[" + std::to_string(index) + "]";
    if (!value.is_object()) return set(path, "visual trigger must be an object");
    if ((schema_version == 1 &&
         !fields_.exact_fields(value,
                 {"cooldownId", "ruleId", "enabled", "mode", "keepInput",
                  "confirmationTimeoutMs", "onUnavailable"},
                 path)) ||
        (schema_version == 2 &&
         !fields_.exact_fields(value,
                 {"cooldownId", "observationId", "enabled", "mode",
                  "keepInput", "confirmationTimeoutMs", "onUnavailable"},
                 path)))
      return false;
    VisualTrigger trigger;
    if (!fields_.string(value, "cooldownId", path, trigger.cooldown_id) ||
        !fields_.string(value, schema_version == 1 ? "ruleId" : "observationId",
                        path, trigger.observation_id))
      return false;
    trigger.enabled = true;
    if (!fields_.boolean(value, "enabled", path, trigger.enabled,
                         json::Required::no))
      return false;
    std::string mode;
    if (!fields_.string(value, "mode", path, mode)) return false;
    const auto parsed_mode = parse_enum_wire(visual_trigger_mode_wire, mode);
    if (parsed_mode == VisualTriggerMode::pixel_only) {
      trigger.mode = VisualTriggerMode::pixel_only;
      if (value.find("confirmationTimeoutMs") != nullptr)
        return set(path + ".confirmationTimeoutMs",
                   "field is only valid for confirmInput");
      trigger.keep_input = false;
      if (!fields_.boolean(value, "keepInput", path, trigger.keep_input,
                           json::Required::no))
        return false;
    } else if (parsed_mode == VisualTriggerMode::confirm_input) {
      trigger.mode = VisualTriggerMode::confirm_input;
      if (value.find("keepInput") != nullptr)
        return set(path + ".keepInput",
                   "field is only valid for pixelOnly");
      const auto timeout = fields_.integer(value, "confirmationTimeoutMs", path,
                                   50, 5'000);
      if (!timeout)
        return false;
      trigger.confirmation_timeout = Milliseconds(*timeout);
    } else {
      return set(path + ".mode", "expected pixelOnly or confirmInput");
    }
    std::string unavailable = "ignore";
    if (const auto *policy_value = value.find("onUnavailable");
        policy_value != nullptr) {
      if (!policy_value->is_string())
        return set(path + ".onUnavailable", "expected string");
      unavailable = policy_value->as_string();
    }
    if (const auto policy = parse_enum_wire(capture_unavailable_policy_wire, unavailable))
      trigger.unavailable_policy = *policy;
    else
      return set(path + ".onUnavailable",
                  "expected ignore or triggerFromInput");
    output.push_back(std::move(trigger));
    return true;
  }

  VisualError failure() const {
    return {fields_.error().path, fields_.error().message};
  }
  VisualError error(std::string path, std::string message) {
    return {.path = std::move(path), .message = std::move(message)};
  }
  bool set(std::string path, std::string message) {
    return fields_.fail(std::move(path), std::move(message));
  }

  const json::Value &root_;
  const GameProfile &base_;
  json::Reader fields_;
  std::int64_t schema_version_{};
  std::vector<std::size_t> rule_indices_;
  std::vector<std::size_t> template_indices_;
};

} // namespace

bool valid_normalized_region(const NormalizedRegion &region) noexcept {
  return std::isfinite(region.x) && std::isfinite(region.y) &&
         std::isfinite(region.width) && std::isfinite(region.height) &&
         region.x >= 0.0 && region.y >= 0.0 && region.width > 0.0 &&
         region.height > 0.0 &&
         region.x + region.width <= 1.0 + 1e-12 &&
         region.y + region.height <= 1.0 + 1e-12;
}

VisualError validate_visual_profile(
    const VisualProfile &profile, const GameProfile &base_profile,
    const std::function<std::string(bool, std::size_t)> &source_path) {
  const auto invalid = [](std::string path, std::string message) {
    return VisualError{std::move(path), std::move(message)};
  };
  if (profile.schema_version != 2)
    return invalid("$.schemaVersion", "unsupported visual schema version");
  if (profile.profile_id != base_profile.id)
    return invalid("$.profileId", "visual sidecar does not match profile id");
  if (base_profile.game.global && !profile.triggers.empty())
    return invalid("$.triggers", "global profiles cannot use visual capture");
  if (profile.sample_rate_hz != 5 && profile.sample_rate_hz != 10 &&
      profile.sample_rate_hz != 15)
    return invalid("$.sampleRateHz", "sample rate must be 5, 10, or 15 Hz");
  if (profile.rules.size() > max_pixel_rules)
    return invalid("$.observations", "at most four pixel rules are supported");
  if (profile.templates.size() > max_template_states)
    return invalid("$.observations",
                   "at most four template observations are supported");
  // Without a source mapper, cite the schema-2 position the serializer emits:
  // pixel rules first, then templates, in one $.observations array.
  const auto observation_path = [&](const bool pixel, const std::size_t index) {
    if (source_path) return source_path(pixel, index);
    return "$.observations[" +
           std::to_string(pixel ? index : profile.rules.size() + index) + "]";
  };

  std::set<std::string, std::less<>> observation_ids;
  for (std::size_t index = 0; index < profile.rules.size(); ++index) {
    const auto &rule = profile.rules[index];
    const auto path = observation_path(true, index);
    if (rule.id.empty() || rule.name.empty())
      return invalid(path, "rule id and name are required");
    if (!observation_ids.insert(rule.id).second)
      return invalid(path + ".id", "duplicate visual observation id");
    if (!valid_normalized_region(rule.region))
      return invalid(path + ".region",
                     "normalized region must be positive and remain in frame");
    if (rule.stable_samples < 1 || rule.stable_samples > 15)
      return invalid(path + ".stableSamples",
                     "stable sample count must be between 1 and 15");
    if (const auto *coverage =
            std::get_if<ColorCoveragePredicate>(&rule.predicate);
        coverage != nullptr &&
        (!std::isfinite(coverage->minimum_fraction) ||
         coverage->minimum_fraction < 0.0 ||
         coverage->minimum_fraction > 1.0))
      return invalid(path + ".predicate.minimumPercent",
                     "minimum percentage is outside the supported range");
  }

  for (std::size_t index = 0; index < profile.templates.size(); ++index) {
    const auto &state = profile.templates[index];
    const auto path = observation_path(false, index);
    if (state.id.empty() || state.name.empty())
      return invalid(path, "template id and name are required");
    if (!observation_ids.insert(state.id).second)
      return invalid(path + ".id", "duplicate visual observation id");
    if (!valid_normalized_region(state.search_region))
      return invalid(path + ".searchRegion",
                     "normalized search region must remain in frame");
    if (!std::isfinite(state.expected_width) ||
        !std::isfinite(state.expected_height) ||
        state.expected_width <= 0.0 || state.expected_width > 1.0 ||
        state.expected_height <= 0.0 || state.expected_height > 1.0)
      return invalid(path + ".expectedSize",
                     "expected template size must be normalized and positive");
    if (state.expected_width > state.search_region.width + 1e-12 ||
        state.expected_height > state.search_region.height + 1e-12)
      return invalid(path + ".expectedSize",
                     "expected template size must fit inside its search region");
    if (state.references.empty() ||
        state.references.size() > max_template_references)
      return invalid(path + ".references",
                     "one to four template references are required");
    std::set<std::string, std::less<>> references;
    for (std::size_t reference_index = 0;
         reference_index < state.references.size(); ++reference_index) {
      const auto &reference = state.references[reference_index];
      const auto reference_path =
          path + ".references[" + std::to_string(reference_index) + "].asset";
      if (!is_lowercase_sha256(reference.asset_sha256))
        return invalid(reference_path,
                       "asset must be a lower-case SHA-256 digest");
      if (!references.insert(reference.asset_sha256).second)
        return invalid(reference_path, "duplicate template reference");
    }
    if (!std::isfinite(state.confidence_threshold) ||
        state.confidence_threshold < 0.0 ||
        state.confidence_threshold > 1.0)
      return invalid(path + ".confidence",
                     "confidence must be between zero and one");
    if (!std::isfinite(state.scale_tolerance) ||
        state.scale_tolerance < 0.0 || state.scale_tolerance > 0.20)
      return invalid(path + ".scaleTolerancePercent",
                     "scale tolerance must be between zero and 20 percent");
    if (state.stable_samples < 1 || state.stable_samples > 15)
      return invalid(path + ".stableSamples",
                     "stable sample count must be between 1 and 15");
  }

  std::set<std::string, std::less<>> cooldown_ids;
  for (const auto &cooldown : base_profile.cooldowns)
    cooldown_ids.insert(cooldown.id);
  std::set<std::string, std::less<>> assigned;
  for (std::size_t index = 0; index < profile.triggers.size(); ++index) {
    const auto &trigger = profile.triggers[index];
    const auto path = "$.triggers[" + std::to_string(index) + "]";
    if (!cooldown_ids.contains(trigger.cooldown_id))
      return invalid(path + ".cooldownId", "unknown cooldown id");
    if (!has_observation(profile, trigger.observation_id))
      return invalid(path + ".observationId", "unknown visual observation id");
    if (!assigned.insert(trigger.cooldown_id).second)
      return invalid(path + ".cooldownId",
                     "only one visual trigger per cooldown is supported");
    if (trigger.mode == VisualTriggerMode::confirm_input) {
      const bool has_binding = std::ranges::any_of(
          base_profile.bindings, [&](const InputBinding &binding) {
            return binding.cooldown_id == trigger.cooldown_id;
          });
      if (!has_binding)
        return invalid(path, "input confirmation requires an input binding");
      if (trigger.confirmation_timeout < Milliseconds(50) ||
          trigger.confirmation_timeout > Milliseconds(5'000))
        return invalid(path + ".confirmationTimeoutMs",
                       "confirmation timeout is outside the supported range");
    }
  }
  return {};
}

const PixelRule *find_pixel_rule(const VisualProfile &profile,
                                 const std::string_view id) noexcept {
  const auto found = std::ranges::find(profile.rules, id, &PixelRule::id);
  return found == profile.rules.end() ? nullptr : &*found;
}

const TemplateState *find_template_state(const VisualProfile &profile,
                                         const std::string_view id) noexcept {
  const auto found =
      std::ranges::find(profile.templates, id, &TemplateState::id);
  return found == profile.templates.end() ? nullptr : &*found;
}

bool has_observation(const VisualProfile &profile,
                     const std::string_view id) noexcept {
  return find_pixel_rule(profile, id) != nullptr ||
         find_template_state(profile, id) != nullptr;
}

std::uint8_t observation_stable_samples(const VisualProfile &profile,
                                        const std::string_view id) noexcept {
  if (const auto *rule = find_pixel_rule(profile, id)) return rule->stable_samples;
  if (const auto *state = find_template_state(profile, id))
    return state->stable_samples;
  return 1;
}

PixelRepeatPolicy observation_repeat_policy(
    const VisualProfile &profile, const std::string_view id) noexcept {
  if (const auto *rule = find_pixel_rule(profile, id)) return rule->repeat;
  if (const auto *state = find_template_state(profile, id))
    return state->repeat;
  return PixelRepeatPolicy::edge;
}

VisualResult parse_visual_profile_json(const std::string_view source,
                                       const GameProfile &base_profile) {
  if (source.size() > max_visual_json_bytes)
    return VisualError{"$", "visual sidecar exceeds 256 KiB limit"};
  const auto parsed = json::parse_bounded(source, max_visual_json_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return VisualError{error->path, error->message};
  return parse_visual_profile_value(std::get<json::Value>(parsed), base_profile);
}

VisualResult parse_visual_profile_value(const json::Value &root,
                                        const GameProfile &base_profile) {
  return Reader(root, base_profile).read();
}

std::string serialize_visual_profile_json(const VisualProfile &profile) {
  std::string output;
  output.reserve(2'048);
  output += "{\n  \"schemaVersion\": 2,\n  \"profileId\": ";
  json::append_quoted(output, profile.profile_id);
  output += ",\n  \"sampleRateHz\": " +
            std::to_string(profile.sample_rate_hz) + ",\n  \"quality\": ";
  json::append_quoted(output, enum_wire_name(capture_quality_wire, profile.quality).value_or("balanced"));
  output += ",\n  \"observations\": [\n";
  const auto observation_count =
      profile.rules.size() + profile.templates.size();
  std::size_t observation_index{};
  for (const auto &rule : profile.rules) {
    output += "    {\n      \"type\": \"pixel\",\n      \"id\": ";
    json::append_quoted(output, rule.id);
    output += ",\n      \"name\": "; json::append_quoted(output, rule.name);
    output += ",\n      \"region\": {\"x\": "; json::append_number(output, rule.region.x);
    output += ", \"y\": "; json::append_number(output, rule.region.y);
    output += ", \"width\": "; json::append_number(output, rule.region.width);
    output += ", \"height\": "; json::append_number(output, rule.region.height);
    output += "},\n      \"predicate\": {";
    std::visit(
        [&](const auto &predicate) {
          using T = std::decay_t<decltype(predicate)>;
          if constexpr (std::is_same_v<T, AverageColorPredicate>) {
            output += "\"type\": \"averageColor\", \"target\": ";
            json::append_color_quoted(output, predicate.target_color);
            output += ", \"tolerance\": " +
                      std::to_string(predicate.tolerance);
          } else if constexpr (std::is_same_v<T, BrightnessPredicate>) {
            output += "\"type\": \"brightness\", \"comparison\": \"";
output += enum_wire_name(brightness_wire, predicate.comparison).value_or("below");
            output += "\", \"threshold\": " +
                      std::to_string(predicate.threshold);
          } else {
            output += "\"type\": \"colorCoverage\", \"target\": ";
            json::append_color_quoted(output, predicate.target_color);
            output += ", \"tolerance\": " +
                      std::to_string(predicate.tolerance) +
                      ", \"minimumPercent\": ";
            json::append_number(output, predicate.minimum_fraction * 100.0);
          }
        },
        rule.predicate);
    output += "},\n      \"stableSamples\": " +
              std::to_string(rule.stable_samples) + ",\n      \"repeat\": \"" +
              std::string(enum_wire_name(pixel_repeat_wire, rule.repeat).value_or("edge")) + "\"\n    }";
    output += ++observation_index == observation_count ? "\n" : ",\n";
  }
  for (const auto &state : profile.templates) {
    output += "    {\n      \"type\": \"template\",\n      \"id\": ";
    json::append_quoted(output, state.id);
    output += ",\n      \"name\": ";
    json::append_quoted(output, state.name);
    output += ",\n      \"searchRegion\": {\"x\": ";
    json::append_number(output, state.search_region.x);
    output += ", \"y\": ";
    json::append_number(output, state.search_region.y);
    output += ", \"width\": ";
    json::append_number(output, state.search_region.width);
    output += ", \"height\": ";
    json::append_number(output, state.search_region.height);
    output += "},\n      \"expectedSize\": {\"width\": ";
    json::append_number(output, state.expected_width);
    output += ", \"height\": ";
    json::append_number(output, state.expected_height);
    output += "},\n      \"references\": [";
    for (std::size_t index = 0; index < state.references.size(); ++index) {
      if (index != 0) output += ", ";
      output += "{\"asset\": ";
      json::append_quoted(output, state.references[index].asset_sha256);
      output += "}";
    }
    output += "],\n      \"confidence\": ";
    json::append_number(output, state.confidence_threshold);
    output += ",\n      \"scaleTolerancePercent\": ";
    json::append_number(output, state.scale_tolerance * 100.0);
    output += ",\n      \"colorTolerance\": " +
              std::to_string(state.color_tolerance);
    output += ",\n      \"stableSamples\": " +
              std::to_string(state.stable_samples);
    output += ",\n      \"repeat\": \"" +
              std::string(enum_wire_name(pixel_repeat_wire, state.repeat).value_or("edge")) + "\"\n    }";
    output += ++observation_index == observation_count ? "\n" : ",\n";
  }
  output += "  ],\n  \"triggers\": [\n";
  for (std::size_t index = 0; index < profile.triggers.size(); ++index) {
    const auto &trigger = profile.triggers[index];
    output += "    {\n      \"cooldownId\": "; json::append_quoted(output, trigger.cooldown_id);
    output += ",\n      \"observationId\": ";
    json::append_quoted(output, trigger.observation_id);
    output += ",\n      \"enabled\": ";
    json::append_bool(output, trigger.enabled);
    output += ",\n      \"mode\": \"" +
              std::string(enum_wire_name(visual_trigger_mode_wire, trigger.mode)
                              .value_or("confirmInput")) +
              "\"";
    if (trigger.mode == VisualTriggerMode::pixel_only) {
      output += ",\n      \"keepInput\": ";
      json::append_bool(output, trigger.keep_input);
    } else {
      output += ",\n      \"confirmationTimeoutMs\": " +
                std::to_string(trigger.confirmation_timeout.count());
    }
    output += ",\n      \"onUnavailable\": \"" +
              std::string(enum_wire_name(capture_unavailable_policy_wire,
                                         trigger.unavailable_policy)
                              .value_or("ignore")) +
              "\"";
    output += "\n    }";
    output += index + 1 == profile.triggers.size() ? "\n" : ",\n";
  }
  output += "  ]\n}\n";
  return output;
}

bool has_enabled_visual_assignments(const VisualProfile &profile) noexcept {
  return std::ranges::any_of(profile.triggers,
                             &VisualTrigger::enabled);
}

} // namespace layerc::core
