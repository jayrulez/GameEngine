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

- **Move the non-scene pages out of Editor.Scene into their domains** (user 2026-10-16): Editor.Scene
  holds pages of other domains beside the scene page: AnimationClipPage, AnimationGraphPage and
  SkeletonPage (animation), MaterialPage (materials), MeshPage (meshes and models),
  ParticleEffectPage (particles) and SettingsProfilePage (render settings profiles); GamePage and
  PieInstancePage (play in editor) belong to neither the scene nor an asset domain. Each goes to
  its domain's editor library, as the audio, font, texture and shader pages already live in
  Editor.Audio, Editor.Fonts, Editor.Texture and Editor.Shaders, making those libraries where none
  exists (an Editor.Animation, an Editor.Materials, ...), so Editor.Scene keeps the scene page and
  what scenes share, and a domain's page builds and is tested with its domain. Decide where play
  in editor lives.
- **An icon set for the editor** (UI survey 2026-10-14): the clearest visible gap against
  LunarSong's editor is icons, not effects: it ships 377 SVG icons; we have about 34 editor icons
  and 10 window-chrome glyphs, so our toolbars and menus are mostly text. Draw or license a
  consistent set (check the licence) and use it in the toolbar, menus, the asset browser and the
  hierarchy.
- **The editor theme gives nothing elevation** (UI survey 2026-10-14): RoundedDark puts no shadow
  on popups, menus or floating panels, though the framework draws a good analytic shadow. Add
  elevation to what floats.

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
  shadow is filtered by a fixed 3x3 PCF grid (`SampleCascade` in `Data/Shaders/lighting.hlsli`),
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
  (R), roughness (G) and metalness (B), so all of its maps are dropped. Wanted: an import option to
  read that slot as packed ORM (not the default: a true Phong specular map would turn to metal),
  and the spec-gloss style conversion for real specular maps. Test against Bistro v5.2 (FBX only)
  at `~/Downloads/Bistro_v5_2`.
- **A slot's own depth and shadow program.** The forward pass draws each submesh with its slot's
  material (2026-10-15), but the depth prepass, shadows and picking still draw a mesh (and an
  instanced set) whole with its first material (`MeshRenderer::DepthLikeFor` over `md.material`):
  a custom shader that moves its vertices or cuts out, on any slot but the first, casts and
  occludes as if it did neither. Wanted: per-submesh depth draws where a slot's material has its
  own depth program, wind, or a cutout.

### UI framework: gaps and bugs found by the UI survey (2026-10-14)

A survey of ours against LunarSong, RmlUi, AUI and eepp (text and fonts left out) found that
most of the polish gap is in how our samples and themes use the framework, but these are real
gaps in it, ranked by how much polish they cost, and two bugs.

- **Bug: a shadow takes its radius from `corner-radius`, not from the background's radius**
  (`View.cppm:1308`): Sky Hopper's `.cloud` and `.hud` cards (`GameTheme.sss`) round their
  background with `radius=24`/`20` but set no `corner-radius`, so they cast square shadows. The
  shadow should follow the shape it sits under.
- **Bug: the sheet's `opacity` does nothing**: `StyleProperty::Opacity` is parsed and classified
  (`SSSParser.cppm:1089`, `StyleProperty.cppm`) and nothing applies it to a view. Fixed on branch
  `vg` (b7ad83366).
- **UI blends in linear light, with no sRGB mode**: vertex colours are decoded to linear and the
  target is `BGRA8UnormSrgb` (`vg.vs.hlsl`; `Renderer.cppm`), so translucent panels, scrims and
  shadows read fainter than the values a designer writes (half-black over white comes out about
  73% grey, not 50%), and gradients brighten at their middles. Offer an sRGB-encoded blend for UI
  canvases (the default), keeping linear for in-world panels that should light like the scene;
  LunarSong's `UITargetSpace` is a reference.
- **Game UI at a reference resolution is scaled as a transform** (`Engine.UI/UISubsystemImpl.cpp`,
  the canvas scaler sets `Transform.Scale`): layout runs in reference pixels, so the device-pixel
  rounding the DPI path does never sees the real scale, and rounded rects and strokes land on
  fractional pixels (a 2 px border at 0.48 scale is a soft 0.96 px line). Feed the scale to
  layout, as DPI is, instead.
- **No rounded clipping in views**: VG has stencil path clipping (`VG/Context.cppm`,
  `PushClipPath`, one level deep), but no view uses it, so a rounded card cannot hold an image, a
  header bar or a scrolled list without its corners leaking. Clip to the rounded box, nested.
- **Sheet gradients**: `gradient()` takes two colours and four directions and ignores the corner
  radius (`SSSParser.cppm:1433`). Stops, angles, radial, and the box's rounding.
- **No offscreen layers in VG**: opacity is per vertex (`VG/Context.cppm`), so a fading card shows
  its children through each other and state cross-fades double-blend translucent backgrounds; and
  there is no backdrop blur or filter (desaturate, glow). One layer seam serves all three. Built on
  branch `vg` (V3): group opacity, `filter: blur()`, `backdrop-filter: blur()`; desaturate and glow
  remain.
- **Motion in sheets**: no `transform` property (rotate, skew, scale), no keyframes, and only
  quadratic easings (no `cubic-bezier`), so hover lifts, press squashes and focus pulses need
  script.
- **Box paint**: one shadow per box; no per-side borders or border styles; `var()` cannot reach
  inside a drawable's arguments.
- **Baseline alignment in FlexLayout** (user 2026-10-16; found by the GUI lab assessments): markup
  takes `align-items="baseline"` and `align-self="baseline"` (`MarkupRegistry.cppm:397`, `:683`)
  and `Align::Baseline` exists, but FlexLayout treats it as `start` (`FlexLayout.cppm:500`), so a
  row of mixed text sizes asking for baseline alignment is top-aligned instead. Views need to
  report a baseline (a text view its first line's, others the bottom of their content box) for
  the cross-axis placement to use. The GUI lab has no baseline alignment yet either; its layout
  design covers it.
- **To check visually**: the fill is drawn over the full bounds and the border stroked inset on
  top (`RoundedRectDrawable.cppm`), which may leave a faint fill-coloured halo outside a thin or
  translucent border at the corners.

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
