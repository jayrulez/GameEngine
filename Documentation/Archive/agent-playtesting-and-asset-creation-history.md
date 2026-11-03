# Agent playtesting and asset creation - creators at the pipeline level, PIE for agents

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: PROPOSED 2026-09-29. Sized L. LED BY SEDULOUS (user, 2026-09-29): built there first,
> phase by phase, and ported here from its commits, as editor-lists-and-asset-slots.md was.
> The citations are SEDULOUS's, checked at `eeaa5376` (branch `polish`); the Raptor notes say
> where each piece sits in this tree. Read CONVENTIONS.md first. Builds on
> scene-authoring-tools.md, which covers structured scene edits and is not repeated here.

## Goal

An agent connected over MCP can make a small game end to end and check that it plays: create
every asset kind the editor can create, run the game in PIE, drive it with scripted input,
read what happened, and look at it. The motivating case is a platformer built from a bought
asset pack: the art comes in through `asset_import`, and everything else (scenes, prefabs,
materials, the input map, sound cues, the bus layout, UI) is created and authored by the agent,
then played.

Not goals:

- Shader creation. There is no shader editing story, and no game here needs a custom shader
  (user, 2026-09-29). Shader stays the one engine asset with neither a creator nor a
  registered importer (see the table).
- Authoring art. Meshes, textures, sprites and sounds come from import.
- Playtesting in Simulate. Simulate previews ONE scene in isolation; it runs no game instance,
  no startup script, no default input map. Gameplay is PIE's (the Game tab), and every play
  tool here drives PIE (user, 2026-09-29).
- Structured scene edits (create entity, add component, set a field): scene-authoring-tools.md.

## The problem as it stands

### Creators: complete, but only the editor executable has them

Every engine asset that does not come from an import has a New Asset creator, except Shader.
The engine's asset types are the 27 cook builders (`PipelineRegistration.bf:151-181`,
`cBuilderCount = 27` at :68) plus scenes and prefabs:

| Asset | Creator | Importer |
|---|---|---|
| Scene, Prefab | Scene, Prefab | - |
| Static Mesh | Cube, Sphere, Plane, Cylinder, Cone, Torus | Model |
| Skinned Mesh, Skeleton, Animation Clip, Model Manifest | - | Model |
| Material | PBR Material, Unlit Material | Model (embedded) |
| Texture | - | Texture; Model (embedded) |
| Image, Font, Audio Clip | - | Image, Font, Audio |
| Script Class | one per language and tier (Behavior, Level, Game) | Script |
| UI Document, UI Theme | UI Document, UI Theme | UI |
| Collision Shape | Collision Shape | Model (Generate collision) |
| Heightfield, Splatmap, Vegetation Mask | Heightfield, Splatmap, Vegetation Mask | the matching file importers |
| Terrain | Terrain | - |
| Animation Graph, Property Animation Clip | Animation Graph, Property Animation Clip | - |
| Particle Effect | Particle Effect | - |
| Input Map | Input Map | - |
| Physical Material, Navigation Zone | Physical Material, Navigation Zone | - |
| Audio Bus Layout, Sound Cue | Audio Bus Layout, Sound Cue | - |
| Shader | - | - (`ShaderImporter` is a static helper called only by the cook tests) |

Where they live:

- `AssetCreator` (`Editor.Core/src/AssetCreator.bf`) runs `delegate Instance(EditorContext,
  Group)`; `EditorContext.RegisterCreator` (`EditorContext.bf:363`) keeps them.
- Eighteen are registered by the EDITOR EXECUTABLE: `EditorCreators.RegisterAll`
  (`Tools/Sedulous.Tools.Editor/src/EditorCreators.bf:24-63`, the twelve data assets) and
  `EditorSeed.RegisterPrimitiveMeshCreators` (`EditorSeed.bf:66-71`, the six primitives), called
  from `EditorRegistration.bf:86-87`.
- The rest by editor modules: Scene and Prefab (`Editor.Scene/src/SceneEditor.bf:42-43`),
  the materials, particle effect and animation graph (Editor.Scene), the property animation clip
  (`Editor.PropertyAnimation`), the scripts (`Editor.Script/src/ScriptEditor.bf:45`).

