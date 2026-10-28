#!/usr/bin/env python3
"""render.py <Name>...: scripts/<Name>.as with {{Asset}} placeholders -> PaperKid's Sources/<Name>.as
with the asset ids, then script_validate over the editor's MCP."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pkgen import mcp
HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Sources")
# By name; a prefab sharing a script's name is written {{Prefab:Name}}, and an animation clip
# {{Clip:Name}} (the kid's Throw clip shares its name with the throw's sound).
PREFIX = {"PrefabDocument": "Prefab:", "AnimationClipAsset": "Clip:"}
ids, ambiguous = {}, set()
for a in mcp("asset_list", {})["assets"]:
    key = PREFIX.get(a["type"], "") + a["name"]
    if key in ids:
        ambiguous.add(key)  # several assets answer to it: a script must not name it (several models'
                            # Walk clips); pass such an asset as a behaviour property instead
    ids[key] = a["guid"]


def resolve(match):
    key = match.group(1)
    if key in ambiguous:
        raise SystemExit("{{%s}} names more than one asset; make it a behaviour property" % key)
    return ids[key]


ok = True
for name in sys.argv[1:]:
    text = open(os.path.join(HERE, "scripts", name + ".as")).read()
    # A name starts with a word character, so "{{{Title}}}" (a GUI document, wanted braced) keeps its
    # outer braces: "{<id>}".
    text = re.sub(r"\{\{(\w[^{}]*)\}\}", resolve, text)
    open(os.path.join(SOURCES, name + ".as"), "w").write(text)
    v = mcp("script_validate", {"source": text, "language": "angelscript", "name": name + ".as"})
    print(name, "valid" if v["valid"] else "INVALID", v.get("properties"), v.get("handlers"))
    for e in v.get("errors", []):
        print("   ", e)
    ok = ok and v["valid"]
sys.exit(0 if ok else 1)
