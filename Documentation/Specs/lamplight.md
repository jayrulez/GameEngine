# Lamplight - a night-time heist in a manor, the fourth demo game

> STATUS: PLAN 2026-10-06, agreed in outline by the user, who named it Lamplight. Not started.
>
> Picked for what it proves about the engine (user, 2026-10-06), after a survey of what no game
> exercises yet: local lights (point and spot, with their shadows), reflection probes, SSR, SSGI,
> auto exposure, reverb zones, world-space UI and two-bone IK. Sky Hopper, PaperKid and Snowline
> are lit by a sun alone and play outdoors; this one is lit by lamps, indoors and at night, and
> the lights are the game. Rulings (user, 2026-10-06): a stealth heist, pure stealth (no
> knockouts); a higher camera the player turns in 90-degree steps; about five levels with
> checkpoints inside a level; scripts in AngelScript, so the Sedulous session can port it (a game
> in Luau, exclusive to this engine, comes after); the masquerade crowd (instanced skinning) saved
> for a later game; TAA on (SSGI needs it). Read CONVENTIONS.md first.
>
> The render checks it needed are done (2026-10-06): auto exposure starts at a scene's level
> (546043f8), SSR reaches (6c2b4cfa), SSGI tints its bounce by the albedo and replaces the sky it
> hides (02ed2d57), TAA holds under a moving camera and keeps each view's history (59dec5d9).

## Where it lives and how it is built

- **Project**: `Data/SampleProjects/Lamplight` (its `Cooked/`, `.cache/`, `Editor/` and `Dist/`
  generated and ignored, as the other samples'). Authoring scripts in its `Tools/`, as
  Snowline's: an MCP client, scene and prefab generators, and Blender scripts on the shared kit
  (`kit3d.py`, copied in).
- **Through the tools, not by hand.** Assets by `asset_create` / `asset_import` /
  `asset_data_write`, scenes by `scene_write`, components by `component_set`, scripts by
  `script_create` / `script_validate`, playtests by `pie_run`. A tool that is missing or wrong is
  a finding: fix it in the engine with a test, on its own commit, then carry on.
- **Levels from a generator.** A level is a table of rooms, walls, doors, lamps, guard routes and
  loot in `Tools/level.py`, which writes the scene: the manor is built from a modular kit (wall,
  doorway, window, floor and stair pieces on a grid), so a level is data and its colliders, nav
  zones, reverb zones and probes are placed from the same table that places the walls.

## Concept

A thief breaks into a manor over one night to take one thing, from the gardens to the vault.
Guards walk their rounds with lanterns; a lantern's beam is what a guard sees. Darkness is cover:
the player reads the light around them, puts out lamps to make shadows, throws a pebble to draw a
guard away, and picks locks out of sight. Being seen is not death but a chase; being caught sends
the thief back to the last checkpoint.

## Core loop (one level)

1. Enter at the level's start; the HUD shows the target, the loot taken, and a light meter (how
   lit the thief is).
2. Move: sneak (slow, silent), walk, or run (fast, loud). The camera looks down from behind and
   above; the player turns it a quarter at a time.
3. Guards patrol set routes, each with a lantern (a spot light). Inside a guard's beam and in his
   line of sight, a visibility meter over his head fills, faster the brighter the thief is lit and
   the closer he is: "?" (suspicious: he turns and looks), then "!" (seen: he gives chase).
4. Make cover: put out a lamp or a candle (it stays out; a guard who passes it may relight it),
   close a door, keep to the walls. Throw a pebble: a guard who hears it goes to look.
5. Doors and locks: hold a button to pick a lock (the hand on the lock, a few seconds, noise if
   rushed); some doors are only locked from one side.
6. Loot along the way (coins, silver, paintings) scores; the target ends the level once carried
   to the exit.
7. Caught (a guard reaches a thief he has seen): back to the last checkpoint, the guards back on
   their rounds. A level's results: time, loot, times seen, and a "ghost" medal for never seen.

## Levels and progression

| Level | Teaches | Features |
|---|---|---|
| 1 Gardens | light and shadow, sneaking | moonlit hedges, lanterns on posts, one guard |
| 2 Stable yard | noise, the pebble | rain on wet cobbles (SSR, particles), gravel louder than grass |
| 3 Ground floor | doors, locks, putting out lamps | candlelit halls, polished floors (SSR, probes), echoing rooms |
| 4 Cellars | darkness both ways | torchlit, nearly dark; bright to dark and back (auto exposure); heavy reverb |
| 5 Upper floor and vault | everything | the vault, the target, an alarm on the way out |

A level unlocks the next. Best times, loot and medals are saved; the title shows them. Ending: a
short summary after the vault.

## The engine it exercises

| Feature | How the game uses it | Today (2026-10-06) |
|---|---|---|
| Point and spot lights, local shadows | lamps, candles, torches, the guards' lanterns | `LightComponent::of(e)` reads and writes `enabled`, `intensity`, `color`, `range`, the cone angles, `castsShadows`, `shadowUpdate`; a spot casts into one shadow tile, a point into six; 16 tiles a frame for realtime lights and 16 for static ones; past the budget a light is unshadowed silently, in extraction order |
| Reflection probes | polished floors and silver in the halls | the component and its capture exist; not yet used by a game |
| SSR, SSGI, TAA, auto exposure | wet cobbles, lamplight bouncing off walls, moving between dark and bright | checked and fixed 2026-10-06 (above); SSGI needs TAA |
| Reverb zones (`audio.ReverbZone`) | each room its own echo | automatic: the wettest zone the listener is in wins; no script handle needed |
| Navigation | guard patrols, investigating a noise, the chase | `NavAgentComponent::of(e)`: `navigate`, `navigateAt`, `finished`, `remaining`; a patrol is the next point on `finished()` (PaperKid's `Vehicle.as`) |
| Physics raycast | a guard's line of sight; what stands between the camera and the thief | `ScenePhysics::of(scene).rayCast` reports the hit's entity, position and normal; `sphereCast`, `overlapSphere` |
| Scene events | noise: a pebble, a run, a door | `scene.events.emit(name, payload)` reaches every behaviour with `on<Name>`; the position rides the payload and each guard checks its own distance |
| Two-bone IK | the hand on a lock or a handle | `SceneAnimation::of(scene).setIkTarget`, `TwoBoneIkComponent::of(e).weight` |
| World-space UI | "?" and "!" over a guard, prompts on doors | `UIWorldPanelComponent` / `UIBillboardComponent` `visible` and `document` from script; not the widgets inside them |
| Physics joints | doors on hinges | as in Snowline; a door the thief opens is driven by script |
| Particles, Save, screen UI, audio | rain, smoke from a snuffed candle, results, saves, footsteps | as in the other games |
| Blender kit | the thief, guards, manor kit pieces, lamps, loot | `kit3d.py` |

## Engine gaps known before starting

Each is fixed in the engine with a test, on its own commit, when the phase that needs it reaches
it; the plan is checked against the code, not guessed.

1. **How lit is a point.** The light meter and a guard's eye both need "how much light reaches
   this position", and the script API has none (no light list, no shadow query). A script could
   find every lamp by name and raycast to each, but the game would carry a copy of the
   renderer's falloff. Fixed in P1 as `SceneRender::of(scene).lightAt(position)` (and with a
   collision-group mask): every enabled light by the renderer's own range falloff and spot cone
   (`LightFalloff`, the CPU twin of the forward shader's), a light that casts shadows stopped by
   what stands between (a ray through the scene's solid surfaces, `ISceneRayQuery`, so render
   does not depend on physics), by its shadow strength, plus the ambient. A light without
   shadows shines through walls, as it does on screen. A CPU estimate of the shading, not a GPU
   readback; the sky's image-based light is not in it.
2. **Seeing the thief indoors from a higher camera.** Walls and upper floors between the camera
   and the thief must get out of the way, and the engine has no fade or cutaway: a script can
   hide a mesh (`MeshComponent.visible`, a hard pop) or swap its material, nothing smoother.
   Decided at P0, after trying both: a per-mesh fade (`MeshComponent.fade`, 0 solid to 1 gone)
   the renderer draws as a screen-door dither, eased in and out by the game's `CutawayWall`
   behaviour (a wall fades while the camera is outside it and the thief inside). A faded mesh
   leaves the depth prepass, so nothing behind it is hidden by depth it no longer covers, and it
   keeps casting its whole shadow, so the room behind a cut-away wall stays as dark as it was;
   the hard hide by `visible` dropped the shadow with the wall.
3. **A meter over a guard's head.** Showing or hiding "?" and "!" works through a world panel's
   `visible`; filling a meter inside it does not, since the screen `ui` facade cannot reach a
   world panel's widgets ("there is no scene.ui script root", `UiScriptFacade.cppm`). Decided
   at P2: script access to a world panel's views (find by name, set text and fill), or two
   panels and a scaled bar entity as a stand-in.
4. **The local shadow budget.** Sixteen realtime tiles a frame (a point light takes six), with no
   priority past the budget: a manor of candles and torches outruns it. Fixed lamps use static
   shadows (their own sixteen), the guards' lanterns are spots (one tile each), and P0 measures
   the rest; if a level still overflows, the finding is a priority by distance to the camera.
5. **A door to swing open.** A door is a body on a hinge joint, and the script has no joint API
   (Snowline's flags only reacted to contact). Opening it by script (a kinematic door rotated by
   its behaviour) needs nothing new; a door that swings under a push is a finding if wanted.

## Phases

- **P0 A lit room.** One room from the modular kit with a lamp (point light, shadows),
  a guard's lantern (spot light, shadows) walking a route, the thief on a stand-in character, the
  quarter-turn camera with its cutaway, SSR on a polished floor, SSGI and TAA on. The exports to
  the web and the Deck, with frame times and pack sizes, wait for the finished game.
- **P1 Sneaking.** The thief (Blender model, rigged, sneak / walk / run / crouch clips on a graph),
  the light meter, noise from footsteps by surface, doors, lockpicking with the hand on the lock
  (two-bone IK), putting out and relighting lamps.
- **P2 Guards.** Patrol routes on the navmesh, the lantern cone and line of sight, the visibility
  meter over the head (world-space UI), suspicion, search, chase, catch, the pebble and hearing,
  checkpoints.
- **P3 Gardens and the stable yard, complete.** Levels 1 and 2 from the generator, the HUD,
  results, saves, rain.
- **P4 Inside the manor.** Levels 3 to 5: reflection probes, reverb zones per room, the cellars'
  darkness, the vault and its alarm, the ending.
- **P5 Ship.** Audio (licences checked, kept beside the assets, credited), the README with
  screenshots, `Integration.Mcp` expectations, web and Deck exports, the demos site.
