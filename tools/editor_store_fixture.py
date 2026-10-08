#!/usr/bin/env python3
"""Writes a representative LayerC profile store for editor resource checks.

The default matches the Linux editor plan's reference store: 20 profiles,
10 cooldowns each, two panels each. Profiles are schema-1 JSON that every
current reader upgrades in memory; nothing here touches a real store.

Usage: editor_store_fixture.py OUTPUT_DIR [--profiles N] [--cooldowns N]
                                [--panels N]
"""
import argparse
import json
from pathlib import Path

PALETTE = ["#50BAC8", "#C8A050", "#8FC850", "#C85078"]


def profile(index: int, cooldowns: int, panels: int) -> dict:
    ident = f"fixture-{index:02d}"
    cooldown_ids = [f"skill-{n:02d}" for n in range(cooldowns)]
    groups = [cooldown_ids[p::panels] for p in range(panels)]
    return {
        "schemaVersion": 1,
        "id": ident,
        "name": f"Fixture {index:02d}",
        "game": {"executableName": f"fixture{index:02d}.exe"},
        "cooldowns": [
            {
                "id": cid,
                "name": f"Skill {n:02d}",
                "durationMs": 1000 + 500 * n,
                "maxCharges": 1 + n % 3,
                "retrigger": "ignore",
            }
            for n, cid in enumerate(cooldown_ids)
        ],
        "panels": [
            {
                "id": f"panel-{p}",
                "name": f"Panel {p}",
                "layout": "row",
                "style": "bar",
                "bar": {
                    "width": 300,
                    "height": 24,
                    "fillColor": PALETTE[p % len(PALETTE)],
                    "backgroundColor": "#202A31",
                    "borderColor": "#80D8E8",
                    "textColor": "#F1F5F7",
                    "hideWhenReady": False,
                },
                "anchorX": 0.5,
                "anchorY": round(0.6 + 0.1 * p, 2),
                "cooldowns": group,
            }
            for p, group in enumerate(groups)
            if group
        ],
        "bindings": [
            # Keyboard scan codes 16.. ("Q" row onward), one key each.
            {"cooldownId": cid, "chord": [{"device": "keyboard", "code": 16 + n}]}
            for n, cid in enumerate(cooldown_ids)
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Write a representative LayerC profile store.")
    parser.add_argument("output", type=Path)
    parser.add_argument("--profiles", type=int, default=20)
    parser.add_argument("--cooldowns", type=int, default=10)
    parser.add_argument("--panels", type=int, default=2)
    args = parser.parse_args()
    if not (1 <= args.cooldowns <= 40 and 1 <= args.panels <= args.cooldowns):
        parser.error("cooldowns must be 1..40 and panels 1..cooldowns")
    args.output.mkdir(parents=True, exist_ok=True)
    for index in range(1, args.profiles + 1):
        path = args.output / f"fixture-{index:02d}.json"
        path.write_text(
            json.dumps(profile(index, args.cooldowns, args.panels), indent=2) + "\n",
            encoding="utf-8",
        )
    print(f"wrote {args.profiles} profiles to {args.output}")


if __name__ == "__main__":
    main()
