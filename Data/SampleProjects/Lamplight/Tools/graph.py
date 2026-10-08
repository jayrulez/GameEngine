#!/usr/bin/env python3
"""graph.py: the characters' animation graphs (Models/<Name>/<Name>Graph), written through the
editor's MCP. The scripts drive them by parameters (SceneAnimation setFloat / setBool), never by
naming clips:

- ThiefGraph (Thief.as): Speed (float, m/s) and Crouched (bool, the Sneak control held). Stand
  blends Idle, Walk and Run at the speeds their strides match (0, 1.6 and 4.5 m/s,
  blender/thief.py); Crouched blends Crouch and Sneak (0 and 1.0 m/s); the two cross-fade on
  Crouched.
- GuardGraph (Guard.as): Speed and Searching (bool, standing and looking round). Move blends Idle,
  Walk and Run (0, 1.2 and 4.0 m/s, blender/guard.py); Look plays while Searching.

Snowline's graph.py is the same writer for the rider's graph.
"""
import os, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenegen import mcp

FLOAT, BOOL = 0, 2
EQUAL = 0
NIL = "00000000-0000-0000-0000-000000000000"

# Each graph: its model, parameters, states (name, loop, a clip or a blend: (param, [(threshold,
# clip)])), its first state, and transitions (src, dst, fade s, exit time or None, [(param, value)]).
GRAPHS = {
    "Thief": dict(
        params=[("Speed", FLOAT), ("Crouched", BOOL)],
        states=[("Stand", True, ("Speed", [(0.0, "Idle"), (1.6, "Walk"), (4.5, "Run")])),
                ("Crouched", True, ("Speed", [(0.0, "Crouch"), (1.0, "Sneak")]))],
        start="Stand",
        transitions=[("Stand", "Crouched", 0.25, None, [("Crouched", True)]),
                     ("Crouched", "Stand", 0.25, None, [("Crouched", False)])]),
    "Guard": dict(
        params=[("Speed", FLOAT), ("Searching", BOOL)],
        states=[("Move", True, ("Speed", [(0.0, "Idle"), (1.2, "Walk"), (4.0, "Run")])),
                ("Look", True, "Look")],
        start="Move",
        transitions=[("Move", "Look", 0.3, None, [("Searching", True)]),
                     ("Look", "Move", 0.3, None, [("Searching", False)])]),
}

assets = mcp("asset_list", {})["assets"]


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


def write(model, g):
    group = "Models/" + model
    graph_name = model + "Graph"
    found = [a["guid"] for a in assets if a["type"] == "AnimationGraphAsset" and a["name"] == graph_name]
    guid = found[0] if found else mcp("asset_create", {"creator": "Animation Graph", "name": graph_name,
                                                       "group": group})["guid"]
    clip = lambda c: next(a["guid"] for a in assets if a["type"] == "AnimationClipAsset"
                          and a.get("group") == group and a["name"] == c)
    PARAMS = g["params"]
    P = {n: i for i, (n, _) in enumerate(PARAMS)}
    STATES = g["states"]
    S = {n: i for i, (n, _, _) in enumerate(STATES)}
    TRANSITIONS = [(S[src], S[dst], fade, exit_time, [(P[p], EQUAL, 1.0 if v else 0.0) for p, v in conds])
                   for src, dst, fade, exit_time, conds in g["transitions"]]
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
    el(layers, "i32", "defaultState", str(S[g["start"]]))
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
    transitions_written = TRANSITIONS
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
    print(graph_name, guid)
    return guid




for model, g in GRAPHS.items():
    write(model, g)
