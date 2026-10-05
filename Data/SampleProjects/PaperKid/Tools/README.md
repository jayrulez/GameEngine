# PaperKid's authoring tools

PaperKid is built through the engine editor's MCP tools (open the project with `Tools.Editor
<this project> --mcp-port 7405`, the port `mcp.py` uses unless `MCP_PORT` says otherwise). These
scripts drive them; none of this is game content.

- `mcp.py`: a small MCP client (`python3 mcp.py http <tool> '<json args>'`, or `--list`).
- `pkgen.py`: scene and prefab XML built from the running editor's `component_schema`, so a
  component record starts from the engine's own defaults.
- `kit.py`: the blockout kit's prefabs (houses, road, kerb, car, pedestrian, junk, newspaper,
  delivery zone); writes `kit.json` (prefab name -> guid).
- `block.py`: the five blocks (one generator, `town()`, sized by the ring road's distance from
  the middle; the `BLOCKS` table is the difficulty ramp) and the Start scene. `block.py Block3`
  writes just that one. Bake a block's navigation after writing it (`navigation_bake`). The look
  (`ENVIRONMENT`, `POST`) is written to two shared profiles, `Profiles/PaperKid Environment` and
  `Profiles/PaperKid Post`, which every scene's settings name; `asset_cook` after changing it.
- `fx.py`: the particle effects (Confetti, Sparkle, Dust, Puff) and the soft round sprite they
  use, written through `asset_data_write` from the engine's own new effect; `kit.py` wraps each in
  an `Fx*` prefab (the effect plus `Fx.as`, which removes it once the burst has played), and the
  scripts spawn those where things happen. Run `fx.py` before `kit.py` in a fresh project.
- `scripts/*.as` + `render.py`: the game's scripts with `{{AssetName}}` placeholders;
  `render.py <Name>...` fills in the asset ids, writes `Sources/<Name>.as` and compile-checks it.
  A name several assets answer to (each model's Walk clip) is refused: such an asset is a
  behaviour property, set where the prefab or scene is built.
- `anim.py`: the property-animation clips (the marker's bob, the porch mat's pulse) and the
  throw guides' glowing unlit material, written through `asset_data_write`; `kit.py` puts them
  on the delivery zone's pieces and the AimDot and TargetRing prefabs.
- `blender/kit3d.py`: the modelling kit the Blender scripts share (bevelled boxes, tubes, spheres
  and tori with palette materials, parts rigid on bones, a two-bone IK solve, static export, a
  preview studio).
- `blender/town.py`: the houses, the cars and the street furniture (bin, hydrant, cone, newspaper),
  each written as `<Name>Model.glb` and imported as `Models/Town/<Name>Model`; `kit.py` keeps each
  prefab's colliders and behaviours and shows its model in place of the blockout's primitives.
- `blender/pedestrian.py`: the pedestrian, rigged with a Walk clip (two 0.65 m steps a second),
  imported as `Models/Town/PedestrianModel`; `Pedestrian.as` turns the figure toward where it walks
  and plays Walk at its pace.
- `blender/animals.py`: the dog and the cat, from one four-legged builder and their proportions,
  each with Walk, Idle, Sit, LieDown and its own clip (Sniff, Groom), every clip starting and ending
  in the same standing pose; imported as `Models/Town/DogModel` and `Models/Town/CatModel`. The Walk
  carries its root one stride forward a loop, and its clip asset extracts that as root motion
  (horizontal): the model's animator moves the pet (Entity mode, aimed at the Pet entity by
  `block.py` through `pkgen.root_motion_entity`). On the
  title backdrop `Pet.as` steers each about its lawn and picks what it does at each spot, and
  `Stroller.as` sends the cars and the walkers across (`block.py`'s `start()`).
- `blender/kid_bike.py`: the kid on his bike, modelled, rigged and animated in Blender (the
  Ride and Throw clips) and written as `KidBike.glb`; run it with Blender in the background
  (`blender --background --factory-startup --python blender/kid_bike.py -- <out dir> [preview]`),
  import the .glb as `Models/KidBike`, and `block.py` places its prefab on the Bike entity.
  `Bike.as` plays Ride at the bike's speed and Throw on a throw.
- `drive.py`: a closed-loop playtest of Block1 over `pie_run` (laps the ring, throws at each
  zone once).
