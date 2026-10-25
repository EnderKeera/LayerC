"""Validator-derived valid/adversarial documents; no production store access.

The byte/depth ceilings come from resource_probe --limits, not parallel parser
constants. Collection maxima are the largest count in each stated minimal-wire
family under the byte limit (n fits, n+1 cannot fit); there is no invented global
cooldown/panel count. Opaque arrays/depth stress accepted unknown data separately.
"""
import copy
import hashlib
import json
from pathlib import Path


def wire(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def base_profile():
    return {"schemaVersion": 4, "id": "limit", "name": "Limit document", "enabled": False,
            "game": {"executableName": "lctest.exe"}, "modules": [], "hidBindings": [],
            "cooldownGroups": [],
            "cooldowns": [{"id": "c0", "name": "x", "durationMs": 1, "recovery": "single"}],
            "panels": [{"id": "p0", "name": "x", "cooldowns": ["c0"]}],
            "bindings": [{"cooldownId": "c0", "chord": [{"device": "keyboard", "code": 69}]}]}


def largest_family(profile, key, factory, ceiling):
    """Binary-search the exact largest n in this fixed, monotonically sized family."""
    def candidate(count):
        item = copy.deepcopy(profile)
        item[key] = [factory(index) for index in range(count)]
        return wire(item)
    low, high = 1, ceiling + 1
    if len(candidate(low)) > ceiling:
        raise ValueError("even the minimal validator workload exceeds its byte ceiling")
    while low + 1 < high:
        middle = (low + high) // 2
        if len(candidate(middle)) <= ceiling:
            low = middle
        else:
            high = middle
    data, overflow = candidate(low), candidate(low + 1)
    if not (len(data) <= ceiling < len(overflow)):
        raise AssertionError("collection maximum does not bracket the compiled byte limit")
    return data, low, len(overflow)


def write_fixtures(output, limits):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    ceiling, depth = limits["profileBytes"], limits["jsonDepth"]
    if type(ceiling) is not int or type(depth) is not int or not 1024 <= ceiling <= 1024**2 or depth < 8:
        raise ValueError("unexpected compiled profile/depth bounds: review the workload before running")
    records, profile = [], base_profile()

    def save(name, data, accepted=True, **details):
        path = output / (name + ".json")
        path.write_bytes(data)
        records.append({"id": name, "file": path.name, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "expectedAccepted": accepted, **details})
        return data

    baseline = wire(profile)
    save("baseline", baseline)
    # A single near-limit preserved string and a high-node-count preserved
    # value are different allocation shapes; neither substitutes for the other.
    item = copy.deepcopy(profile)
    item["future"] = ""
    item["future"] = "x" * (ceiling - len(wire(item)))
    save("opaque-string-byte-limit", wire(item), inputByteLimit=ceiling)
    item["future"] = []
    overhead = len(wire(item))
    count = (ceiling - overhead + 1) // 3  # [] elements: 2n bytes + n-1 commas.
    item["future"] = [[] for _ in range(count)]
    dense = wire(item)
    save("opaque-array-byte-limit", dense + b" " * (ceiling - len(dense)),
         opaqueNodes=count + 1, inputByteLimit=ceiling)

    # Object/map allocations differ from empty arrays. Exercise the smallest
    # one-member opaque object, without imposing schema rules on future data.
    item["future"] = []
    count = (ceiling - len(wire(item)) + 1) // 7  # {"":0} plus comma.
    item["future"] = [{"": 0} for _ in range(count)]
    dense = wire(item)
    if len(dense) > ceiling or len(dense) + 7 <= ceiling:
        raise RuntimeError("opaque object family did not reach the byte frontier")
    save("opaque-object-byte-limit", dense + b" " * (ceiling - len(dense)),
         opaqueObjects=count, opaqueNodes=2 * count + 1, inputByteLimit=ceiling)

    # The root and outer future-array consume two levels; each repeated chain
    # reaches the last accepted container depth. Its one-level-over twin must
    # be rejected by the real parser/validator rather than truncating data.
    prefix = baseline[:-1] + b',"future":['
    chain_depth = depth - 2
    chain = b"[" * chain_depth + b"null" + b"]" * chain_depth
    chain_count = (ceiling - len(prefix) - 2 + 1) // (len(chain) + 1)
    nested = prefix + b",".join([chain] * chain_count) + b"]}"
    save("opaque-depth-byte-limit", nested + b" " * (ceiling - len(nested)),
         chainDepth=chain_depth, chainCount=chain_count, inputByteLimit=ceiling)
    excessive = baseline[:-1] + b',"future":' + b"[" * depth + b"null" + b"]" * depth + b"}"
    save("opaque-depth-over-limit", excessive, accepted=False, attemptedContainers=depth + 1)
    save("profile-one-byte-over-limit", baseline + b" " * (ceiling + 1 - len(baseline)),
         accepted=False, inputByteLimit=ceiling)

    families = (
        ("cooldowns", lambda i: {"id": f"c{i}", "name": "x", "durationMs": 1, "recovery": "single"}),
        ("panels", lambda i: {"id": f"p{i}", "name": "x", "cooldowns": ["c0"]}),
        ("bindings", lambda _i: {"cooldownId": "c0", "chord": [{"device": "keyboard", "code": 69}]}),
    )
    for key, factory in families:
        data, count, next_bytes = largest_family(profile, key, factory, ceiling)
        save(key + "-byte-limit", data, collection=key, count=count,
             nextCountBytes=next_bytes, inputByteLimit=ceiling)
    manifest = {"compiledLimits": limits, "cases": records,
                "scope": "Base-profile byte/depth/collection families. Module/trace/template limits are separate workloads; not yet qualified by this manifest."}
    (output / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest
