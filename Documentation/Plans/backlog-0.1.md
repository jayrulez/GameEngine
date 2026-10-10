# 0.1 backlog - open items for the 0.1 release

> The work queued for the engine's 0.1 release, in three parts: the editor, the engine, and the
> rest of the release (builds, samples, docs, the website and the flagship game). Moved here from
> `weekly_backlog.md` on 2026-10-10; that file keeps everything else open, and `backlog-0.2.md`
> what waits for 0.2. Pull an item out of here into the current weekly when it is started; delete
> it here when done.

## Editor

- **An orthographic editor camera** (user 2026-10-09): the scene view's camera can switch
  between perspective and orthographic with a toggle (in the view's toolbar, and a key). Pairs
  with the orientation gizmo: snapping to a face is where an orthographic top, front or side
  view is most used. The engine's cameras already have an orthographic projection
  (`MakeCameraProjection`); the editor camera's zoom becomes the ortho size there.
- **The Welcome page needs a purpose** (user 2026-10-09, to think about): it has been a stub
  from the start. Its real job is to hold the top split: with every editor page closed, the
  top section stays for a newly opened page to dock into (without it the Console and Assets
  panels grow to the top and a new scene docks as a tab beside them). Give it a use while it
  does that: perhaps the project's page, with the project's information and quick ways to its
  settings and the other project-level things. Keep the docking job whatever it becomes.
- **An editor logo, and a splash screen while the editor loads** (user 2026-10-09): the editor
  has no logo (for its window and taskbar icon, the project manager and the Welcome page), and
  shows nothing while it starts and opens a project. Design a logo (our own, with its licence
  beside it; GameEngine, never Raptor, in public text), then a splash window with it and the
  loading progress, shown from launch until the main window is ready.
- **Polish the project launcher** (user 2026-10-09): the page the editor opens on to pick a
  project. A list / grid toggle, the grid showing each project's thumbnail (a picture of the
  project to recognise it by); and some of the per-project options moved into a menu (a
  hamburger or similar) so the page reads cleanly.
- **Project-wide defaults for scene settings, per domain** (user 2026-10-09, to think about): a
  scene's render settings come from the scene itself (its own values, or a profile asset it
  names, as Lamplight's levels share theirs), else from hard-coded defaults; other domains are
  probably the same. Add a project layer between: each domain's settings with project-wide
  defaults, set in the project settings, so the lookup goes scene, then project, then the
  hard-coded default. Survey which domains have scene settings, and how a project default
  relates to the profiles (a project default could simply name a profile).
- **Custom shaders through the editor, before the 0.1 release** (user 2026-10-09, ready and
  showcased for 0.1; merged 2026-10-10 with the planning seed of 2026-08-26, "in-editor shader
  editing + material/shader node-graph editor"): custom shaders already work at the engine level,
  and there are shader and shader include assets, but nothing outside the unit tests has used them
  (the shader asset is `Pipeline/Shaders.Pipeline/ShaderAsset.cppm`; includes resolve in
  `Shaders.System`; confirm where the include asset lives when this starts). Expose the pipeline in
  the editor (author a shader asset, a material that uses it, see it cook, compile and draw, with
  its errors shown), and showcase it: a sample project or a demo game's effect built on a custom
  shader.
  - **Origin of the seed**: the parity doc, "MATERIALS: par - both shader-ref + property-set with
    editor pages, NO node graph either side" and "SHADERS: ... Lumix-ahead on one nicety:
    in-editor shader editor WITH LIVE DISASSEMBLY."
  - **What it builds on**: the offline shader cook to a versioned SPIR-V / DXIL / WGSL pack with
    a variant lint (so "live disassembly" is our cook output, already produced); `CodeEditView`
    and its per-language lexer registry (a shader code page with an HLSL lexer, save re-cooks and
    hot-reloads); `MaterialEditorPage` (shader ref, property set, preview), the page a graph would
    feed; the `NodeGraph` UI and `AnimationGraphPage` (three panes, persisted node positions) as
    the precedent for a graph editor; custom materials without renderer changes.
  - **Two tiers to weigh and sequence**:
    1. In-editor shader source editing, the cheaper win: open a shader in a `CodeEditView` page
       with the HLSL lexer; saving re-runs the cook and hot-reloads, and the page shows the cooked
       SPIR-V / WGSL disassembly (from our cooked pack, not a runtime compiler).
    2. A material / shader node graph, the big feature (neither engine compared has one): visual
       nodes (texture sample, math, blend, ... to a surface output) that generate HLSL fed through
       the existing cook, so the variant lint and the compiler-free dist still apply. Authors
       custom materials without hand-written shaders, on the NodeGraph UI and the
       AnimationGraphPage pattern.
  - **The design doc decides** (in `Documentation/Specs/`, before any graph code): tier 1 first
    with tier 2 as the follow-on, or the graph directly; the graph's data model (nodes to
    generated HLSL, how variants and features interact, the material property surface it
    exposes); how generated shaders ride the variant cook and lint; the graph's serialization and
    editor shape (reusing AnimationGraphPage's spine). For 0.1, tier 1 and the showcase are the
    likely scope; whether the graph is in 0.1 is part of the decision.

### Seeded: gizmo vertex snapping (user 2026-08-26)

> Checked 2026-10-10: not built, not even in part. The gizmo still has only the delta snap
> (`translateSnap` / `rotateSnapDegrees` / `scaleSnap` under Ctrl, now `Gizmo.cppm:92-95`), and
> nothing in the editor snaps to a vertex or a surface.

Origin: parity doc, editor - Lumix has VERTEX SNAPPING; "Draconic snaps drag
DELTAS only." Our transform gizmo quantizes the drag delta to grid increments
(translateSnap/rotateSnap/scaleSnap, Ctrl-held, PlayCanvas-style - Gizmo.cppm:
92-95), but cannot snap an object's point ONTO a vertex of other geometry -
precise assembly (align this corner to that wall's corner) is manual.

Goal: a vertex-snap mode for the translate gizmo - while dragging (a modifier /
toggle), snap the moved object so its snap SOURCE point lands exactly on the
nearest VERTEX of the geometry under the cursor.

The pieces (builds on the existing gizmo + scene pick):
- **Target pick** - ray-pick a mesh under the cursor (QueryRay, already used for
  selection/terrain), then find its NEAREST VERTEX to the hit (needs the mesh's
  vertices in world space at edit time - from the bound mesh resource).
- **Source point** - v1 = the moved object's PIVOT snaps to the target vertex;
  v2 = pick a source vertex ON the moved object (Blender-style) so any corner can
  be the anchor.
- **Apply** - offset the object so source -> target; this is an ABSOLUTE snap
  (unlike today's relative delta quantization), a distinct gizmo mode.

Scope for the week: P0 = pivot-to-nearest-vertex snap on the translate gizmo
under a modifier, with a highlight of the target vertex. Source-vertex pick +
edge/face (surface) snapping as P1.

Open questions for week start:
- UX: the modifier/toggle (a held key like Blender's V, vs a toolbar snap-mode
  toggle vs Ctrl-variant) - and coexistence with the existing delta snap.
- Vertex data access at edit time: nearest vertex to the ray hit - do we walk the
  bound mesh's CPU vertices, or sample the picked triangle's corners (cheaper, and
  enough for pivot->vertex)?
- Snap targets: vertex only (v1) vs also edges / faces / the grid; visual feedback
  for the candidate target.

Tests: with vertex snap on, dragging an object onto a mesh places its pivot at the
EXACT nearest vertex position (headless gizmo test, known mesh + known drag);
snap-off leaves the existing delta-snap behaviour byte-identical.

## Engine

- **A script sets a scene's sky texture and environment profile** (user 2026-10-10): no script
  can point a scene at another sky texture or Environment Profile. `EnvironmentSettings.of(scene)`
  leaves its asset references out of AngelScript (the property loop skips a type it cannot declare)
  and in Luau takes only another bound `Ref` of the same type, a plain copy that nothing
  re-resolves (`EnvironmentSystem::ResolveResources` runs only from ResolveSceneResources and the
  editor); `UseSettingsProfile` is not on the script surface. Give scripts what
  `SceneRender.setMesh`/`setMaterial` already do for components: set by asset (guid or a typed
  asset handle) and bind at once, the sky's environment rebaked; switching the profile turns the
  block's source and binds the profile. Mind that the handle resolves to `Effective()`, so in
  Profile mode a write lands in the shared profile. Then tell Sedulous (it copies the value fields
  and leaves its references untouched today).
- **Luau's write error for a mistyped property says "read-only"** (user 2026-10-10): assigning a
  value of the wrong type to a reflected property (a guid or a string to a `Ref` field, say)
  fails in `SetProperty` as InvalidArgument, and Luau's `__newindex` reports every failure as
  "property is read-only" (`LuauScript.cppm`, NewIndexThunk). Say what failed: the property takes
  a value of type X, not Y; keep "read-only" for a property that has no setter.

- **Scene facades as scene properties, as Sedulous has them? Decide before 0.1** (user
  2026-10-09): Sedulous binds a scene facade as a property of the scene (`scene.Physics.RayCast`,
  its SceneFacade role); ours are reached through the facade type (`ScenePhysics::of(scene)`,
  `SceneRender::of`, `SceneAudio::of`, ...), kept so by the ruling of 2026-10-01 until the role
  was revisited. Look at it before 0.1, while the script surface can still change freely: which
  reads more naturally in AngelScript and Luau, what moving means for every facade, the sample
  games' scripts and the docs. The earlier write-up is `Plans/beef-script-facades-note.md`.
- **Luau variants of the sample games, and whether we keep two languages** (user 2026-10-09):
  perhaps write a Luau version of each sample game's scripts (all four are AngelScript), as an
  exercise of the Luau side end to end either way. First decide whether we really want to
  support two scripting languages (the cost of every script facade, test and doc twice, against
  what each brings). The heist plan already had a Luau-only game coming after Lamplight.

### The web demos on a phone (user 2026-10-06)


- **Touch drives the game UI**: the web shell takes touch (`Shell.Web/WebInput.cppm`, start / move /
  end / cancel on the canvas) and consumes it so the page does not scroll or zoom, which also stops
  the browser turning taps into mouse clicks; the game UI reads only mouse, keys and pads. So on a
  phone no menu can be pressed (Snowline's title cannot pick a course). Route touch into the UI as
  pointer input (a tap presses, a drag scrolls). Sedulous (asked 2026-10-06): its UI input pump has
  no touch either; its web shell does not prevent the browser's default, so a tap may arrive as a
  synthesized mouse click there (unverified), a drag never does. New on both sides.
- **Touch controls in the sample games**: the input map already binds touch (`TouchButton`, a screen
  region; `TouchStick`, a floating virtual stick), but no game uses it. Each game binds a stick and
  buttons (Snowline: the left half carves, zones on the right for jump, tuck and grab) and draws a
  light overlay showing them, shown only once a touch is seen.
- **Orientation on mobile**: held upright, a phone gives the canvas a portrait shape, and the games
  are landscape. Options: ask the browser for landscape (`screen.orientation.lock`, which needs
  fullscreen and is refused on some browsers), letterbox a landscape render into the portrait canvas,
  or rotate the presented image a quarter turn with the input transformed to match, and a prompt to
  turn the phone where nothing else works. Likely the larger item of the three: the shell's canvas
  sizing, the player's fit, and every pointer and touch coordinate are involved.

### Seeded: GPU profiler + profiler visualization (user 2026-08-26)

Origin: same parity doc - "GPU PROFILER: Lumix-ahead - unified CPU/GPU profiler
timeline + pipeline statistics + NVML telemetry + PIX/RenderDoc; Draconic:
per-pass graph timestamps + CPU record-time report, SEPARATE from its CPU
profiler." We have both halves but they are text-dump only and disjoint.

What exists ([[profiler-system]]): `draconic.profiler` CPU scopes
(compile-gated) + a GPU `GraphProfiler` (per-pass Vulkan timestamps); the P key
DUMPS TEXT. Two gaps, and the user asked for both:

1. **GPU profiler** - broaden past the current per-pass Vulkan-only timestamps:
   finer scopes and coverage on the OTHER backends (WebGPU timestamp-query where
   available, DX12) so the GPU timeline is not Vulkan-only. Pipeline statistics
   (draw/primitive counts) alongside the timings if cheap.
2. **Profiler visualization** - replace the text dump with a VISUAL profiler
   panel (timeline / flamegraph) that UNIFIES the CPU scopes and the GPU
   timestamps on ONE per-frame timeline (the "separate" problem the parity note
   calls out). ImGui is already integrated ([[imgui-integration]]) - a candidate
   host for the overlay - but decide editor-panel vs in-app-overlay at design.

Open questions for week start:
- The shared data model: merge CPU scope trees + GPU pass timings into one frame
  record with a common clock/axis (GPU timestamps resolve a frame or two late -
  the view must align them to the right CPU frame).
- Where it lives: an editor profiler panel, an ImGui overlay in the samples, or
  both reading the same capture. Keep the capture layer UI-free.
- Portability: WebGPU timestamp-query is optional/limited and DX12 differs from
  Vulkan - the GPU side degrades gracefully where timestamps are unavailable.
- Compile-gating stays (the CPU profiler is already gated); the visualization is
  a debug tool, not shipped in dist.

## Other

The release itself and the work around it: builds and packaging, the samples, the docs, the
website and the flagship game.

- **Documentation cleaned up and made proper, for the 0.1 release** (user 2026-10-09): the
  documentation as a user of the engine meets it, current and organised (getting started, the
  editor, each system, scripting, exporting, the MCP guide), with the plans, specs and history
  that grew in `Documentation/` kept apart from it or archived.
- **The 0.1 release** (user 2026-10-09): the release itself, with
  - **a website** for the engine (beyond the demos site, GameEngineDemos);
  - **a showcase game**: the engine's flagship, released on Steam. Separate from the sample
    games in this repository and not part of it (its own project and repository); the samples
    stay the engine's examples.
  The items marked for 0.1 above (custom shaders through the editor, the documentation, the
  fifth sample below) belong to it.
- **A fifth sample game, on networking and crowds, before 0.1** (user 2026-10-09): no sample
  project uses the network stack or crowds yet, so neither has been showcased or stressed in a
  game. The fifth sample in `Data/SampleProjects/` built around both: players over the network
  (the engine's reliable UDP and replication) and a crowd (instanced skinning, navigation
  crowds), built and playtested through the MCP tools as the others were. (Lamplight's crowd
  was set aside "for later" when it was planned.)
- **Sort the backlog and plans into 0.1 and 0.2** (user 2026-10-09): go through `weekly_backlog.md`,
  the plans and everything else open (`Documentation/Plans`, `Ideas`, `Specs` with open parts,
  `terrain-backlog.md`), keep what 0.1 needs, and move everything that will not be looked at for
  0.1 to 0.2, so what is left in front of 0.1 is only the release's work.
- **The 0.1 release builds and samples** (user 2026-10-09): build and package what users
  download for 0.1: the export templates (each platform), the editor and the command-line tools
  (Tools.Mcp, Tools.Export, Tools.Cook and the rest a user needs; `scripts/build-editor-dist.*`
  and `scripts/build-export-templates.*` are the starting point), and the sample projects
  packaged so users have something to open and play with. Perhaps the project launcher lists
  remote samples too, to download and open from there (pairs with the launcher polish above).
