# 0.1 backlog - open items for the 0.1 release

> The work queued for the engine's 0.1 release, in three parts: the editor, the engine, and the
> rest of the release (builds, samples, docs, the website and the flagship game). Moved here from
> `weekly_backlog.md` on 2026-10-10; that file keeps everything else open, and `backlog-0.2.md`
> what waits for 0.2. Pull an item out of here into the current weekly when it is started; delete
> it here when done.

## Editor

- **UAT checks for the user** (user 2026-11-02, from retiring the old smoke checklist): what the
  checklist held that only a person can judge, everything else having been checked, covered by
  tests or made obsolete (week 2026-10-24, "The old smoke checklist retired"):
  - Listening: a paused scene's sounds fade together while one-shots still play; four emitters
    on one clip at 0.75, 1, 1.5 and 2 pitch sound as four notes (AudioPlayground); a bus layout's
    muffled effects and quieter music; varied shots and the cave's reverb (AudioPlayground); a
    sound cue's audition; a reverb zone and a source's reverb send; a named bus muffled alone; no
    click when a voice is stolen.
  - Hardware: the InputActions panel, a rebind kept over a restart and the Deck's touch zones; touch
    in the Game tab on a touchscreen; a real gamepad's focus moving in a game's menus, and two pads
    with the first unplugged (web); dead keys typed into a game's text field; Firefox's wheel
    speed; a HiDPI mouse in the browser; F5, F12 and Ctrl+C reaching the browser while space and the
    arrows do not scroll; iOS Safari, and a phone's adapter, compression formats, memory and frame
    rate; a Windows run of the VG gradients; moving the editor between monitors of different scales.
  - Looks: the PhysicsPlayground's character climbing, pushing and the hinge blade; a character's
    capsule cyan grounded and purple airborne; the graph page's preview (the skeleton playing, the
    active state ring, "blending in"); the clip and skeleton pages' wireframes and the chosen bone's
    marker; the clip page's playhead; the particle page's emission gizmo and curve editing; the
    WebScene content list and its ImGui panel (desktop and browser), the shadow toggle test with the
    sun or its cascades off, and the SSR sliders; billboards placed in the camera preview inset;
    several restored floating windows all drawing on Wayland.
  - Out of the agent's reach: a local's value on hover while a run is paused (`editor_hover` does
    not raise the code editor's hover), and keys typed into another panel never reaching a playtest
    (`editor_key` goes through the editor's keymap only).
  - Split-screen game UI (each half its own HUD, billboards per half) waits on a GUI block in the
    Sandbox's two-view mode.

- **An HDR image's preview is black** (user 2026-11-02, UAT): an `.hdr` imported as an Image
  (`kloofendal_48d_partly_cloudy_puresky_1k.hdr`) shows its facts (1024 x 512, RGBA32F, Linear) but
  the picture is black throughout; a PNG's picture shows. The page's comment says an HDR source
  previews clamped (`ImageEditorPageImpl.cpp`).
- **The skeleton page does not follow a reimport** (user 2026-11-02, UAT): with the Crab's skeleton
  page open, a bone renamed in `Crab.gltf` and reimported reaches the skeleton asset and recooks,
  but the page keeps the old bones until `page_reload`. The page should rebuild when its object
  changes (`SkeletonPageImpl` OnObjectChanged), with a test.
- **Three smaller findings from the UAT check** (user 2026-11-02):
  - The script page stays dirty ("Enemy *", Save enabled) after undo returns to the saved text; the
    undo stack has no clean point there.
  - The debugger's Locals show garbage for a local declared further down the function while paused
    above it (`dx = 3.27e-41`); say it is not declared yet, or leave it out.
  - `editor_hover` does not raise the code editor's hover popups (a type name's API hover, a local's
    value while paused), so an agent cannot check them; the hover reaches the code editor's own
    hover path.
- **Four small editor findings from the UAT check** (user 2026-11-02):
  - The Export dialog's "Add Files..." button (`#export-add-files`) is drawn over the "Exports go
    to <path>" text at its bottom left, cutting the path in two.
  - `pie_run` does not create its `screenshotDir`: a capture into a folder that is not there fails
    ("could not write ..."). Make the folder, as `editor_screenshot` does its own (check).
  - An Image asset's tile in the Assets panel shows a placeholder, not its picture.
  - A delete (and likely an import) asked for during a cook waits for the cook and then runs, but
    nothing says it is waiting; the old "queued until the current cook finishes" notice is gone.
    Say so in the status bar or a toast.

- **The editor's frame, what is left** (user 2026-11-02, after comparing the old UI and the GUI):
  The user finds the editor slower than the old one, which was snappy even in Debug. Measured so far
  (Debug, clang): on one page of rows built in both sandboxes (GUI_PERF_ROWS / GUI_PERF_MODE, timing
  code uncommitted in UISandbox, UIRuntime.cppm and GUISandbox), the GUI was no slower than the old
  UI: at 150 rows a relayout 6.4 ms against 8.2, render 1.2 ms against 3.4, 42 draw commands against
  153. The old editor's viewport called the layout-affecting `Invalidate()` every frame it showed, so it
  relaid out every frame too. The user's printed frame was CPU-bound (38 ms, 1.2 ms on the GPU fence),
  three 3D views about 18 ms of it. Fixed: the toast host's clock asked for a frame every frame
  (fc5842fec). Held until the user has felt the editor with that fix; each item is measured before and
  after:

  1. **Disabled icon buttons draw through offscreen layers.** `button:disabled { opacity: ... }`
     (`Data/Editor/editor.css:265`) on a node with children fades it through a VG layer
     (`Node.cppm`, `layered`), one render pass each: 8 a frame on Sky Hopper's Level 1 (the disabled
     toolbar buttons). Options: a disabled look by colour (`image-color`, text colour), or opacity
     multiplied into the children when they do not overlap.
  2. **Draw calls break at every tree row.** 272 GUI draw commands a frame on Level 1, most of the GUI's
     main-thread time in driver calls (`VGRenderer::DrawCommands`). A row draws its background, its icon
     (its own texture, Layer mode) and its text (the glyph atlas), and each change of texture or mode
     starts a new command. Options: the draw mode per vertex and the glyph atlas and an icon atlas bound
     together, or merging commands across non-overlapping draws. The largest of the items: the VG
     renderer. First, the cheap part (user 2026-10-28, from a RenderDoc capture where every icon was
     its own draw): `DrawImagePremultiplied` closes the open command before every image
     (`Foundation/VG/Context.cppm`, FlushCurrentCommand), even the next one on the same atlas page
     with the same clip, blend and draw mode, so a bar's icons are a draw each. Keep the command open
     while the state holds, with a test counting the commands a row of icons makes; first verify the
     icons land on one atlas page (the picture baker's pages, GpuPictureBaker), not a texture each.
     Every clip push also closes a command.
  3. **The scene overlay walks every entity each frame.** `SceneOverlays::DrawGizmos` and
     `DrawEntityMarkers` (`Editor.Scene/SceneOverlays.cppm`) visit every entity, and each one every
     component manager (`GizmoRendererRegistry::DrawEntity`): about 19% of the main thread on Level 1.
     The old editor did the same; skipping entities with nothing to draw would save it.
  4. **Input to screen, in frames.** Not measured: whether input reaches the screen later in the GUI
     than in the old UI (routing, hover, layout order). Count frames from an injected event to the
     changed picture in both sandboxes. The editor's third swapchain image (4f7a8931b, the user's
     ruling) adds a frame while the editor keeps up with vsync, little at 26 fps.
  5. **Unscoped time.** About 5 ms of the printed frame's Update and 5.4 ms of its Render had no
     profiler scope (the GUI host's input and context update, the pumps: MCP, the Samples tab,
     tasks, export, the status bar). Scopes first, then measure.