None of them needs the editor. Each reads `context.Project` for the source database's root group
and the sources root, writes an instance, and some call `context.RequestCook(false)`. The
editor's one caller, `CreateAndOpen` (`EditorApplicationProject.bf:690-715`), already owns every
effect around a creation: it defers while a cook holds the databases, sets the default scene for
a creator marked `SetsDefaultScene`, rebuilds the browser, and requests a cook when a builder
handles the new type. The creators' own `RequestCook` calls repeat that.

So no MCP tool can create an asset. The headless host (`Tools/Sedulous.Tools.Mcp`) cannot reach
creators that the editor executable registers, and the editor host exposes none. `scene_write`
and `prefab_write` create scenes and prefabs from full XML; nothing creates a material, an input
map or a sound cue.

### PIE: startable, then invisible to an agent

- `game.play` and `game.playNewInstance` are registered actions (`EditorApplicationMenus.bf:151,
  157`), so `action_execute` can start PIE. Nothing stops it, reports its state, or reaches the
  running game.
- PIE is not one game. Play New Instance opens another Game tab with a game instance of its
  own; the editor names the tabs `game-page` (the primary) and `game-page-N`
  (`EditorApplication.bf:489-493`). An action runs over one page, so an agent cannot say which
  instance it means.
- The game reads input through `GameViewportInputSource` (`Editor.Scene/src/Game/
  GameViewportInputSource.bf`), an `IInputSourceProvider` over the Game tab's viewport,
  installed by `BindInput` (`GameEditorPageRun.bf:177`) on the input subsystem and the game
  instance. There is no way to feed it anything but the real devices.
- `entity_inspect` reads a scene page's scene. The running game's scene belongs to its
  `GameInstance` (`GameEditorPage.bf:38`); no tool reads it.
- `viewport_screenshot` captures a scene page's viewport through `ViewportCapture`
  (`Editor.Scene/src/Page/ISceneEditorPage.bf:35`). The Game tab renders into its own
  `ViewportView` (`GameEditorPage.bf:63`), which nothing captures.

## Design

### D1. The creator moves to the pipeline core, and stops knowing the editor

`AssetCreator` moves to `Sedulous.Pipeline.Core`, beside `BuilderRegistry`, with an
`AssetCreatorRegistry` holding them:

- `Label`, `Category` (unchanged), and the created type's full name (new: a tool selects by type
  as well as by label, and a host decides from it whether to cook).
- `Run(in AssetCreationContext context) -> Instance`, the context carrying the target `Group`
  (the root when none was picked) and the sources root path. No `EditorContext`.
- `SetsDefaultScene` stays as data. Setting the default scene, deferring for a cook, requesting
  the cook and opening a page are the HOST's, as `CreateAndOpen` already does them. The
  creators' own `RequestCook` calls go.

### D2. Each domain contributes; the registration composes

A creator lives with the pipeline domain that owns its asset type, as the builders do. Each domain
exposes a `Register(AssetCreatorRegistry)`, and `PipelineRegistration.RegisterAllCreators`
calls them, with a `cCreatorCount` tripwire beside `cBuilderCount` and `cImporterCount`.

| Creators | Home |
|---|---|
| Input Map | Input.Pipeline |
| Physical Material, Collision Shape | Physics.Pipeline |
| Navigation Zone | Navigation.Pipeline |
| Audio Bus Layout, Sound Cue | Audio.Pipeline |
| UI Document, UI Theme | UI.Pipeline |
| Heightfield | Heightfield.Pipeline |
| Terrain, Splatmap | Terrain.Pipeline |
| Vegetation Mask | Vegetation.Pipeline |
| PBR and Unlit Material | Materials.Pipeline |
| Particle Effect | Particles.Pipeline |
| Animation Graph | Animation.Pipeline |
| Property Animation Clip | PropertyAnimation.Pipeline |
| the primitives | Geometry.Pipeline (`Primitives` is in Sedulous.Geometry) |
| the scripts | Script.Pipeline (`ScriptLanguageCooks` is already there) |
| Scene, Prefab | Scene.Pipeline, new (see below) |

