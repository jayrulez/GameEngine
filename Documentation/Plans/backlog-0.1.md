# 0.1 backlog - open items for the 0.1 release

> The work queued for the engine's 0.1 release, in three parts: the editor, the engine, and the
> rest of the release (builds, samples, docs, the website and the flagship game). Moved here from
> `weekly_backlog.md` on 2026-10-10; that file keeps everything else open, and `backlog-0.2.md`
> what waits for 0.2. Pull an item out of here into the current weekly when it is started; delete
> it here when done.

## Editor

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

- **Scene and prefab thumbnails never draw** (user 2026-10-26; the old editor too): the assets
  browser and the pickers show a scene's and a prefab's icon, never a picture, while meshes and
  materials draw. The generators exist (Editor.Scene's AssetThumbnailsImpl.cpp: Prefab- and
  SceneThumbnailGenerator) and are registered with the scene editor; they are the two that stage
  into a private scene (`NeedsPrivateScene`), which the thumbnail stage creates on its own scene
  manager (Editor.Preview's ThumbnailStageImpl.cpp), whose installer the scene subsystem does set.
  Not yet found: whether their jobs are queued at all, whether their Stage fails (the prefab
  payload read, the spawn, the scene load) or times out waiting on meshes, or whether the private
  scene renders empty. Their failures log nothing today: a warning on a failed thumbnail would
  have shown which.
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

- **Images that share a texture draw in one call** (user 2026-10-28, from a RenderDoc capture of
  the editor: every icon is its own draw): VG's DrawImagePremultiplied closes the open command
  before every image (Context.cppm), even the next one on the same atlas page with the same clip,
  blend and draw mode, so a bar's icons are a draw each. Keep the command open while the state
  holds, with a test counting the commands a row of icons makes. First verify the icons do land on
  one atlas page (the picture baker's pages, GpuPictureBaker), not one texture each. Beyond it, by
  design and for when a measurement asks: a box, an icon and a label alternate three pipelines or
  textures in draw order, and every clip push closes a command.

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

- **Players rebind their controls** (user 2026-10-12; found writing the input docs): a player's
  rebinding has a home, `InputBindingOverrides` in the user settings (`Foundation/Input/
  InputMap.cppm`), but nothing outside its tests reads or writes it, so a game cannot offer
  rebinding. Apply the overrides when the input map binds at startup, and give scripts what a
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
  style conversion for real specular maps. The FBX half is now `Specs/phong-material.md`: legacy
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
- **Pausing a run from the Game tab stops its clock too** (user 2026-10-26, playing Sky Hopper in the
  editor): with the run paused from the Game tab's bar, the level clock on Sky Hopper's HUD keeps
  counting. Not looked into yet: whether the clock's script reads time that a paused scene still
  advances, or the HUD's canvas runs apart from the scene it shows. A paused run should hold every
  clock a game reads.
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

- **Documentation cleaned up and made proper, for the 0.1 release** (user 2026-10-09): the
  documentation as a user of the engine meets it, current and organised (getting started, the
  editor, each system, scripting, exporting, the MCP guide). Ruled 2026-10-11: it is written
  fresh in its own repository, `AssiduousEngine/docs` (Astro Starlight, private source, published
  to the public `docs-site` at https://assiduousengine.github.io/docs-site/), from the DeepWiki
  page, corrected against `Documentation/` and the source; this repository's `Documentation/`
  stays as the engine's development documentation. Started 2026-10-11: the home page and the
  introduction, with the outline in its README.
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
  The items marked for 0.1 above (the documentation and the fifth sample below) belong to it.
- **A fifth sample game, on networking and crowds, before 0.1** (user 2026-10-09): no sample
  project uses the network stack or crowds yet, so neither has been showcased or stressed in a
  game. The fifth sample in `Data/SampleProjects/` built around both: players over the network
  (the engine's reliable UDP and replication) and a crowd (instanced skinning, navigation
  crowds), built and playtested through the MCP tools as the others were. (Lamplight's crowd
  was set aside "for later" when it was planned.)
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
