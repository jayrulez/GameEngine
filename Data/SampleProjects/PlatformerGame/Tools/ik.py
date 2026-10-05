#!/usr/bin/env python3
"""ik.py: the hero's inverse kinematics in every level, through the editor's MCP tools.

The Player entity (the gameplay root; the Character model is its child, the importer's prefab)
gets two components, which drive the model's animator below it:
- FootIkComponent: the feet stand on the ground under them. The Character rig's feet are IK-target
  bones off the root (detached), met by the shins; the pelvis is Body (it carries the legs).
  PlayerController turns it off in the air.
- AimIkComponent: the head (Neck 0.4, Head 1.0) looks at the nearest coin within reach; Coin.as
  picks the coin and sets the target, and turns it off when none is near.

`ik.py` writes every level; `ik.py Level2` one. A rerun rewrites the same values.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcp

LEVELS = ["Level1", "Level2", "Level3", "Level4", "Level5"]

FEET = {
    "legs": [
        {"startBone": "UpperLeg.L", "midBone": "LowerLeg.L", "endBone": "Foot.L"},
        {"startBone": "UpperLeg.R", "midBone": "LowerLeg.R", "endBone": "Foot.R"},
    ],
    "pelvisBone": "Body",
    "pelvisDropMax": 0.25,
    "liftHeight": 0.2,
}
LOOK = {
    "bones": [{"bone": "Neck", "share": 0.4}, {"bone": "Head", "share": 1.0}],
    "maxAngle": 70.0,
    "fadeSeconds": 0.35,
    "active": False,  # until a coin is near
}


def call(tool, args):
    r = mcp.http_rpc("tools/call", {"name": tool, "arguments": args})
    if "error" in r:
        raise SystemExit("%s: %s" % (tool, r["error"]))
    result = r["result"]
    text = "".join(p.get("text", "") for p in result.get("content", []))
    if result.get("isError"):
        raise SystemExit("%s: %s" % (tool, text))
    return text


def scenes():
    import json
    assets = json.loads(call("asset_list", {}))["assets"]
    return {a["name"]: a["guid"] for a in assets if a["type"] == "SceneDocument"}


def has(page, component):
    import json
    entity = json.loads(call("entity_inspect", {"page": page, "entity": "Player"}))["entity"]
    return any(c["type"] == component for c in entity["components"])


def write(level, guid):
    call("page_open", {"guid": guid})
    for component, values in (("foot_ik", FEET), ("aim_ik", LOOK)):
        if not has(guid, component):
            call("component_add", {"page": guid, "entity": "Player", "component": component})
        for prop, value in values.items():
            call("component_set", {"page": guid, "entity": "Player", "component": component,
                                   "property": prop, "value": value})
    call("action_execute", {"id": "file.save"})
    print(level, "done")


def main():
    found = scenes()
    for level in (sys.argv[1:] or LEVELS):
        write(level, found[level])


main()
