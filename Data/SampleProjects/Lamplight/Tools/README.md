# Lamplight's tools

The scripts that build Lamplight through the editor's MCP tools. They talk to a running editor with
the project open and its MCP server on (`Tools.Editor Data/SampleProjects/Lamplight --mcp-port
7405`; `mcp.py` reads the token the editor writes). Each script's own header says what it makes,
what it needs first and how to call it. Generated files land in `generated/` (gitignored); the
imported copies live in the project's `Sources/`.

## In order

1. **Models** (Blender 5, headless; the shared modelling kit is `blender/kit3d.py`):
   - `blender/manor.py`: the manor kit on its 2 m grid (wall, doorway, door, corner post, floor)
     and the furniture (table, oil lamp), into `generated/Manor`.
   - `blender/thief.py`: the thief, rigged, with its clips (Idle, Walk, Sneak, Run, Crouch), into
     `generated/Thief`; `blender/guard.py`: the guard with his lantern (Idle, Walk, Run, Look),
     into `generated/Guard`. Both stand on `blender/figure.py`'s shared rig and gaits.
   - `importmodels.py`: imports them, the kit into `Models/Manor`, the thief and the guard into
     `Models`, a group per model.
2. **Project setup**: `controls.py` (the input map, made the project's default), `look.py` (the
   night environment and the post profile every level shares), `materials.py` (the put-out
   lamp's and the pebble's), `surfaces.py` (what floors are made of, as physical
   materials) and `sounds.py` (the footsteps on each surface, generated). `graph.py` writes the
   thief's and the guard's animation graphs.
3. **Scripts**: `importscripts.py` imports `scripts/*.as` and `ui/*.sml`: the thief (`Thief`,
   `Footsteps`, `LightMeter`), the guard (`Guard`, and his `GuardMeter`), the quarter-turn camera
   (`CameraRig`), the wall that fades out of the camera's way (`CutawayWall`), lamps (`Lamp`),
   doors (`Door`), the pebble (`Pebble`), checkpoints (`Checkpoint`) and the HUD. Then
   `prefabs.py` writes the prefabs that use them (the pebble).
4. **The room**: `room.py` writes the room from the kit, with its colliders and their surfaces,
   the lamp, the door, the guard on his round, a checkpoint in the yard, the thief and the camera,
   bakes the navigation, and makes it the project's default scene.

`scenegen.py` (the scene document writer) and `mcp.py` (the MCP client) are the helpers the others
share.
