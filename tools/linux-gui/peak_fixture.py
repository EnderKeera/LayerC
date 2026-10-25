"""The exact representative store used for active advanced-operation peaks."""
import copy
import hashlib
import json
from pathlib import Path


def write_representative(profile, store, evidence):
    store, evidence = Path(store), Path(evidence)
    if any(store.iterdir()):
        raise RuntimeError("representative resource fixture requires an empty private store")
    inventory = []
    for number in range(1, 21):
        item = copy.deepcopy(profile)
        item.update(id="fixture" if number == 1 else f"fixture-{number:02d}",
                    name="GUI fixture" if number == 1 else f"Fixture {number:02d}",
                    enabled=number == 1)
        ids = ["ability"] + [f"ability-{c:02d}" for c in range(2, 11)]
        item["cooldowns"] = [
            {"id": cid, "name": f"Ability {c}", "durationMs": 8000,
             "maxCharges": 1, "retrigger": "ignore", "recovery": "single"}
            for c, cid in enumerate(ids, 1)]
        second = copy.deepcopy(item["panels"][0])
        second.update(id="side", name="Side", anchorX=0.75, anchorY=0.25)
        item["panels"][0]["cooldowns"], second["cooldowns"] = ids[:5], ids[5:]
        item["panels"].append(second)
        item["bindings"] = [{"cooldownId": "ability",
                             "chord": [{"device": "keyboard", "code": 69}]}]
        name = "fixture.json" if number == 1 else f"fixture-{number:02d}.json"
        data = (json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        (store / name).write_bytes(data)
        inventory.append({"name": name, "sha256": hashlib.sha256(data).hexdigest(),
                          "cooldowns": len(item["cooldowns"]), "panels": len(item["panels"])})
    (evidence / "representative-fixture.json").write_text(
        json.dumps({"profiles": inventory}, indent=2) + "\n", encoding="utf-8")
