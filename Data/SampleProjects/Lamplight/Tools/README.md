# Lamplight's tools

The scripts that build Lamplight through the editor's MCP tools. They talk to a running editor with
the project open and its MCP server on (`Tools.Editor Data/SampleProjects/Lamplight --mcp-port
7405`; `mcp.py` reads the token the editor writes). Each script's own header says what it makes,
what it needs first and how to call it. Generated files land in `generated/` (gitignored); the
imported copies live in the project's `Sources/`.

## In order

1. **Models** (Blender 5, headless; the shared modelling kit is `blender/kit3d.py`):
   - `blender/manor.py`: the manor kit on its 2 m grid (wall, doorway, corner post, floor) and the
     furniture (table, oil lamp), into `generated/Manor`.
   - `blender/thief.py`: the thief, rigged, with its clips (Idle, Walk, Sneak, Run, Crouch), into
     `generated/Thief`.
   - `importmodels.py`: imports them, the kit into `Models/Manor` and the thief into `Models`, a
     group per model.
2. **Project setup**: `controls.py` (the input map, made the project's default), `look.py` (the
   night environment and the post profile every level shares) and `materials.py` (the stand-ins'
   colours).
3. **Scripts**: `importscripts.py` imports `scripts/*.as`: the thief (`Thief`), the quarter-turn
   camera (`CameraRig`) and the wall that fades out of the camera's way (`CutawayWall`).
4. **The room**: `room.py` writes P0's room from the kit, with its colliders, the lamp, the guard
   on his route, the thief and the camera, and makes it the project's default scene.

`scenegen.py` (the scene document writer) and `mcp.py` (the MCP client) are the helpers the others
share.
