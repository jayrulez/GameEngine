#!/usr/bin/env python3
"""graph.py: the rider's animation graph (Models/Rider/RiderGraph), written through the editor's MCP.

Board.as drives it by parameters (SceneAnimation setFloat / setBool), never by naming clips:
- Lean (float, -1..1): the carve, heel edge to toe edge; Ride's state blends CarveHeel, Ride and
  CarveToe by it, and Tuck's TuckHeel, Tuck and TuckToe (a tucked rider leans into its turn too).
- Tuck, Airborne, Grab, Crashed (bools): the rider's situation, each its own state.

States: Ride (the lean blend), Tuck, Air, Grab, Land (once, then back to Ride), Crash (held while
Crashed). Every clip but Crash starts and ends in Ride's stance (blender/rider.py), so the cross
fades stay short.
"""
import os, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import call as mcp

FLOAT, BOOL = 0, 2
EQUAL = 0
PARAMS = [("Lean", FLOAT), ("Tuck", BOOL), ("Airborne", BOOL), ("Grab", BOOL), ("Crashed", BOOL)]
P = {name: i for i, (name, _) in enumerate(PARAMS)}

assets = mcp("asset_list", {})["assets"]


def clip(name):
    return next(a["guid"] for a in assets if a["type"] == "AnimationClipAsset"
                and a.get("group") == "Models/Rider/RiderModel" and a["name"] == name)


def graph_guid():
    found = [a["guid"] for a in assets if a["type"] == "AnimationGraphAsset" and a["name"] == "RiderGraph"]
    return found[0] if found else mcp("asset_create", {"creator": "Animation Graph", "name": "RiderGraph",
                                                       "group": "Models/Rider"})["guid"]


NIL = "00000000-0000-0000-0000-000000000000"
# name, loop, clip or a blend: (param, [(threshold, clip)])
STATES = [
    ("Ride", True, ("Lean", [(-1.0, "CarveHeel"), (0.0, "Ride"), (1.0, "CarveToe")])),
    ("Tuck", True, ("Lean", [(-1.0, "TuckHeel"), (0.0, "Tuck"), (1.0, "TuckToe")])),
    ("Air", True, "Air"),
    ("Grab", True, "Grab"),
    ("Land", False, "Land"),
    ("Crash", False, "Crash"),
]
S = {name: i for i, (name, _, _) in enumerate(STATES)}


def when(param, value):
    return (P[param], EQUAL, 1.0 if value else 0.0)


# src, dst, fade (s), exit time (None = none), conditions
TRANSITIONS = [(S[s], S["Crash"], 0.12, None, [when("Crashed", True)]) for s in ("Ride", "Tuck", "Air", "Grab", "Land")]
TRANSITIONS += [
    (S["Crash"], S["Ride"], 0.25, None, [when("Crashed", False)]),
    (S["Ride"], S["Air"], 0.15, None, [when("Airborne", True)]),
    (S["Tuck"], S["Air"], 0.15, None, [when("Airborne", True)]),
    (S["Ride"], S["Tuck"], 0.2, None, [when("Tuck", True)]),
    (S["Tuck"], S["Ride"], 0.2, None, [when("Tuck", False)]),
    (S["Air"], S["Grab"], 0.15, None, [when("Grab", True)]),
    (S["Grab"], S["Air"], 0.2, None, [when("Grab", False)]),
    (S["Air"], S["Land"], 0.06, None, [when("Airborne", False)]),
    (S["Grab"], S["Land"], 0.06, None, [when("Airborne", False)]),
    (S["Land"], S["Ride"], 0.15, 0.9, []),
]


def el(parent, tag, name=None, text=None, **attrs):
    e = ET.SubElement(parent, tag, **({"name": name} if name else {}), **attrs)
    if text is not None:
        e.text = text
    return e


def arr(parent, name, items, tag):
    a = el(parent, "array", name, count=str(len(items)))
    for v in items:
        el(a, tag, text=v)
    return a


def write():
    guid = graph_guid()
    root = ET.fromstring(mcp("asset_data_read", {"guid": guid})["xml"])
    payload = root.find("object[@name='payload']")
    for child in list(payload):
        if child.get("name") != "fileName":
            payload.remove(child)
    arr(payload, "paramNames", [n for n, _ in PARAMS], "string")
    arr(payload, "paramTypes", [str(t) for _, t in PARAMS], "u8")
    arr(payload, "paramFloats", ["0" for _ in PARAMS], "f32")
    arr(payload, "paramInts", ["0" for _ in PARAMS], "i32")
    arr(payload, "paramBools", ["0" for _ in PARAMS], "u8")
    layers = el(payload, "array", "layers", count="1")
    # A layer and each state and transition are written flat in their arrays (the serializer's form).
    el(layers, "string", "name", "Base")
    el(layers, "i32", "defaultState", str(S[os.environ.get("RIDER_GRAPH_START", "Ride")]))
    el(layers, "u8", "blendMode", "0")
    el(layers, "f32", "weight", "1")
    el(layers, "array", "maskWeights", count="0")
    states = el(layers, "array", "states", count=str(len(STATES)))
    for name, loop, node in STATES:
        el(states, "string", "name", name)
        el(states, "f32", "speed", "1")
        el(states, "bool", "loop", "true" if loop else "false")
        blend = isinstance(node, tuple)
        el(states, "u8", "kind", "1" if blend else "0")
        el(states, "string", "clipRef", NIL if blend else clip(node))
        el(states, "i32", "paramIndex", str(P[node[0]]) if blend else "-1")
        el(states, "i32", "paramIndexX", "-1")
        el(states, "i32", "paramIndexY", "-1")
        arr(states, "entryThresholds", [repr(t) for t, _ in node[1]] if blend else [], "f32")
        el(states, "array", "entryPositions", count="0")
        arr(states, "entryClips", [clip(c) for _, c in node[1]] if blend else [], "string")
    # RIDER_GRAPH_START / RIDER_GRAPH_STILL: a debugging aid, a state held with no way out.
    transitions_written = [] if os.environ.get("RIDER_GRAPH_STILL") else TRANSITIONS
    transitions = el(layers, "array", "transitions", count=str(len(transitions_written)))
    for src, dst, fade, exit_time, conditions in transitions_written:
        el(transitions, "i32", "src", str(src))
        el(transitions, "i32", "dst", str(dst))
        el(transitions, "f32", "duration", repr(fade))
        el(transitions, "bool", "hasExitTime", "true" if exit_time is not None else "false")
        el(transitions, "f32", "exitTime", repr(exit_time if exit_time is not None else 1.0))
        el(transitions, "i32", "priority", "0")
        conds = el(transitions, "array", "conditions", count=str(len(conditions)))
        for param, op, threshold in conditions:
            el(conds, "i32", "paramIndex", str(param))
            el(conds, "u8", "op", str(op))
            el(conds, "f32", "threshold", repr(threshold))
    # Where the graph page draws each state (a column per situation).
    positions = el(payload, "array", "layerStatePositions", count="1")
    column = el(positions, "array", count=str(len(STATES)))
    for i in range(len(STATES)):
        xy = el(column, "object")
        el(xy, "f32", "x", repr(280.0 + 220.0 * (i % 3)))
        el(xy, "f32", "y", repr(120.0 + 160.0 * (i // 3)))
    anys = el(payload, "array", "layerAnyStatePositions", count="1")
    xy = el(anys, "object")
    el(xy, "f32", "x", "60")
    el(xy, "f32", "y", "40")
    mcp("asset_data_write", {"guid": guid, "xml": ET.tostring(root, encoding="unicode")})
    print("RiderGraph", guid)
    return guid


write()
