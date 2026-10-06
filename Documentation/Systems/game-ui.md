# Game UI

> Status: CURRENT
> Verified: 2026-08-12 @ 3364be29
> Track: [[game-ui-subsystem]] / [[game-ui-p1-progress]]

The game-facing UI runtime: `foundation.ui`'s retained-mode control framework driven as an engine
subsystem, showing UI three ways (screen HUD/menus, world-anchored billboards, world-space panels) as
cooked document + theme assets, with actions-only input arbitration and a script facade. Shipped end
to end (P1-P3 + the overlay-roles refactor + split-screen + the world tier).

## Modules

- **Framework** (`foundation.ui` + `.toolkit` / `.runtime` / `.viewport` / `.shell` / `.vfs` /
  `.application`) - the retained-mode UI stack (`.sml` markup + `.sss` styling loaders, VG renderer,
  themes). Shared with the editor's UIHost and UISandbox: ONE host implementation.
- **`foundation.ui.resource`** (`Code/Foundation/UI.Resource`) - the cooked `UIDocument` + `UITheme`
  resources + factories.
- **`ui.pipeline`** (`Code/Pipeline/UI.Pipeline`) - `UIDocumentAsset` / `UIThemeAsset` + builders (cook
  = validate markup/styling + write-through, recording image/font dependency edges).
- **`engine.ui`** (`Code/Engine/Engine.UI`) - `UISubsystem`, the canvas / billboard / world-panel
  components, the overlay-role bridges, the consumption mask, `GameTheme`, and the UI script facade.
- **`editor.gameui`** (`Code/Editor/Editor.GameUI`) - `UIDocumentPage` (text pane + live preview
  rendered through the runtime context).

## Tiers + components

All rendered through the SAME `UIContext`/root/VG machinery, no parallel host:
- **Screen** - `UICanvasComponent` (`CanvasRenderMode::ScreenOverlay`): menus + HUD, with a document +
  optional theme ref, `order` stacking, and a `CanvasScaler` (ConstantPixel / ReferenceResolution,
  letterboxed min-fit). A menu is a prefab (spawn/despawn = open/close); documents can also be pushed
  scene-less onto the screen tier (`PushScreenOverlay`, topmost, survives scene swaps).
- **Billboards** - `UIBillboardComponent`: a small document anchored to the entity's projected screen
  position (offset + distance scaling), batched into one VG draw (ported from the Sedulous reference,
  its best-behaved piece).
- **World panels** - `UIWorldPanelComponent`: an RT-quad panel (per-panel offscreen target keyed
  (scene, entity, kind)) driving an auto-managed sibling `SpriteComponent` with
  `SpriteOrientation::EntityOriented` (quad on the entity's right/up axes), sized in pixels-per-meter,
  unlit, INTERACTIVE (camera-ray -> plane -> UV -> pointer injection). Being ordinary scene content it
  gets depth / occlusion / TAA / post correct by construction.

## Overlay roles + rendering

The render layer coordinates overlays two-tier (the a78ec94 refactor): **`ISceneOverlay`** (per-view,
drawn inside the compose after post / before debug draw, matched by SceneKey, given the view's real
camera) + **`IScreenOverlay`/`IScreenRenderer`** (window-space registry; hosts make one generic
RenderOverlays call per target). `UISubsystem` implements both: per-scene roots (billboards + canvases;
scene isolation is structural; billboards project per view - correct in editor viewports/camera
previews) plus the scene-less screen root. Split-screen: the VG renderer has a sub-rect Render overload
(viewport offset + scissors clamped to the rect), so scene HUDs lay out per half. The VG ring resets
once per UI frame. A `RenderTexture` canvas mode renders a canvas into an offscreen target (in-world
screens; UI as a sprite/decal texture override).

## Input

Actions-only arbitration (no `IsMouseOverUI` polling). UI dispatches first (screen canvases by order ->
billboards -> world panels), then `ActionRuntime::SetConsumptionMask` mutes the matched input CLASSES
(pointer / keyboard / text - SEPARATE, so a menu eating the mouse never mutes gamepad movement) for
gameplay that frame; raw device facades stay unfiltered. Per-surface scene binding
(`SetSourceProvider(provider, sceneKey)` + `UnboundInputScenePolicy`) resolves the active input root
(occupied screen tier = modal > pointer-hit root > first scene root with content for pad-only nav);
GamePage binds on Play, the editor's embedded context runs ScreenTierOnly (HUDs are WYSIWYG but not
interactive while editing). Gamepad navigation (dpad/stick MoveFocus + hold-repeat, South = activate,
East = escape; pad is NOT a consumption class) and keyboard/text input (IME via the shell) are wired.
Play-in-editor works by construction (the same event stream through the viewport `InputSurface`).

