#!/usr/bin/env python3
"""controls.py: Lamplight's input map (Controls), written through the editor's MCP, and made the
project's default.

- Move: WASD or the left stick, relative to the camera.
- Sneak: C, or the east button (B): slow and silent. Run: Left Shift, or pressing the left stick:
  fast and loud.
- Interact: F, or the south button (A): pick a lock, open a door, put out a lamp.
- Turn the camera a quarter left or right: Q / E, or the left and right shoulders.
- Pause: Escape, or Start.
Codes are the engine's enums' values (shell::KeyCode, shell::GamepadButton, input::StickCode).
"""
import os, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import call as mcp

KEY, GAMEPAD_BUTTON, GAMEPAD_STICK, COMPOSITE = 0, 4, 6, 7
BUTTON, AXIS2D = 0, 2
A, C, D, E, F, Q, S, W = 1, 3, 4, 5, 6, 17, 19, 23
ESCAPE, LEFT_SHIFT = 62, 109
SOUTH, EAST, LEFT_SHOULDER, RIGHT_SHOULDER, LEFT_STICK_PRESS, START = 0, 1, 4, 5, 6, 14
LEFT_STICK = 0

ACTIONS = [
    ("Move", AXIS2D, [dict(source=COMPOSITE, negX=A, posX=D, negY=S, posY=W),
                      dict(source=GAMEPAD_STICK, code=LEFT_STICK, invert=True)]),
    ("Sneak", BUTTON, [dict(source=KEY, code=C), dict(source=GAMEPAD_BUTTON, code=EAST)]),
    ("Run", BUTTON, [dict(source=KEY, code=LEFT_SHIFT), dict(source=GAMEPAD_BUTTON, code=LEFT_STICK_PRESS)]),
    ("Interact", BUTTON, [dict(source=KEY, code=F), dict(source=GAMEPAD_BUTTON, code=SOUTH)]),
    ("TurnLeft", BUTTON, [dict(source=KEY, code=Q), dict(source=GAMEPAD_BUTTON, code=LEFT_SHOULDER)]),
    ("TurnRight", BUTTON, [dict(source=KEY, code=E), dict(source=GAMEPAD_BUTTON, code=RIGHT_SHOULDER)]),
    ("Pause", BUTTON, [dict(source=KEY, code=ESCAPE), dict(source=GAMEPAD_BUTTON, code=START)]),
]


def el(parent, tag, name=None, text=None, **attrs):
    e = ET.SubElement(parent, tag, **({"name": name} if name else {}), **attrs)
    if text is not None:
        e.text = text
    return e


def binding(parent, b):
    """One binding, every field (arrays of structs are written flat)."""
    el(parent, "u8", "source", str(b["source"]))
    el(parent, "u32", "code", str(b.get("code", 0)))
    el(parent, "u32", "modifiers", "0")
    el(parent, "i32", "device", "-1")
    el(parent, "f32", "deadZone", "0.15")
    el(parent, "f32", "scale", "1")
    el(parent, "bool", "invert", "true" if b.get("invert") else "false")
    el(parent, "bool", "normalize", "true")
    for k in ("negX", "posX", "negY", "posY"):
        el(parent, "u32", k, str(b.get(k, 0)))
    for k, v in (("regionX", "0"), ("regionY", "0"), ("regionW", "1"), ("regionH", "1"), ("stickRadius", "0.15")):
        el(parent, "f32", k, v)


def main():
    assets = mcp("asset_list", {})["assets"]
    found = [a["guid"] for a in assets if a["type"] == "InputMapAsset" and a["name"] == "Controls"]
    guid = found[0] if found else mcp("asset_create", {"creator": "Input Map", "name": "Controls"})["guid"]
    root = ET.fromstring(mcp("asset_data_read", {"guid": guid})["xml"])
    payload = root.find("object[@name='payload']")
    for child in list(payload):
        if child.get("name") == "sets":
            payload.remove(child)
    sets = el(payload, "array", "sets", count="1")
    el(sets, "string", "name", "Gameplay")
    el(sets, "i32", "priority", "0")
    actions = el(sets, "array", "actions", count=str(len(ACTIONS)))
    for name, kind, bindings in ACTIONS:
        el(actions, "string", "name", name)
        el(actions, "u8", "kind", str(kind))
        for k, v in (("sensitivity", "0"), ("gravity", "0")):
            el(actions, "f32", k, v)
        el(actions, "bool", "snap", "false")
        el(actions, "f32", "responseExponent", "1")
        el(actions, "bool", "timeScale", "false")
        el(actions, "u8", "interaction", "0")
        el(actions, "f32", "interactionSeconds", "0.3")
        b = el(actions, "array", "bindings", count=str(len(bindings)))
        for each in bindings:
            binding(b, each)
    mcp("asset_data_write", {"guid": guid, "xml": ET.tostring(root, encoding="unicode")})
    mcp("project_settings_set", {"defaultInputMapId": guid})
    print("Controls", guid)


main()