- **Terrain and vegetation tools: fixes and a regression check** (user 2026-11-01):
  - Sculpting (and maybe the vegetation tool) still acts when the pointer is over UI: a press or
    drag over a panel or the tool's own controls goes through to the viewport.
  - The paint tool's layers UI looks poor and needs a design pass. Its slots spilled over their
    row and were too small to tell apart: they fit, at 40 pixels, and wrap (2f6f0c9b8); the help
    line names the minus key (9d70e997b). The vegetation panel checked on Meadow: fine.
  - Check the vegetation tool the same way.
  - Compare against the old editor's painting (on the `vg` branch, before the editor moved to the
    new GUI) for regressions in behaviour, brushes and controls.

- **Project-wide defaults for scene settings, per domain** (user 2026-10-09, to think about): a
  scene's render settings come from the scene itself (its own values, or a profile asset it
  names, as Lamplight's levels share theirs), else from hard-coded defaults; other domains are
  probably the same. Add a project layer between: each domain's settings with project-wide
  defaults, set in the project settings, so the lookup goes scene, then project, then the
  hard-coded default. Survey which domains have scene settings, and how a project default
  relates to the profiles (a project default could simply name a profile).
- **Vertex snap, the next steps** (from the seeded item, 2026-08-26; the pivot-onto-vertex snap
  is built, week 2026-10-10): pick a SOURCE vertex on the moved object (Blender-style, any corner
  as the anchor, not only the pivot); snap to edges, faces and the grid as well as vertices; a
  skinned mesh in its current pose (today its bind pose, the pose its CPU vertices hold); the
  instanced meshes and other drawn surfaces answering `ISceneSurfaceQuery` too.