## Resources + script

- **`UIDocument`** (cooked) - a validated view-tree payload (markup text v1; the cook fails on
  unknown types/properties); the factory instantiates a fresh tree per canvas (documents are
  templates). Hot reload rebuilds on resource reload.
- **`UITheme`** (cooked) - a validated `.sss` payload; a project `defaultUiThemeId` (manifest v5,
  shared with audio's bus layout) selects it, per-canvas override allowed, `GameTheme`/`GameLightTheme`
  as built-in fallbacks. Swappable-theme-asset model (Godot) over our SSS.
- **UI facade** - addresses a control by its authored `id`: `setText` / `setProgress` / `setVisible`,
  and `onClick` binds a script delegate, resolved through the live screen tier + resource manager.
  Every handle moves and turns its view with `setTranslation(x, y)` (pixels from where layout put
  it) and `setRotation(degrees)`, the view's post-layout transform, so a minimap marker or a
  compass needle moves each frame without a relayout; `translation` and `rotation` read it back.
- **Tweens** - every handle animates on the UI frame clock, which runs while the game is paused:
  `fadeTo(opacity, seconds)`, `moveTo(x, y, seconds)`, `scaleTo(scale, seconds)` (`setScale` and
  `scale` for the instant form), `rotateTo(degrees, seconds)`, each with an optional `Ease`
  (`Linear`, `In`, `Out`, `InOut` the default, `OutBack`, `OutBounce`, `OutElastic`), and
  `pulse(peak, seconds)`, out to `peak` times the normal size and back (a counter that changed).
  Each property tweens on its own channel: a new tween of one property replaces the running one of
  that property only, so a score can rise, fade and swell at once, and a `set` stops only its own
  property's tween. Zero seconds, or a view in no tree yet, is a set.
- **Vector images** - an `.svg` imports as a `UIVectorImage` asset (linked in `Sources/`, validated
  at cook by the same SVG loader that draws it: the icon-systems subset, paths and basic shapes with
  fills and strokes). A game theme names one with `@icon heart "{guid}"` and draws it with
  `svg(heart, tint=#E53935)` wherever a drawable goes, e.g. `.lives-icon { background: svg(heart); }`
  on a sized `<Panel class="lives-icon" width="28" height="28"/>`. The theme's cook embeds every icon
  it names (`UIThemeSource::icons`, a `reads` edge so a changed SVG recooks the theme), so the sheet
  parses at runtime with nothing else to load; an `@icon` naming no vector image fails the cook.
  The editor's theme page previews the edited sheet the same way: its parse reads an `@icon`'s
  vector image through the editor's resource manager and textures through the game UI's provider
  (`ThemePreviewResources`), so icons and images show before the sheet is cooked.
- **Images** - `<ImageView source="{guid}"/>` shows a texture asset: the subsystem is its context's
  `IResourceProvider` and binds the id through the application's resource manager, one image key per
  texture, registered on every VG renderer before it draws. A render texture a camera targets is a
  texture too, so a HUD shows a live minimap or monitor (Specs/render-textures.md). Script:
  `ui::findImage(id).setSource(textureId)`, and `SceneRender.of(scene).setCameraTarget(camera,
  textureId)` on the render side. `corner-radius="8"` (or four values, top-left first) rounds the
  picture's corners.
- **Picture buttons** - a `<ContentButton id="card">` holds one element as its face (a Flex with an
  `ImageView` and labels, say): the button draws its style's background behind it, and
  `ui::findButton("card").onClick(...)` and `findLabel` reach it and the labels inside.

## Locked decisions

Core UI only (never the toolkit - that is editor tooling); one `UIContext` owned by the subsystem with
the theme on the context; actions-only input gating with separate consumption classes; the VG renderer
is OFF-LIMITS without a consult (game-UI needs no VG changes). UI updates with UNSCALED dt (menus
animate while the game is paused).

## Deferred

World-tier direct-draw mode (option A, VG-consult-gated), dirty-gated panel redraws, atlas packing +
panel MIP chains, declarative markup bindings (`onClick="game.resume"`), theme variations beyond the
built-ins, UIDocumentPage per-line diagnostics (the page already uses `CodeEditView`), the
two-interactive-scenes routing edge, and the
toolkit test tail: `Documentation/Backlog/game-ui-followups.md`.

---

Design rationale (the reference survey - Sedulous / Flax / Godot / Traktor Spark - the Sedulous.Engine.UI
deep-read tier table, the locked-decision reasoning, the world-tier A-vs-B decision) is in
`Documentation/Archive/game-ui-design-history.md`.
