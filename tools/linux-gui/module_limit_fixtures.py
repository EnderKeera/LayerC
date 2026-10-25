"""Module-boundary inputs for the test-only compiled resource probe.

This generates candidates, not acceptance claims: every expected verdict must
be checked by the current production parser/validator. Bounds are supplied by
resource_probe --limits. No production path is read or written.
"""
import copy
import hashlib
import json
from pathlib import Path

from limit_fixtures import base_profile, wire


def write_module_fixtures(output, limits):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    base = base_profile()
    (output / "base.json").write_bytes(wire(base))
    records = []

    def save(name, kind, value, accepted=True, **dimensions):
        data = wire(value)
        path = output / (name + ".json")
        path.write_bytes(data)
        records.append({"id": name, "kind": kind, "file": path.name,
                        "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                        "expectedAccepted": accepted, **dimensions})

    literal = {"op": "literal", "valueType": "bool", "value": True}

    def rule(index=0):
        return {"id": f"r{index}", "event": "inputPressed", "condition": copy.deepcopy(literal),
                "actions": [{"type": "triggerCooldown", "target": "c0"}]}

    def module():
        return {"schemaVersion": 1, "profileId": base["id"], "variables": [],
                "automations": [{"id": "a0", "name": "Boundary workload", "enabled": True,
                                 "rules": [rule()]}]}

    item = module()
    item["variables"] = [{"id": f"v{i}", "type": "bool", "default": False,
                          "persistent": False} for i in range(limits["variables"])]
    item["automations"][0]["rules"] = [rule(i) for i in range(limits["rules"])]
    save("automation-rules-variables-limit", "automation", item,
         rules=limits["rules"], variables=limits["variables"])
    over = copy.deepcopy(item)
    over["automations"][0]["rules"].append(rule(limits["rules"]))
    save("automation-rules-over-limit", "automation", over, accepted=False)
    over = copy.deepcopy(item)
    over["variables"].append({"id": "overflow", "type": "bool", "default": False})
    save("automation-variables-over-limit", "automation", over, accepted=False)

    item = module()
    actions = [{"type": "triggerCooldown", "target": "c0"}
               for _ in range(limits["actionsPerRule"])]
    item["automations"][0]["rules"][0]["actions"] = actions
    save("automation-actions-limit", "automation", item, actions=limits["actionsPerRule"])
    over = copy.deepcopy(item)
    over["automations"][0]["rules"][0]["actions"].append(actions[0])
    save("automation-actions-over-limit", "automation", over, accepted=False)

    # A unary chain brackets depth. Count convention is deliberately checked
    # by adjacent real-validator verdicts, never inferred from Python recursion.
    item = module()
    expression = copy.deepcopy(literal)
    for _ in range(limits["astDepth"] - 1):
        expression = {"op": "not", "args": [expression]}
    item["automations"][0]["rules"][0]["condition"] = expression
    save("automation-depth-limit", "automation", item, astDepth=limits["astDepth"])
    over = copy.deepcopy(item)
    over["automations"][0]["rules"][0]["condition"] = {"op": "not", "args": [expression]}
    save("automation-depth-over-limit", "automation", over, accepted=False)

    def expression_nodes(count):
        if count == 1:
            return copy.deepcopy(literal)
        if count % 2 == 0:
            return {"op": "not", "args": [expression_nodes(count - 1)]}
        left = (count - 1) // 2
        return {"op": "and", "args": [expression_nodes(left),
                                         expression_nodes(count - 1 - left)]}

    item = module()
    item["automations"][0]["rules"][0]["condition"] = expression_nodes(limits["nodesPerRule"])
    save("automation-nodes-limit", "automation", item, nodesPerRule=limits["nodesPerRule"])
    over = copy.deepcopy(item)
    over["automations"][0]["rules"][0]["condition"] = expression_nodes(limits["nodesPerRule"] + 1)
    save("automation-nodes-over-limit", "automation", over, accepted=False)

    item = module()
    item["variables"] = [{"id": "strings", "type": "list", "persistent": False,
                          "default": ["x" * limits["stringBytes"]
                                      for _ in range(limits["collectionEntries"])]}]
    save("automation-collection-string-limit", "automation", item,
         entries=limits["collectionEntries"], stringBytes=limits["stringBytes"])
    over = copy.deepcopy(item)
    over["variables"][0]["default"].append("x")
    save("automation-collection-over-limit", "automation", over, accepted=False)
    over = copy.deepcopy(item)
    over["variables"][0]["default"][0] += "x"
    save("automation-string-over-limit", "automation", over, accepted=False)

    # Largest number of max-sized list variables within the module byte limit,
    # under the independent compiled variable count. Padding exercises input
    # bytes too, but is not presented as extra expression/collection content.
    candidate = copy.deepcopy(item)
    variables = []
    for i in range(limits["variables"]):
        variable = copy.deepcopy(item["variables"][0])
        variable["id"] = f"v{i}"
        candidate["variables"] = variables + [variable]
        if len(wire(candidate)) > limits["automationBytes"]:
            break
        variables.append(variable)
    candidate["variables"] = variables
    data = wire(candidate)
    if not variables or len(data) > limits["automationBytes"]:
        raise AssertionError("invalid module byte-bound candidate")
    padded = data + b" " * (limits["automationBytes"] - len(data))
    path = output / "automation-byte-limit.json"
    path.write_bytes(padded)
    records.append({"id": "automation-byte-limit", "kind": "automation", "file": path.name,
                    "bytes": len(padded), "sha256": hashlib.sha256(padded).hexdigest(),
                    "expectedAccepted": True, "variables": len(variables),
                    "entriesEach": limits["collectionEntries"], "stringBytes": limits["stringBytes"]})
    path = output / "automation-one-byte-over-limit.json"
    data = padded + b" "
    path.write_bytes(data)
    records.append({"id": "automation-one-byte-over-limit", "kind": "automation", "file": path.name,
                    "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                    "expectedAccepted": False})

    region = {"x": 0.0, "y": 0.0, "width": 0.5, "height": 0.5}
    pixels = [{"type": "pixel", "id": f"p{i}", "name": f"Pixel {i}",
               "region": copy.deepcopy(region), "stableSamples": 1, "repeat": "edge",
               "predicate": {"type": "brightness", "comparison": "above", "threshold": 128}}
              for i in range(limits["pixelRules"])]
    templates = [{"type": "template", "id": f"t{i}", "name": f"Template {i}",
                  "searchRegion": copy.deepcopy(region),
                  "expectedSize": {"width": 0.25, "height": 0.25},
                  "references": [{"asset": f"{i * limits['referencesPerTemplate'] + j + 1:064x}"}
                                 for j in range(limits["referencesPerTemplate"])],
                  "confidence": 0.9, "scaleTolerancePercent": 0, "colorTolerance": 10,
                  "stableSamples": 1, "repeat": "edge"}
                 for i in range(limits["templateStates"])]
    visual = {"schemaVersion": 2, "profileId": base["id"], "sampleRateHz": 5,
              "quality": "balanced", "observations": pixels + templates, "triggers": []}
    save("visual-observation-reference-limit", "visual", visual,
         pixelRules=len(pixels), templateStates=len(templates),
         referencesEach=limits["referencesPerTemplate"])
    for kind, observation in (("pixel", pixels[0]), ("template", templates[0])):
        over = copy.deepcopy(visual)
        extra = copy.deepcopy(observation)
        extra["id"] = "overflow"
        over["observations"].append(extra)
        save("visual-" + kind + "-over-limit", "visual", over, accepted=False)
    over = copy.deepcopy(visual)
    over["observations"][len(pixels)]["references"].append({"asset": "f" * 64})
    save("visual-references-over-limit", "visual", over, accepted=False)
    manifest = {"compiledLimits": limits, "cases": records,
                "scope": "Module parser/validator boundary candidates only. Template hashes are syntactically valid, not decoded assets; live capture/template/trace resource acceptance is separate."}
    (output / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest
