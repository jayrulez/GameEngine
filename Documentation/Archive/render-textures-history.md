# Render textures (cameras that render into a texture, and UI that shows one)

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: PROPOSED 2026-10-01, to be built on the `render-textures` branch. Sized L (P0-P4).
> Origin: PaperKid's rebuild adds a minimap (the original spec ruled one out), and a minimap needs
> the scene drawn from a second camera into something the HUD shows. The design came out of a
> discussion with the user, whose rulings shape it: the UI layer knows nothing about cameras, a
> camera knows nothing about UI views, and nothing maps one to the other. Both sides reference
> the same texture. Every decision below is language-neutral and names the seam it lands on, so
> Sedulous can follow it one for one. Read CONVENTIONS.md first.

## Goal

An author makes a render texture asset (a size and a format), points a camera's target at it, and
points a UI image at it. The camera renders the scene into the texture every frame (or every Nth),
and the image shows it. The same texture works anywhere a texture does: a sprite, a decal, a
material slot. With an orthographic camera looking straight down, that is a minimap; with a
perspective one, a security monitor, a rear-view mirror or a picture-in-picture.

Along the way the game UI learns to show any texture asset as an image, which every HUD icon
needs and which game UI cannot do today.

## Not goals

- Culling layers (a camera that sees only some entities). Minimap markers are UI views placed by
  script over the image, not world geometry only the minimap camera sees. A later spec.
- A camera that renders the game UI into its target. UI already renders into textures through the
  `RenderTexture` canvas mode.
- Render textures that resize at run time, MSAA targets, cube or array targets (reflection probes
  already capture cubes), depth-only targets, reading a target back on the CPU.
