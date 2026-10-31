"""Canonical ordered required-case manifest for the production X11 walkthrough.

Feature modules own their descriptions; only this module assembles execution
order. The runner and report import the same manifest. No thresholds or case
requirements are inferred from observed results.
"""
from automation_walkthrough import EXPECTED as AUTOMATION_EXPECTED
from items_walkthrough import EXPECTED as ITEMS_EXPECTED
from autosave_walkthrough import EXPECTED as AUTOSAVE_EXPECTED
from runtime_walkthrough import EXPECTED as RUNTIME_EXPECTED
from device_walkthrough import EXPECTED as DEVICE_EXPECTED
from tools_walkthrough import EXPECTED as TOOLS_EXPECTED
from parity_walkthrough import EXPECTED as PARITY_EXPECTED
from visual_walkthrough import EXPECTED as VISUAL_EXPECTED
from overlay_walkthrough import EXPECTED as OVERLAY_EXPECTED


def merge_cases(*groups):
    """Keep declared order and fail visibly instead of overwriting duplicate IDs."""
    result = {}
    for group in groups:
        duplicate = result.keys() & group.keys()
        if duplicate:
            raise ValueError(f"duplicate required case IDs: {', '.join(sorted(duplicate))}")
        result.update(group)
    return result


EXPECTED = merge_cases(
    {
        "bridge-guards": "Bad token, out-of-screen input, stale screenshot and unknown "
                         "operations are rejected and logged without acting.",
        "startup": "Editor and game fixture are visible; the profile is a file; LayerD "
                   "answers; LayerE exports an AT-SPI tree with its bridge status.",
        "accessibility-semantics": "Through AT-SPI alone: names, roles, focus, keyboard Tab, "
                                   "Unicode text, tab/combo selection, menu Save and a "
                                   "cancelled modal Delete, with the matching events.",
        "keyboard-navigation": "Tab/Shift+Tab move focus between fields; Ctrl+2/Ctrl+1 switch "
                               "pages; Alt+F opens the File menu and Escape closes it.",
        "menus-context": "The empty-list context menu leaves the document unchanged; the "
                         "selected-row context menu duplicates the profile; Profile > Delete "
                         "is cancelled with Escape, then confirmed, and removes the copy.",
        "profiles-crud": "Ctrl+N creates a profile that saves durably; the enabled check box "
                         "autosaves; undo/redo restore durable values; an invalid pending "
                         "selection change prompts, cancels, then discards.",
        "invalid-no-write": "Duration 'nan' is reported as invalid while typing and after "
                            "Ctrl+S, and the durable profile stays byte-identical.",
        "edit-commit-reload-overlay": "Duration 3.25 s and panel X 0.75 are saved durably, "
                                      "LayerD reloads, and the live overlay moves right.",
        "import-export": "Export JSON through the file chooser writes the saved bytes; "
                         "import of the same file reports the collision, copies it on "
                         "request, and a cancelled import writes nothing.",
    },
    AUTOMATION_EXPECTED,
    ITEMS_EXPECTED,
    VISUAL_EXPECTED,
    AUTOSAVE_EXPECTED,
    RUNTIME_EXPECTED,
    DEVICE_EXPECTED,
    TOOLS_EXPECTED,
    PARITY_EXPECTED,
    {
        "layout": "Corner drags grow the window and clamp it at 640x480 with scrolling; "
                  "title-bar double-clicks maximize and restore; Ctrl+Plus/Ctrl+0 scale "
                  "and restore the window.",
        "daemon-reconnect": "With LayerD stopped the status says so and Save commits offline; "
                            "after LayerD restarts the status recovers and a Save is "
                            "acknowledged.",
        "store-failure-visible": "When the store cannot replace the profile, Save fails "
                                 "visibly and the original profile bytes survive.",
    },
    # Last: these resize the target, stop LayerD and toggle the profile.
    OVERLAY_EXPECTED,
)
