#!/usr/bin/env python3
"""importscripts.py [name ...]: the game's scripts (scripts/*.as, group Scripts) and UI documents
(ui/*.sml, group UI), imported through the editor's MCP, then cooked. With names, only those
(Board, Title, ...). A re-run re-imports over the same assets, keeping their ids, which the scripts
name each other's assets by (Snowline.as holds the scenes' and UI documents' ids).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

only = set(sys.argv[1:])
for folder, ext, group in (("scripts", ".as", "Scripts"), ("ui", ".sml", "UI")):
    for f in sorted(os.listdir(os.path.join(HERE, folder))):
        name, e = os.path.splitext(f)
        if e != ext or (only and name not in only):
            continue
        r = mcp("asset_import", {"source": os.path.join(HERE, folder, f), "group": group})
        print(group + "/" + name, r["guid"])
print(mcp("asset_cook", {}))