- **The PIE tools an agent was missing** (user 2026-10-31, from playtesting the run branch in the
  editor over MCP): what a user does to a Game tab, an agent could not, and had to click the GUI
  for (`editor_click` on `#game-pause`, a hover then the tab's `TabCloseButton`):
  - **Close a Game tab.** `page_close` takes an asset guid and refuses a PIE id ("invalid guid
    'game-page-1'"); no PIE tool closes one. Closing an extra tab releases its instance (its run
    destroyed), a path worth driving in a playtest. Either `pie_close {pie}` or `page_close`
    accepting a PIE id.
  - **Pause and resume one.** No tool sets the tab's pause (the toolbar's toggle, which pauses
    the run: its simulation and its sound), and `pie_state` does not say whether a tab is paused
    (`runTime` keeps counting frames either way, so it cannot tell).
  - **Bring one to the front.** No tool activates a Game tab. With the item below, that is also
    how an agent focuses its run (the one heard, with "Game audio: all instances" off).
  Each new PIE tool bumps `kPieToolCount` (`Code/Editor/Editor.Play/PieMcpTools.cppm`, 7 today)
  and goes into McpGuide.md with the PIE tools.

- **A Game tab made active focuses its run** (user 2026-10-31): today a run takes the focus (the
  one heard, the one the run manager reports focused) only when Play is pressed in its tab
  (`GamePageImpl.cpp:536`) or its viewport takes the keyboard (`:706`); selecting the tab alone
  does not, so switching tabs keeps the other run's sound. The editor knows the active page
  (`EditorContext::ActivePage()`, following the dock's active tab through `NoteActivePanel` in
  `EditorPages.cppm`): the Game tab focuses its run when it becomes the active page, as well as on
  a viewport click.

- **The assets browser's slow-click rename and a typed guid selecting** (X8 parity check, gap 8;
  user 2026-10-27: evaluate later): the old browser started a rename on a slow second click
  (AssetsView.cppm:330 at 4fe177db2) and selected the asset a whole typed guid names, so Enter or
  a double click opened it (AssetsViewImpl.cpp:733); the new one renames by F2 or the menu (GUI
  ItemView begins an edit only on F2 or BeginEdit) and only filters by the guid. Decide whether
  either comes back.
- **An asset or entity dropped on a list appends to it** (X8 parity check, gap 3; tabled by the
  user 2026-10-27): the old ContainerListEditor (ContainerListEditorImpl.cpp:26-76 at 4fe177db2)
  took a drop on a list of references (mesh materials, reflected lists, project fonts, particle
  materials, behaviours) as a new element, a wrong type refused with the types it takes. GUI.Toolkit's
  ListPropertyEditor appends only by Add. Needs a design: the list asking its element's editor
  whether it takes the payload, and the insert and the write as one undo step.
- **Curves and gradients edited in a popup, not on the row** (X8 parity check; user 2026-10-27:
  discuss later): the old particle page edited a curve or a gradient on its row; the toolkit shows
  a preview row that opens an editor in a popup (where a stop's colour can now be edited too).
- **A Composite 2D binding's four keys as a keyboard cross, not rows** (X8 parity check; user
  2026-10-28: backlog): the old input map page drew the four keys as they sit on a keyboard, up
  over left, down and right, each a keycap that captured its own key (InputMapPageImpl.cpp:872-890
  at 4fe177db2); the new page shows them as four rows. Decide which reads better.

- **The editor's accessibility pass** (editor-gui X9; user 2026-10-28: backlog): every control
  reached by the keyboard in a sensible order, focus always shown, every icon-only button named
  (a tooltip at least), the token pairs' contrast checked in both schemes and high contrast, and
  reduced motion honoured everywhere. The keyboard and focus parts are testable; the looks are the
  user's.

- **The profiler page lays its labels out again every frame** (user 2026-10-28: backlog): with the
  page open and recording, GUI.Draw took 5.8 ms a frame in Debug, 5.0 ms of it the profiler views'
  own drawing and 2.4 ms of that text layouts the cache did not hold, 103 a frame (the scopes of
  87be72361 showed it). Every view draws its labels through one `Text` (ProfilerWidget::DrawLabel),
  changing its string per label, so each label is shaped and broken again. Keep laid-out labels by
  their text (a small cache in the views) so an unchanged one is reused; then measure the timeline's
  bars, the rest of the views' 2.6 ms.

- **A process's resident memory on every platform** (user 2026-10-28: backlog): GUISandbox's soak
  log reads `/proc/self/statm`, so it reports 0 MiB resident off Linux. Core's system layer could
  answer it everywhere (`GetProcessMemoryInfo` on Windows, `task_info` on macOS) and the sandbox ask
  it.

- **Windows that ask for high pixel density** (user 2026-10-28: backlog): on a scaled Linux (Wayland)
  or macOS display SDL gives a window its logical size unless it asks for
  `SDL_WINDOW_HIGH_PIXEL_DENSITY`, and the compositor stretches it: blurry text, though it fills. With
  the flag the GUI draws at the display's pixels, as on Windows (tried locally at 133%: correct after
  the root's clip fix). Decide whether every window asks, and check the 3D views' cost at the higher
  resolution.

## Engine

- **Export pruning misses assets a scene hands its scripts** (user 2026-11-02, from the UAT
  check): a behaviour's asset-valued property set in a scene (an override of asset kind, a guid)
  is not followed by the reachability scan (`SceneReferenceScanner`), so a pruned export drops what
  it names. Sky Hopper's Level 1 gives its Crab a walk clip and its Skull a model that way; a pruned
  Linux export dropped both and the player logged "bind failed" for each. Follow the overrides in the
  scan, with a test of a scene whose only reference to an asset is a behaviour's property. Apart
  from it, by design: what a script loads by name (Sky Hopper's Levels 2 to 5, its sounds) needs
  Always Export (`Systems/export-reachability.md`, the unbuilt script-load lint, Phase 4).
- **A VG gradient's lookup texture uploaded once a frame, each upload waiting** (user 2026-11-02,
  from the UAT check): VGSandbox makes a new 256x1 lookup texture about once a frame (a new image
  instance each time), and `VGRenderer::GetOrCreateCachedTexture` uploads each through a transfer
  batch of its own that it submits and waits on. Find why the ramp is new each frame (an animated
  gradient, or a cache that misses), keep one per ramp, and put the frame's uploads in one transfer
  or the frame's own encoder; then check whether the GUI's animated gradients do the same.