- One texture shown in two windows at once (the editor's undocked Game tab is a separate host).

## What exists (checked, cited)

**Cameras.**
- `CameraComponent` (`Code/Engine/Engine.Render/RenderComponents.cppm:195-203`): `fovYRadians`,
  `aspect`, `nearZ`, `farZ`, `clearColor`, `primary`. Perspective only; no target.
- The camera used to render is picked by `ExtractPrimaryCamera`
  (`Code/Engine/Engine.Render/ExtractImpl.cpp:381-405`): the first camera with `primary` set whose
  entity is effectively active. It builds `PerspectiveFovRH(fov, aspect > 0 ? aspect : cam.aspect,
  ...)` (:398). The aspect passed in comes from the scene size, the viewport or the target
  (`RenderSubsystemImpl.cpp:375-398`); `cam.aspect` is only a fallback.
- The editor's camera preview duplicates that projection in `BuildCameraPreviewOverride`
  (`Code/Editor/Editor.Scene/CameraPreview.cppm:33-46`), using the component's own aspect.
- `Float4x4::OrthographicRH` exists (`Code/Foundation/Core/Math/Float4x4.cppm:123`) and is used by
  shadows (`Code/Foundation/Render/ShadowSystem.cppm:119`) and editor thumbnails
  (`Code/Editor/Editor.Preview/ThumbnailStageImpl.cpp:383`). No camera uses it.
- Script reaches the camera through reflection: `REFLECT_VALUE(CameraComponent)` with `of` and
  its fields (`Code/Engine/Engine.Render/RenderComponentsImpl.cpp:102-116`).

**Rendering into a texture.**
- `RenderScene` (`Code/Foundation/Render.Api/RenderApi.cppm:311-332`) renders into any
  `rhi::TextureView`, with an optional `CameraOverride` (:101, a `ViewCamera` and a clear color)
  and a `TargetState` (:179) that, for an offscreen target, names the texture so the render graph
  adds the barriers and leaves it in `ShaderRead`.
- The editor already renders a second camera this way: `SceneEditorPage::RenderCameraPreview`
  (`Code/Editor/Editor.Scene/ScenePageImpl.cpp:1868-1911`) renders the selected camera into an
  offscreen target ending in `ShaderRead`. That is this spec's mechanism, run by the engine for
  authored cameras instead of by an editor page.
- Scene extraction runs once per scene per frame and is shared by all of that scene's views
  (`RenderSubsystemImpl.cpp:357-359`, :400-411), so an extra view of the same scene does not
  extract twice.
- TAA and auto-exposure history are indexed by the ORDER of `RenderScene` calls in the frame, not
  by any key (`PipelineImpl.cpp:1751`; `TaaPass.cppm:42`, :116; `ExposurePass.cppm:35`, :109,
  `m_views[viewIndex % kMaxViews]` with `kMaxViews = 8`). Adding views changes later views'
  indices; see Decision 4.
- The shipped player's frame (`Code/Engine/Engine.DefaultApp/DefaultApplicationImpl.cpp`):
  `RenderCanvasTextures` (:709-712), `BeginRendering` (:713), `RenderScene` per active scene into
  the backbuffer (:728, or :747 at a fixed render resolution), `EndRendering` (:752, where
  scene-tier UI draws), `RenderOverlays` for the screen-tier UI (:756).

**Textures.**
- `texture::Texture` (`Code/Foundation/Texture.Resource/TextureResource.cppm:75`) owns a GPU
  texture, view and sampler; `Adopt(device, texture, view, sampler, w, h, format)` (:101) takes
  ownership of ones made elsewhere, and `Uid()` (:115) changes on every adopt. Its factory is
  `FactoryWithService<Texture, TextureResource, TextureFactory, rhi::Device>` (:458), listed in
  `RenderDomain` (`RenderSubsystemImpl.cpp:1025`).
- Materials bind textures by view: `Material::SetDefaultTexture(name, rhi::TextureView*)`
  (`Code/Foundation/Materials/Material.cppm:134`), filled from `Proxy<texture::Texture>` by the
  material factory (`MaterialResource.cppm:267-301`); `MaterialInstance::SetTexture` overrides one
  at run time (`MaterialInstance.cppm:112`).
- The precedent for a runtime-only pointer beside a serialized asset ref:
  `SpriteComponent::texture` (runtime, "wins over textureAsset") and `textureAsset`
  (`RenderComponents.cppm:247-251`); extraction takes the runtime view first
  (`ExtractImpl.cpp:302-310`); `Serialize` writes only the asset ref (`RenderComponents.cppm:400-411`).
  `DecalComponent` does the same (:264-267).
- The `RenderTexture` canvas mode hands an engine-owned texture to a renderer consumer by writing
  that same runtime field (`UISubsystemImpl.cpp:1767-1815`, "so the render subsystem stays
  UI-unaware").

**UI images.**
- `ImageView::SetImage(const image::ImageData*)` (`Code/Foundation/UI/Controls/ImageView.cppm:54`)
  draws through `VG().DrawImage` (:90-110).
- `VGRenderer::RegisterExternalTexture(const ImageData* key, rhi::TextureView*)`
  (`Code/Foundation/VG.Renderer/Renderer.cppm:556-586`) makes `DrawImage(key)` sample a GPU texture
  the caller owns; the key is an `image::ImageDataRef` with a size and no pixels
  (`Code/Foundation/Image/ImageData.cppm:117-127`). `ViewportView` uses exactly this.
- Registration is per renderer, and Engine.UI has one renderer per (format, stencil, sample count),
  created lazily (`UISubsystemImpl.cpp:180-189`, `RendererFor` :429-470), shared by the scene tier,
  the screen tier and the canvases (canvases draw in RGBA8UnormSrgb, :1740). A texture registered
  on one renderer is invisible to the others.
- Game markup registers `ImageView` with no attributes (`MarkupRegistry.cppm:1187`), and a
  property setter is `void(*)(View*, StringView)` (:72), with nothing to resolve an asset with.
  Engine.UI never gives an `ImageView` an image.
- The UI's own seam for loading images is `IResourceProvider::LoadImage(path)` returning a
  borrowed `ImageData*` (`Code/Foundation/UI/Styling/Parser/IResourceProvider.cppm`). Stylesheets'
  `image()` and `@image` use it (`SSSParser.cppm:342-371`), but Engine.UI's `StyleSheetLoader` sets
  no provider (`UISubsystemImpl.cpp:752`, :2132), so they do nothing in game UI today.
- Script UI handles (`Code/Engine/Engine.UI.Script/UiScriptTypes.cppm`): View, Label, Button,
  ProgressBar, Slider, TextBox, ViewGroup, Screen. No image handle.

## Design

### Decision 1: a render texture is a texture

`RenderTextureAsset` is a settings-only asset (`width`, `height`, `format`), cooked like the input
map (`Code/Pipeline/Input.Pipeline/InputMapAsset.cppm`: asset, `DefaultAssetBuilder`, a File > New
creator). Its runtime product is an ordinary `texture::Texture`: the factory creates a GPU texture
with render-target and sampled usage (plus CopySrc, as `ViewportView` found it needs for
captures) and `Adopt`s it. So everything that takes a texture already takes a render texture:
`SpriteComponent.textureAsset`, decals, material slots, and the UI image below. No consumer learns
a new type.

Formats: `Ldr` (RGBA8UnormSrgb, the default: what a tonemapped view writes and what UI samples)
and `Hdr` (RGBA16Float, for a material that wants the linear values). The size is fixed by the
asset; P0 does not resize.

The factory lives beside `TextureFactory` in `foundation.texture.resource`, registered in
`RenderDomain` the same way, since it needs the device.

### Decision 2: a camera renders into a target texture

`CameraComponent` gains:

- `target`: a `Ref<texture::Texture>` (serialized, picked in the inspector). When set, the camera
  renders into that texture instead of the screen. A texture made at run time is assigned to the
  same Ref: a `Ref` already holds a direct object that wins over its id and is never saved
  (`ResourceModule.cppm`, "Code-created resources ... assign a RefPtr<T> directly"), so no
  separate runtime pointer is needed.
- `targetInterval` (frames, default 1): render every Nth frame. A minimap at 30 Hz on the Deck.

The camera never learns what shows the texture.

**Selection.** `ExtractPrimaryCamera` skips cameras with a target: a targeted camera is never the
screen camera, even if `primary` is set. Every other effectively-active camera with a target is a
target camera.

**When they render.** Inside `RenderSubsystem`, the first `RenderScene` of a scene in a frame,
right after it extracts the scene (so the snapshot's view origin stays the asking view's camera),
renders that scene's target cameras, each into its texture, through the same `RenderScene` path
with a `CameraOverride` and a `TargetState` from and to `ShaderRead` (what the camera preview
does). Which cameras are due, and with what camera, is the pure `CollectTargetCameras`
(Extract), which the tests drive. A target camera's view draws neither the scene's debug lines
nor the scene-tier overlays (HUD canvases, billboards: `ViewSettings::sceneOverlays`), so a
minimap shows the world without the HUD it sits in. A per-frame set of scenes already done stops the editor's second view of a
scene (the camera preview, a second Game tab) from rendering them twice. Hosts change nothing:
the player, the Game tab and the scene page all get target cameras without a new call. Because
they record before the scene's main view, anything sampling the texture in that view sees this
frame's image.

**Aspect.** A target camera's aspect is its target's width over height.

### Decision 3: an orthographic projection

`CameraComponent` gains `projection` (`Perspective`, the default, or `Orthographic`) and
`orthoHeight` (the world-space height the view spans; the width follows the aspect). One helper,
`MakeCameraProjection(const CameraComponent&, f32 aspect)`, builds the matrix for
`ExtractPrimaryCamera`, the target cameras and `BuildCameraPreviewOverride`, which today
duplicates the perspective math. A minimap camera is then an ordinary entity: orthographic,
pointing down, with a target, moved by a script over the player.

### Decision 4: view order and history

TAA and auto exposure keep history per view index. Target cameras render first, in a stable order
(the order the camera manager iterates, which is stable for a loaded scene), so a scene with a
fixed set of target cameras gives every view the same index every frame. Target cameras render
with `ViewPostOverride{disablePost = true}` by default (no TAA, bloom, AO or SSR; exposure and
tonemap stay, so the image still displays), which makes a minimap cheap and keeps it out of the
TAA history altogether.

Known edge: a target camera that becomes active or inactive shifts the indices of the views after
it for one frame. `kMaxViews` is 8. Keying the history by view instead of by order is the proper
fix and a separate change; this spec documents the edge rather than taking it on.

### Decision 5: game UI shows texture assets through the resource provider

The UI already has the seam: `IResourceProvider::LoadImage(path)` returns a borrowed `ImageData*`.

- **Core UI.** `ImageView` gains a `Source` string property, set by a `source` markup attribute.
  When the view is measured or drawn with a source in a context that has a provider, it asks the
  provider once; until the provider answers (an asset still loading) it shows nothing and asks
  again next frame. `UIContext` gains `SetResourceProvider` / `ResourceProvider()`. The UI never
  interprets the string.
- **Engine.UI** implements the provider. A source is an asset guid, bare or in braces.
  `LoadImage` binds it to a `texture::Texture` through the resource manager the application gives
  the subsystem (`UISubsystem::SetResourceManager`, called by `DefaultApplication` wherever it
  takes a manager) and returns one `ImageDataRef` key per texture, of the texture's size, kept
  for the subsystem's life (views hold the pointer). Before a VG renderer draws, the keys are
  registered on it, once per change rather than per frame: a new key, a reloaded texture
  (`Uid()` changes), a renderer of another format drawing for the first time. A new resource
  manager drops every binding; the keys stay valid and show nothing. The same provider goes to
  the `StyleSheetLoader`, so stylesheet `image()` works with assets too.
- The editor's UI document page gets a texture picker for `source` the way other asset-typed
  fields do; the MCP document tools carry it as text.

`ImageView`'s `ScaleType` and `Tint` already cover fitting and tinting.

### Decision 6: script access

- Camera fields come through `REFLECT_VALUE(CameraComponent)` as today: `projection`,
  `orthoHeight` and `targetInterval`. The script backends bind no texture-reference field, so
  `target` is set the way `setMesh`/`setMaterial` set theirs: `SceneRender.of(scene)
  .setCameraTarget(camera, textureId)`, bound through the run's resource manager; a nil id
  clears it.
- A UI `Image` handle (`UiScriptTypes.cppm`) with a `findImage` finder and a `source` property
  (a Guid; `setSource` swaps it, a nil id clears it, and so does its image).
- Markers on a minimap are views in the HUD whose position a script sets. That turned out to need
  one addition (found building PaperKid, 2026-10-02): the handles had no way to move a view, so
  every handle gained `setTranslation(x, y)` and `setRotation(degrees)` over the view's post-layout
  transform (Systems/game-ui.md).
  With an orthographic top-down camera, world to map is a scale and an offset; the spec's sample
  includes that helper in the game script, not the engine.

## Phases

Each phase is a set of layer commits with tests, built on clang and gcc, on the branch.

**P0 - Orthographic projection.** `projection` and `orthoHeight` on `CameraComponent` with
serialization and reflection; `MakeCameraProjection` used by `ExtractPrimaryCamera` and
`BuildCameraPreviewOverride`. Tests: the helper's matrices for both modes against
`PerspectiveFovRH` / `OrthographicRH`; a scene round-trips the new fields; an older scene without
them reads as perspective.

**P1 - The render texture asset.** `RenderTextureAsset` (pipeline), its cook, its File > New
creator, and the factory that makes a render-target `texture::Texture`. Tests: the asset cooks and
round-trips; invalid sizes are refused at cook; the factory's texture has the size, format and
usages asked for (on the test device the render tests already use).

**P2 - Target cameras.** `target`, the runtime view, `targetInterval`; `ExtractPrimaryCamera`
skips targeted cameras; `RenderScene` renders a scene's target cameras once per frame before its
main view. Tests: camera selection (a targeted primary is not the screen camera; inactive target
cameras are skipped; the interval is honoured); the once-per-frame guard with two views of one
scene; a rendered target holds the clear color of its camera (the existing offscreen render
tests' readback, if the test device supports it; otherwise an Integration test).

**P3 - Images from textures in game UI.** `ImageView.Source`, the `source` markup attribute,
`UIContext`'s provider; Engine.UI's provider with registration across all renderers; stylesheet
images through the same provider. Tests: core UI asks the provider once per source and draws the
returned image; Engine.UI's provider registers on renderers created before and after the load and
unregisters on release; a stylesheet `image()` with an asset guid resolves.

**P4 - Script and tooling.** The `Image` script handle and finder; the camera's new reflected
fields; McpGuide and Scripting.md updated; the inspector shows the fields (reflection). Tests: an
AngelScript and a Luau script set an image's source and a camera's target.

**Acceptance.** A test scene with an orthographic camera targeting a 256x256 render texture, and a
HUD document with `<ImageView source="{that texture}"/>`, shows the top-down view live in the
player and in the Game tab, and the same texture on a sprite in the scene shows the same image.

## PaperKid's minimap (the motivating use, built in the PaperKid rebuild, not here)

- `Minimap.xasset`: a render texture, 256x256, `Ldr`.
- A `MinimapCamera` entity: orthographic, `orthoHeight` the block's size, rotated to look straight
  down, `target` = Minimap, `targetInterval` 2.
- `hud.sml`: `<ImageView id="minimap" source="{Minimap guid}"/>` in a corner, with marker views
  (the bike arrow, subscriber dots) over it.
- The game script moves the camera over the bike each frame and places the markers by the same
  scale and offset.

## Gotchas

- A target texture is written and sampled in the same frame on one queue; the render graph's
  barriers (through `TargetState`) order it, as they do for the camera preview. Do not sample a
  target camera's texture from a view recorded before that camera.
- Releasing a render texture while a UI renderer still has it registered: unregister first (the
  `ViewportView` rule, `Renderer.cppm:547`).
- A render texture in `Hdr` shown by the UI displays linear values unscaled. The UI is for `Ldr`.
- Sedulous: the same seams exist there (`ViewportView.bf` is where ours came from), but none of
  this spec's pieces do; port it phase by phase.