Scene and Prefab have no pipeline project today; their document types live in Scene.Resource.
They get one, `Sedulous.Scene.Pipeline`, the scene domain's like every other domain's, holding
their creators. A new scene is seeded with a directional sun, which needs a live scene with the
render engine's `LightComponentManager`, and a new prefab is captured from a seed scene, so the
library links Scene, Scene.Resource and Engine.Render. That is the right direction: the pipeline
layer sits above the engine (Pipeline.ScriptSurface already links Engine.Composition), and the
engine never links a pipeline library, so the creators cannot live in Engine.Composition:
registering them needs Pipeline.Core, a layer above it. Registration's closure does not grow: it already
reaches Engine.Render through ScriptSurface. Seeding needs no device, only the component manager
in a plain scene (the inspector tests add `LightComponentManager` headless), so the headless host
creates a lit scene too; it links the graphics libraries transitively already and creates no
device.

Rejected: a scene creator that seeds nothing (every new scene would start unlit, a user-visible
loss for a layering reason), and a general pipeline render library (no domain of its own).

The editor keeps its New menus exactly as they are, built from the one registry. The creator
calls in `EditorRegistration.bf:86-87` and in the editor modules become the two registration
calls.

### D3. `asset_creators` and `asset_create`, in both hosts

- `asset_creators` (read only): every creator's label, category and type.
- `asset_create`: a creator by label or by type (a type with one creator), a `name`, and a group
  path (created when missing, through `McpTools.ResolveGroupPath` as `scene_write` does). It answers the new asset's guid, name and
  path. A taken name is refused, not suffixed: an agent that names an asset means that name.
- The headless host creates in the source database (files are truth) and does not cook; the
  agent calls `asset_cook` next, as after `asset_import` ("Does not cook - call asset_cook
  next"). The editor host goes through `CreateAndOpen`'s effects
  without opening a page: the cook gate, the default scene, the browser rebuild, the cook
  request.

McpGuide gains the two tools, and says to create an asset, then edit it (`component_set` and the
scene edits for scenes, the asset's page tools where it has them).

### D4. PIE control: `pie_start`, `pie_stop`, `pie_state`, `pie_list`

Editor host only; PIE is the editor's. Every PIE tool addresses ONE instance by its id, the Game
tab's (`game-page`, then `game-page-1`, `game-page-2`, ... for Play New Instance's), and defaults to the primary. Several instances are
the point for anything networked: a host and a client, each driven and probed on its own.

- `pie_start` runs `game.play` (the primary) or, with `newInstance`, `game.playNewInstance`,
  and answers once that instance's first frame has rendered: its id and its start scene.
- `pie_stop` stops one instance; `all` stops every one.
- `pie_state` answers for one instance whether it is running, the current scene, the game time
  since start, the frame count, and the startup script's state (running, faulted with its error).
- `pie_list` answers every open instance with its state, so an agent finds what the user started
  as well as what it did.

### D5. Scripted input: a provider the run can swap in

`ScriptedInputSource : IInputSourceProvider` holds a virtual keyboard, mouse and gamepads, and an
event stream, driven from a timeline. While a script plays, the addressed Game page's
`BindInput` path installs it in place of that page's `GameViewportInputSource`, on the input
subsystem and the game instance both, and puts the viewport source back when the script ends or
the run stops. The other instances keep their own sources: scripting a client leaves the host
alone. The real devices
are not merged in: a playtest must not depend on where the user's mouse is.

The timeline is device level, keys, buttons and axes, not input map actions, so a playtest
exercises the project's input map as a player would: `{at: 0.0, key: "D", down: true}`,
`{at: 0.4, key: "Space", down: true}`, `{at: 0.5, key: "Space", down: false}`,
`{at: 0.2, gamepad: 0, axis: "LeftX", value: 1.0}`. Times are GAME time since the script starts.

### D6. The playtest primitive: `pie_run`

One call runs a timeline on one instance and reports what happened, the loop an agent tunes
against:

- `input`: a D5 timeline; `duration`: game seconds to run.
- `probes`: entities by name or guid, with the fields to read (the shapes `entity_inspect` uses),
  sampled `every` N game seconds or `at` listed times; the answer is one row per sample.
- `screenshots`: game times at which to write a PIE frame (D7); the answer lists the files.
- `until`: an optional stop condition, a probe field crossing a value ("player y below -10",
  "score at least 3"), so a death or a win ends the run early and says so.

The run starts from the scene PIE is in. `pie_start` then `pie_run` from a fresh start is the
reproducible form. Runs are real frames at real frame rates, so timings vary by a frame. Physics
steps at its fixed rate, which keeps motion comparable. Expected values want tolerances, and the
guide says so.

Several instances can run at once, each with its own `pie_run`; the calls do not wait on each
other, so a host and a client can be scripted over the same seconds.

`entity_inspect` gains a `pie` target, an instance id, reading that game's scene, for a look
outside a run.

### D7. `pie_screenshot`

What one instance's Game tab renders, as a PNG, the same contract as `viewport_screenshot`: it
brings that Game page to front, waits for the next rendered frame and the GPU, writes the file, and answers
the path and size. It captures the Game tab's `ViewportView` through the same capture the scene
page uses (`ViewportCapture`), hoisted where both pages reach it. `pie_run`'s screenshots are
this capture at the requested game times.

## Phases

- **P1: creators at the pipeline level** (Pipeline.Core, the domain pipelines, a new
  Scene.Pipeline, Registration, the editor; M). D1 and D2; the editor's menus unchanged. Tests: the
  registry holds every creator the table lists and `cCreatorCount` matches; each creator makes
  an instance of its type in a scratch source database with no editor; the editor's New menu
  lists what it listed before.
  As built in Sedulous (P1):
  - `Pipeline.Core`: `AssetCreator` (label, category, created type's full name,
    `SetsDefaultScene`, `Run(AssetCreationContext)`; `CreateWritten` for the common "a uniquely
    named instance holding this asset" shape), `AssetCreationContext` (`Picked`, `Root`,
    `SourcesRoot`; `Target` is the picked group else the root, `TargetOr(name)` the picked group
    else that folder under the root, made when missing), `AssetCreatorRegistry` (`FindByLabel`,
    case-insensitive; `FindByType`, null when a type has several, as materials do).
  - One `<Domain>Creators.Register` per pipeline domain, composed by
    `PipelineRegistration.RegisterAllCreators` in the old menu order; `cCreatorCount = 25`
    (twelve data assets, six primitives, seven from the editor modules) plus
    `ScriptCreators.CountFor(languages)`, three per script language with a cook.
  - Two seeds moved down with their creators: the particle one (`ParticleCreators.
    SeedDefaultEffect`) and the animation graph one (`AnimationCreators.SeedDefaultGraph`). The
    graph seed builds the page's edit model, so that model moved to Animation.Pipeline with it:
    `GraphDocument`, `GraphLayer`, `GraphState`, `GraphParam`, `GraphTransition`,
    `GraphCondition` (pure data over `AnimationGraphSource`). Raptor moves its equivalents the
    same way.
  - The editor: `EditorContext.Creators` is the registry, filled by one `RegisterAllCreators`
    call where `EditorRegistration` made its two; the module registrations, the editor
    creator files and Editor.Core's `AssetCreator` are gone. `CreateAndOpen` builds the
    context from the open project (refused without one) and keeps every effect around a
    creation. The new-project seed and the animation panel's Create Clip call the pipeline's
    `GeometryCreators.CreatePrimitive` and `PropertyAnimationCreators.CreateClip`.
  - Tested: every creator registers (the count) and makes its own type in a plain source
    database with no editor; a picked group wins; a file-backed creator refuses without a
    sources folder; `FindByType`. The editor's creator tests (scene, material, particle,
    graph) run the pipeline creators over a real project. The menus are unchanged by
    construction: the same creators in the same order, which the count and the ordered
    registration keep; no headless test builds the menu.
