# Sky Hopper

A small 3D platformer, built entirely through the engine's MCP tools: the test of how far an
agent gets making a game with them. It was played through to the end over the same tools before
it shipped to a Steam Deck through the container build (`scripts/build-steamdeck.sh`).

![Sky Hopper, Level 1](../../../Documentation/Images/SkyHopper-Play.png)

## The game

Five levels of floating islands (Grassy Hills, Crab Crossing, Sky Climb, Bee Meadow, Cloud
Fortress): hop between platforms, collect coins, stomp crabs, skulls and bees from above, cross
bridges, dodge spikes, swinging saws and spiky balls, and reach the flag.

A run has three lives: a fall or a hit costs one, a heart found in a level gives one back, and
so does every 50th coin; the last one lost is the game over, and trying again starts from the
first level. Coins, stomps and each level's hidden gem score points, and a clear adds a bonus
for every second under the level's par time and for a level without a fall. Each clear earns up
to three stars (all the coins, no falls, under par), counted up on the clear card; the best
score, stars and time of every level, and the best run, are saved and shown on the title.

A title screen with settings (master, music and effect volumes), an intro banner per level, a
pause menu, a HUD of icons (lives, coins, the gem, the clock and the score, with gains floating
up from it), the clear card, a game over card and a victory screen with the run's totals.
Falling respawns you on the last safe ground.

Controls: WASD to move, Space to jump, Escape to pause; the arrows and Enter drive the menus. A
gamepad works throughout: the left stick to move (and the stick or d-pad in menus), A (cross)
to jump and confirm, Start (Options) to pause.

## Project layout

- `Project.xml`: the manifest: the start scene, the Game script, the input map, the UI theme
  and fonts, and the render resolution (1280x720, letterboxed).
- `Sources/`: the raw sources: the AngelScript scripts, the UI markup and theme, the fonts, the
  audio, and the kit's glTF models.
- `Content/`: the asset envelopes (`*.xasset`) and their sidecars.
- `export_presets.xml`: the Linux desktop and Steam Deck export targets.
- `Tools/`: the authoring scripts that drive the editor's MCP tools (levels 4 and 5 among them);
  not game content.
- `CREDITS.md` and `Licenses/`: the third-party assets and their licences. The music by CodeManu
  is CC-BY 3.0 and must stay credited; everything else is CC0, OFL or Apache 2.0.
- `Cooked/`, `.cache/`, `Editor/`, `Dist/`: generated, and gitignored; the tools rebuild them.

Open the project from the editor's project manager.