- **Terrain lit as the forward lights** (user 2026-10-31): `terrain.ps.hlsl` lights with a
  hard-coded ambient (`float3(0.28, 0.30, 0.34)`) and a sun term of `ndl * shadow` with no colour or
  intensity, so terrain ignores the scene's sky, ambient and key light while everything on it
  follows them. The scene's ambient is reachable: the terrain renderer gets the same render context
  as the mesh renderer, and `ctx.ambient` is `scene->Ambient()`, the value `view.hlsli`'s `Ambient`
  carries; it would go in `GpuTerrainView` / `TerrainView` (the pad after `LightDir` is free).
  Taken alone it would darken terrain badly: in the forward, `Ambient` is a fill (default 0.03) on
  top of the IBL diffuse and specular (`lighting.hlsli`, AmbientLight), and terrain has no IBL.
  Light terrain with the forward's ambient model (the SH or IBL diffuse, the flat fill, AO) and the
  key light's colour and intensity, ideally through `lighting.hlsli` rather than a copy, then
  retune the terrain samples (Snowline, the terrain playground) with the user, since every terrain
  scene's look changes.

- **Reflection probe resolution as a scene setting** (user 2026-10-31): every probe captures at
  `ReflectionProbeSystem::kCaptureRes` (128) and prefilters at `kPrefilterRes` (128), because all
  probes share one cube array and its slices cannot differ in size. The component's per-probe
  `resolution` never reached the renderer, so it was removed (ReflectionProbeComponent data version
  1; a v0 probe's value is read and dropped). Make the size a scene render setting instead (64, 128
  or 256, beside the environment's other settings) that rebuilds the captured and prefiltered
  arrays when it changes and recaptures every probe (check whether the prefilter's mip count,
  `kPrefilterMips`, should change with it).

- **Windows on X11 stay composited, as a setting** (user 2026-10-30, from
  https://github.com/jayrulez/GameEngine/pull/15, closed): SDL asks X11 compositors to stand
  aside for its windows by default (`SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR`), so the
  editor, the tools and a windowed game are drawn uncomposited on an X11 desktop. The pull request
  set the hint to 0 in `SDL3WindowManager::CreateWindow`; the review asked for the hint to be set
  once, in the SDL3 shell's constructor with the other global hints (`SDL3ShellImpl.cpp`, before
  SDL starts), and for it to be a setting that flows through the window settings, since a
  fullscreen game may still want the compositor out of the way: composited by default, bypassed
  when a game asks. The commit as `Area: what it does`, saying the problem and how it was
  checked (an X11 desktop with a compositor; no unit test). Its author offered to take it.

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

- **Players rebind their controls** (user 2026-10-12; found writing the input docs): a player's
  rebinding has a home, `InputBindingOverrides` in the user settings (`Foundation/Input/
  InputMap.cppm`), and only the InputActions sample applies and saves it (`ApplyBindingOverrides`);
  the engine does not, so a game cannot offer rebinding. Apply the overrides when the input map
  binds at startup, and give scripts what a
  controls menu needs: list an action's bindings with their display names, listen for the next
  input to rebind one, reset to the map's own, and save. Then the input docs page gains a section.
- **Debug views hold still under TAA** (user 2026-10-13): a debug view (albedo, normals,
  roughness, depth, ...) shows a buffer from before the TAA resolve, which TAA's sub-pixel jitter
  shifts every frame, so its edges shimmer. While a view shows a debug view, draw it unjittered
  (the pipeline applies the jitter where `post.taaEnabled`), with a test that a debug view's
  projection carries no jitter.
- **Smoother shadow edges** (user 2026-10-12; found checking the toon's shadows): a directional
  shadow is filtered by a fixed 3x3 PCF grid (`SampleCascade` in `Data/Engine/Shaders/lighting.hlsli`),
  at 1024 texels per cascade (`kShadowResolution`, `ShadowSystem.cppm`). Soft shading hides the
  grid; a hard edge (a toon's) traces it as a wavy outline. Filter with a disk of 12 to 16 taps
  rotated per pixel, and make the resolution a render setting (1024, 2048, 4096); local-light
  shadows (`SampleLocalShadow`) the same. Contact hardening (PCSS) after, if wanted. Changes every
  game's shadows slightly: the full lanes and a visual check on the sample games.

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

- **Spec-gloss and legacy FBX materials keep their specular** (Sedulous's user, 2026-10-13; they
  port it after): both model importers bring Bistro, and anything like it, in as flat dielectrics.
  glTF (`GltfLoader.cppm`, the `KHR_materials_pbrSpecularGlossiness` branch): the diffuse stands in
  for base colour, metallic 0, roughness 1 - glossiness factor, and the specular-glossiness texture
  (RGB specular, A per-pixel glossiness) is ignored, so per-pixel roughness and any metal are lost.
  Wanted: the standard spec-gloss to metal-rough conversion, metalness solved from diffuse and
  specular as the Khronos sample viewer and gltf-transform do, and a metallic-roughness texture
  baked per pixel at import. FBX (`FbxLoader.cppm`, the legacy Lambert/Phong path): metallic 0 and
  roughness 0.8, the specular colour, its texture and the shininess never read. Bistro's FBX binds
  its `*_Specular.dds` to Phong's SpecularColor slot, and its README says those maps pack occlusion
  (R), roughness (G) and metalness (B), so all of its maps are dropped. Wanted: the spec-gloss
  style conversion for real specular maps. The FBX half is now `Archive/phong-material-history.md`: legacy
  Phong and Lambert materials import as authored, onto a built-in Phong material that keeps their
  specular. The import option once wanted here, to read Phong's specular slot as packed ORM, is
  dropped (user, 2026-10-27: no importer case for one asset's convention); Bistro's materials are
  fixed by a script outside the engine (rebuilt as PBR with the packed maps wired as such) so it
  imports normally. Test data: Bistro v5.2 (FBX only) at `~/Downloads/Bistro_v5_2`.
- **A slot's own depth and shadow program.** The forward pass draws each submesh with its slot's
  material (2026-10-15), but the depth prepass, shadows and picking still draw a mesh (and an
  instanced set) whole with its first material (`MeshRenderer::DepthLikeFor` over `md.material`):
  a custom shader that moves its vertices or cuts out, on any slot but the first, casts and
  occludes as if it did neither. Wanted: per-submesh depth draws where a slot's material has its
  own depth program, wind, or a cutout.
- **The player opens on a black window** (user 2026-10-29, running the exported Linux players): the
  window stays black through startup (the graphics device, the shader and content packs, the GUI's
  fonts and sheets, the game script), then the title appears. The player's only drawn screen is the
  GUI loading overlay (PlayerApplication.h, the boot splash), shown just while the first scene
  loads async, so it shows only where that load is long (Masquerade, the Shader Gallery). The
  editor has what the player lacks: a splash drawn on the CPU into a software window opened before
  the device (EditorSplash.cppm). Wanted: measure a release player's startup phases first (where
  the black time goes; GPU pipeline creation on the first frames is a guess, unmeasured), draw a
  splash at once as the editor does and hold it to the first real frame (the web player its
  equivalent in the page), and cut the time where the measurements show how (a pipeline cache kept
  on disk, if the player keeps none).
- **A blue cast on PaperKid's road and cars, seen once** (user 2026-10-29, playing Block 1 in the
  editor): for two or three frames about every third of a second, then held while paused, the
  road, the pavement and the cars brightened toward blue (road 82,93,123 to 98,147,214) while the
  lawn, the house walls and the sky stayed exactly as they were. Ruled out: the toon shader (it
  held with the materials on PBR), geometry and culling (the scene page drew the road from the
  same camera), TAA and the sky's specular (neither switch changed it); PaperKid has no reflection
  probes and SSR is off. It came in a long editor session with many MCP captures, running the
  build of 76d1ee64a (the tone map fix); the same build played through every level with validation
  on showed nothing, nor did the build before it. That session's log was overwritten. If it comes
  back: keep the log (validation is on in Debug), pause, and capture the editor and the Game tab.

### The GUI's filters: drop-shadow and colour (UI survey 2026-10-14; reframed 2026-10-28)

The survey's gaps were found in foundation.ui, which the engine is leaving; checked against
foundation.gui, what is still open is its filters. `filter` and `backdrop-filter` take `blur()`
alone (`StyleValue.cppm`, ParseFilters): a list naming any other function does not parse.

- **A content-shaped glow**: CSS's `filter: drop-shadow()`, a blurred, tinted copy of what a node
  draws, under it. It needs a layer drawn twice, sharp and blurred. A box's glow is already a
  `box-shadow` with no offset.
- **Colour filters**: `brightness()`, `saturate()`, `contrast()`, `grayscale()`, `sepia()`,
  `hue-rotate()`, `invert()` and `opacity()`. They need a colour pass in VG's layer composite,
  which it does not have yet.

### Scripts

- **A component's `EntityRef` properties are not on the script surface** (found building
  Masquerade, 2026-10-14): AngelScript skips a property whose type it cannot declare, so
  `InstanceMemberComponent.instancedMesh`, `InstancedSkinningComponent.targets` and any other
  entity reference cannot be read or set from a script (Masquerade groups its guests by costume
  and switches a hunter's look by activating children instead). Expose them as `Entity`, resolved
  through the component handle's scene, in both backends.

## Other

The release itself and the work around it: builds and packaging, the samples, the docs, the
website and the flagship game.

- **`--screenshot` in the framework samples** (user 2026-11-02, from the UAT check): the samples
  built on `SampleApp` (VGSandbox, the RHI samples, WebTriangle) take the `--screenshot` flags, but
  none calls `CaptureBackbuffer`, as the framework asks (since 8ad96bca7, 2026-10-16), so no picture
  is written and with `--screenshot-exit` the run never ends. Let the framework capture by itself
  (or give each sample the call), with a test that a sample run with the flags writes its PNG and
  exits. It blocks the UAT's VG gradient and blend checks (pad, reflect, radial rings, screen blend,
  the SVG badge).

- **Documentation cleaned up and made proper, for the 0.1 release** (user 2026-10-09): the
  documentation as a user of the engine meets it, current and organised (getting started, the
  editor, each system, scripting, exporting, the MCP guide). Ruled 2026-10-11: it is written
  fresh in its own repository, `AssiduousEngine/docs` (Astro Starlight, private source, published
  to the public `docs-site` at https://assiduousengine.github.io/docs-site/), from the DeepWiki
  page, corrected against `Documentation/` and the source; this repository's `Documentation/`
  stays as the engine's development documentation. Started 2026-10-11: the home page and the
  introduction, with the outline in its README.

  Where it stands (2026-10-31, paused by the user): every page was checked against the source and
  rewritten plainly (docs 95cf7ba); the four screenshots from the old editor UI are replaced; 37
  screenshots are in, through docs 26cc8a9. Still to capture, each under the section named:
  - editor: the import dialog for a .glb (assets "Importing your own files"); the Snap step menu
    (scene-viewport toolbar); the F3 overlay in a running game (profiler); paused at a breakpoint
    with Call stack and Locals (debugging "Looking around");
  - rendering: the Initializers right-click menu (particles); New Splat Weights and the terrain
    tools menu (terrain "Making a terrain"); a Terrain Vegetation procedural layer with grass
    (vegetation); Sky Mode HDREquirect in the scene row and a texture page's Preset menu on
    Equirect Sky (skies); a profile row with Source = Profile, and a polished floor with and
    without a probe (environment); one leaf card as Opaque, Masked and AlphaBlend (meshes
    "Blending");
  - scripting: the Script component with two behaviors, the scene's Level Script field, and the
    Assets menu at New > Scripts;
  - systems: the property animation panel (dopesheet and curve view) and a clip's Root motion
    section (animation); the Physics section's Collision Groups grid and a Collision Shape
    asset's inspector (physics); a style sheet page with its Previewed Over pane and a nameplate
    in game (user interface);
  - tools: the model viewer's model, Browse and Library tabs and its Preferences.
  The capture recipe: the editor with `--mcp-port` on a scratch copy of a sample, pages opened
  with `page_open`, dialogs through the command palette (`action_execute` cancels dialogs), shots
  with `editor_screenshot`, cropped and placed under each section's first paragraph.
- **The 0.1 release** (user 2026-10-09): the release itself, with
  - **a website** for the engine: started 2026-10-11 as `AssiduousEngine/website` (Astro, private
    source, published to the public `assiduousengine.github.io`, live at
    https://assiduousengine.github.io/): a landing page and the sample games, whose web builds
    publish apart to the public `AssiduousEngine/demos` at `/demos/` (the website's
    `scripts/publish-demos.sh`), replacing the old GameEngineDemos site (user 2026-10-11); the
    domains are the user's, their DNS not yet set up;
  - **a showcase game**: the engine's flagship, released on Steam. Separate from the sample
    games in this repository and not part of it (its own project and repository); the samples
    stay the engine's examples.
  The items marked for 0.1 above (the documentation) belong to it.
- **The model viewer, a standalone build** (user 2026-10-30): the viewer is in use for curating
  the flagship game's assets, from the Debug tree. Build the relocatable release with
  `scripts/build-model-viewer-dist.sh` (RelWithDebInfo, the clang-reldbg preset: a full build, so
  it takes the build lane), check it starts from its own folder, and ship it with the 0.1 tools;
  a Windows build of the same with the other Windows downloads.
- **The 0.1 release builds and samples** (user 2026-10-09; after every other 0.1 item but the
  release notes and download links, user 2026-10-11): build and package what users
  download for 0.1: the export templates (each platform), the editor and the command-line tools
  (Tools.Mcp, Tools.Export, Tools.Cook and the rest a user needs; `scripts/build-editor-dist.*`
  and `scripts/build-export-templates.*` are the starting point), and the sample projects
  packaged so users have something to open and play with. Perhaps the project launcher lists
  remote samples too, to download and open from there (pairs with the launcher polish above).
- **The players' size in the export templates** (user 2026-10-29), measured 2026-10-29:
  - Native: neither template's player is fully stripped, so every exported game carries 5.5 to
    5.8 MB of symbol names (Linux 22.8 MB, Deck 24.9 MB). The Linux template strips only the debug
    info (its `.debug` is kept beside the build); the Deck template is made from the build's
    unstripped player (`build-steamdeck.sh` runs `Tools.Export --template create "$BIN"`), while
    the container strips only the copy it stages into `dist/` (19.0 MB). First find whether the
    crash log names its backtrace from the symbol table; then strip fully, keeping a `.debug`
    beside each build (the Deck build keeps none). The Deck player is larger by its static C++
    runtime (about 1 MB, needed for SteamOS) and, plausibly, `-O3` against `-O2`.
  - The code itself (13 MB on Linux): the engine's own about 7 MB, of which the reflection and
    script-binding glue is 1.7 MB in 5,330 small template instances and the GUI 1.4 MB; SDL3
    1.7 MB, Jolt 1.2 MB, HarfBuzz 0.9 MB, AngelScript 0.8 MB and Luau 0.4 MB (both languages
    linked, a game uses one), miniaudio 0.5 MB. Unwind tables 2.1 MB.
  - Web: the shipping wasm is 15.2 MB (4.5 MB gzipped), almost all code (14.6 MB), and already
    carries no names. It is built at `-O3` with `-sASYNCIFY` unrestricted, which instruments
    every function that may reach a yield and is commonly a large part of a wasm's size;
    `ASYNCIFY_ONLY`/`ASYNCIFY_REMOVE` lists, JSPI where browsers have it, `-Os`/`-Oz` and LTO are
    the levers to measure. The web profile player is no larger than the shipping one (15.18
    against 15.22 MB), not yet explained.