- **P2: asset tools** (both hosts; S). D3. Tests: `asset_creators` lists them; `asset_create` in
  the headless host makes an input map and a sound cue that cook; a taken name is refused; the
  editor host's create requests the cook and sets the default scene for a first scene.
  As built in Sedulous (P2):
  - The folder a creator lands in is DATA now: `AssetCreator.DefaultGroup`, set with
    `.Under("Materials")` at registration, carried into the context, and `TargetFor(picked,
    root)` says where an instance would land without creating the folder. The context gains
    `Name` (`NameOr(fallback)`): a named creation uses the name exactly, an unnamed one keeps
    the creator's unique default.
  - `IProjectOperations.Create(CreateRequest, CreateOutcome, error)`; the shared
    `AssetCreation.Run` resolves the group (created when missing), refuses a name already
    taken in the target ("an asset named '…' already exists in '…'"), and runs the creator.
    The inline host finishes at once. The editor host answers NotYet while the cook holds
    the databases (timing out as the other operations do), then runs it and calls the
    `OnCreated` seam, which is the same `AfterCreate` File > New runs: the default scene for
    a first scene, the assets view rebuild, the cook request.
  - `asset_creators` (read-only: label, category, type, defaultGroup) and `asset_create`
    (`creator` by label or `type` when one creator makes it; `name`; `group`) in
    `EngineTools`, `cEngineToolCount = 24`. The headless host fills its own registry with
    `RegisterAllCreators`; the editor host passes `EditorContext.Creators`.
  - Tested: headless, an input map named into a group, a sound cue by type and a PBR
    material by label, all three cook; a taken name, an unknown label and an ambiguous type
    are refused. The editor host's operations hold creation while the cook is locked, then
    create and call `OnCreated`, and refuse a taken name. `AfterCreate`'s effects themselves
    are File > New's, unchanged, and not driven by a headless test.
- **P3: PIE control and capture** (editor host; M). D4 and D7. Tests: start, state, stop; a
  second instance gets its own id, and stopping it leaves the first running; `pie_list` lists
  both; a capture writes a PNG of the addressed tab's size; a faulted startup script shows in
  the state.
  As built in Sedulous (P3):
  - `IPieInstancePage` (Editor.Scene) is what the tools act through, as `ISceneEditorPage` is
    for the scene tools; the Game page implements it and tests fake it. The id is the page's:
    `GamePageFactory(newInstance, pieId)` hands it in, the same string the dock persists the
    tab under (`game-page`, `game-page-1`, ...; the counter never reuses a number).
  - `PieMcpTools` (five tools, `cPieToolCount`), a second contribution beside the scene
    tools. `pie_start` runs the editor's own `game.play` / `game.playNewInstance` (the action's
    project gate refuses without a project), finds the tab that opened, calls its Play and is
    re-entered each pump until it runs and has rendered a frame (up to five minutes, the cook
    included); a running primary answers `alreadyRunning`, a start that ends neither running
    nor starting is an error. `pie_stop` leaves the tab open.
  - Game time is the run's gameplay clock: `GameInstance.RunTime`, moved by `TickScript` by
    the scaled delta, zeroed by `ResetRunClock` at the page's start, standing still while the
    debugger holds the run. The script state reads `GameInstance.ScriptFault`, which the fault
    path now keeps ("faulted in <handler>: <the runtime's problems>", or "did not
    instantiate"); `ScriptRunHost.ReportProblems` can append what it logs.
  - Capture: `ViewportCaptureRecorder` holds the request, record and complete steps both
    pages share (the scene page moved onto it). The Game page records after its overlays, so
    the picture has the game's UI.
  - Found on the way: every editor exit after a PIE run trapped in `ResourceFactorySet`'s
    teardown (a field destroyed before the destructor that used it); fixed on its own.
  - Tested: headless pages behind stand-in play actions: the start's wait (cook, then frame),
    the refusal without a project, `alreadyRunning`, a second instance's own id, state by id
    with frames and game time, an unknown id, a faulted script in the state, stopping one
    leaving the other running, `all`, a start that fails; the capture's wait, its refusal for
    a stopped instance and a stop mid-capture. The engine: a fault is kept with its handler,
    the clock moves and resets, a new start and a clean stop leave no fault. Live in the
    editor: `asset_create` of a first scene made it the default, then `pie_start`, a second
    instance, both screenshots, `pie_stop`, and a clean exit.
- **P4: scripted input and the run** (Editor.Scene, the editor host; L). D5 and D6. Tests: a
  script holding a key moves an input map action's value in the addressed instance and not in
  another; the real source returns after; a
  `pie_run` samples a moving entity at the asked times; `until` stops a run and reports why; the
  screenshots land.
  As built in Sedulous (P4):
  - `ScriptedInputSource` lives in Sedulous.Input, beside `ShellInputSource`, not Editor.Scene:
    it is device level and knows nothing of the editor, and the engine test drives a real
    `GameInstance` through it. A virtual keyboard, mouse and up to four gamepads; `Advance(t)`
    applies every entry due and makes that frame's edges and events, so a press and release
    inside one frame still read as a press; `ReleaseAll` lets go of what is held. Names are the
    enums' own case names, any case.
  - The Game page (`GameEditorPageScripted.bf`) swaps it in for its viewport source on its game
    instance, and on the input subsystem only while that tab holds the subsystem, so the other
    tabs keep theirs. It advances it in OnUpdate; the embedded app's DriveInput runs before the
    pages each frame, so the game reads a frame's script on the next frame. An end is one
    release frame, then the viewport source; a stop drops it at once. `IPieInstancePage` gains
    `RunningScene`, `GetScriptProperty`, `BeginScriptedInput` (ownership transfers),
    `EndScriptedInput` and `IsScripted`.
  - THE CLOCK CHANGED: the timeline, the samples and the run are timed by RUN time, not game
    time. `GameInstance.RunTime` is now the host's unscaled delta. PaperKid's main menu sets
    `Run.TimeScale = 0`, which pauses the scene while the menu keeps working; the P3 clock
    summed the scaled delta, which is 0 then (measured: the instance's scale reads 0 after
    that launch and the scaled delta per frame is 0), so a timeline timed by it could not
    advance to the menu's click. It still stands still while the debugger holds the run. `pie_state` and `pie_run` report `runTime`.
  - `pie_run` (`PieRunTool`, a sixth PIE tool): `input`, `duration` (up to 600 s), `probes`
    (entity field paths: `worldPosition`, `position`, `rotation`, `scale`, `active`, or
    `<component>.<property>` with dotted component ids resolved, then keys, indices or
    x/y/z/w; or `{script: name}`, the game script's fields, private included), `every` (0.5
    default) or `sampleAt`, `screenshots`, `screenshotDir`, `until` (`<`, `<=`, `>`, `>=`,
    `==`, `!=`). Everything is checked against the running game before the run starts. One run
    per instance, found again by its page, so instances run side by side (the HTTP host
    re-enters every unfinished call each pump); `pie_screenshot`'s wait became per instance
    for the same reason. The answer: `endedBy` (duration, until, stopped, timeout: the run
    clock stood still for four times the duration plus thirty seconds), `runTime`, `frames`,
    `samples` ({t, frame, values}, one per crossed sample time plus a final row), `screenshots`,
    `until`, `state`.
  - `entity_inspect` takes `pie` (and an `entity` by guid, name or path) for the running
    game's current scene; `EntityJson` now reads any scene.
  - Found on the way: a PIE run over a freshly cooked project crashed as its menu measured a
    label. The editor re-bound the game UI's font before the cook's hot reload, which then
    freed that product; the bind now follows the reload (its own commit).
  - Tested: the engine: a scripted held key moves one instance's input map action and not
    another's, names parse, a same-frame tap reads as a press, `ReleaseAll`. The tools,
    headless: refusals change nothing, a two second run samples a walking entity at the asked
    times and a script property, shoots at its time, ends and hands the input back; `until`
    ends a run and names why; a stop mid-run; two instances run side by side and only the
    scripted one moves; `entity_inspect` by `pie`. Live, PaperKid in the editor: a timeline
    clicked New Game on its menu (found from a `pie_screenshot`), W rode the bike down the
    street (sampled), `until` stopped it past z 5, the game script's private `m_state` read
    MainMenu then Playing, and the editor exited clean.

Each phase is one commit with its tests and its McpGuide section. The user does the visual
checks.

## Acceptance

- `asset_create` makes every asset in the table that has a creator, in both hosts.
- An agent can start PIE, run a timeline of key presses, read where the player went and what the
  score is, see frames of it, and stop, with no human at the keyboard.
- The editor's New menus and their behaviour are unchanged.

## For the Raptor port

Raptor registers its creators from the editor executable too (`Code/Tools/Tools.Editor/Main.cpp`)
and keeps them on `Editor.Core`'s context (`Context.cppm`, `ContextImpl.cpp`); the move is the
same, to its pipeline core and pipeline registration, with Scene and Prefab in a scene pipeline
library of its own. Raptor's Game page installs the same viewport input source as Sedulous's
(user, 2026-09-29), so the scripted provider takes its place the same way, in the same bind.
