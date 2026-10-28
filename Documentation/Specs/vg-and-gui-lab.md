# VG and text first, then the experimental GUI as a scriptable lab

> STATUS: APPROVED 2026-10-14 (user direction: "fix VG first"; keep the experimental GUI alive as a
> smaller experimental UI framework, improve its shortfalls, build it on our RTTI so it is
> scriptable, and prove things there before they move into `foundation.ui`). Starts once
> Masquerade is finished, all its phases (user 2026-10-14). The F line (text: runs, shaping,
> effects) added 2026-10-15, after VG and before the lab (user: "before lab work"). Read
> CONVENTIONS.md first.

## Why

A survey of our UI against LunarSong, RmlUi, AUI and eepp (2026-10-14; the gaps are in
`Plans/backlog-0.1.md`, "UI framework") found most of the polish gap in how our samples and themes
use the framework, but the deepest limits are in the drawing back end, VG, which both
`foundation.ui` and the experimental GUI draw through: it blends in linear light, clips to one
path at a time, has no offscreen layers, and draws a rounded box as three tessellated passes. So VG
comes first: whatever is fixed there lifts both frameworks.

The experimental GUI (`Code/Experimental/GUI`, eepp-derived, parked since 2026-09) has a better
style front end than `foundation.ui` (a real CSS cascade, `@media`, `@keyframes`, custom
properties, nesting, radial gradients, node transforms, a model/view layer) but applies only about
30 properties, animates only opacity (easing parsed and ignored), and is not scriptable (widgets
carry RTTI, no reflected properties). It becomes the lab: where the CSS-shaped model, effects,
animation, and a UI that is scriptable from the core are proven, then moved into `foundation.ui`
a piece at a time.

## References

GPUI, Zed's UI framework, and its community fork GPUI-CE were surveyed on 2026-10-15 (clones at
`~/Dev/CPP/zed`, `crates/gpui*`, and `~/Dev/CPP/gpui-ce`). Their GPUI crates are Apache-2.0: code and
shaders may be adapted with the licence text kept beside them and the attribution credited (two
snippets inside carry MIT notices of their own). Zed's component crate (`crates/ui`) is
GPL-3.0-or-later, a design reference only. Where a phase below names GPUI, that is what it takes.

## Part 1: VG

Facts first (read 2026-10-14): VG's fills already include multi-stop linear, radial and conic
gradients with spread modes (`VG/Fills.cppm`); the renderer targets `BGRA8UnormSrgb` and decodes
vertex colours to linear (`VG.Renderer/Renderer.cppm`, `vg.vs.hlsl`); `PushClipPath` clips by
stencil one level deep (`VG/Context.cppm`); opacity is per vertex; rounded rects are tessellated
with a 0.75 px fringe (`VG/Tessellation.cppm`).

- **V1 Blend space.** A canvas chooses how it composites: Linear (today; right for in-world panels
  lit like the scene) or Encoded (sRGB-encoded values blended as browsers and design tools do; the
  default for UI). Half-black over white is then 50% grey, not 73%, and gradients and cross-fades
  interpolate as authored. Tests: pixel probes of a translucent fill, a gradient midpoint and a
  shadow edge in each space.
  As built on branch `vg` (2026-10-15, the user's choice: a UNORM view of the target, not an
  offscreen layer): the RHI's textures and swapchains may be viewed in their sRGB or UNORM twin
  (Vulkan with VK_KHR_swapchain_mutable_format, the web canvas, DX12 flip buffers; desktop
  wgpu-native says no and falls back); VG's `VGBlendSpace::Encoded` keeps colours as authored and
  encodes what it samples back; the editor's windows draw Encoded. Found on the way: gradient ramps
  are baked from the authored stops, so gradients already ran as authored in both spaces; the
  difference is in translucency. The game's screen UI followed (aa9a00103): the overlay pass draws
  through the target's UNORM view when the host gives one (the player's back buffer, the Game tab's
  viewport, now 8-bit sRGB like the player's window); world panels and render-texture canvases stay
  Linear, lit as part of the scene.
- **V2 Nested clips.** Clip paths nest (an intersection, not a replacement), so a rounded card can
  hold a scrolled list of rounded items. Tests: two nested rounded clips probed at their corners.
  As built on `vg` (afdc172e1): a nested clip writes its winding inside the mask and applies over
  the outer's bounds (the intersection, no new pipeline); a pop replays the clips that remain; a
  clip rect inside a path clip scissors and stencil-tests together. `foundation.ui` does not use
  path clips yet (rounded overflow clipping is G4's, or a later layer here).
- **V3 Layers.** `PushLayer(opacity, blend)` draws a subtree into an offscreen target and
  composites it once, so a group fades as one (no children showing through each other, no
  double-blended cross-fades). Then filters on a layer (blur, desaturate, a colour matrix) and a
  backdrop filter (blur what is behind a panel). Tests: a group of overlapping shapes at 50%
  opacity probed for single blending; a blur's falloff. GPUI-CE has these (offscreen isolation for
  group opacity, `filter: blur` and `backdrop-filter: blur` as a half-size downsample and a separable
  Gaussian); its offscreen target is the whole viewport, so ours sizes a layer to its bounds, for
  the web build's budget.
  As built on `vg`: `PushLayer`/`PopLayer` record a layer's draws apart
  and one composite quad (0654bc70c); `VGRenderer::RenderLayers` draws each layer, innermost first,
  into a target of its own (the renderer's format, sample count and stencil, so clips work inside;
  64-pixel size buckets from a pool per frame slot) before the host's pass, and the composite
  scales the premultiplied target by the opacity through `vg_layer` (0c4977e0c). Layers are a host
  opt-in (`SetLayers`), and past 128 in a batch a layer is an opacity push. `foundation.ui` fades a
  view group through a layer (a leaf keeps the opacity push) and a fading popup (0ee738bf8); the
  editor's windows draw them, and the game's screen UI does through a new step before the shared
  overlay pass, `IScreenOverlay::PrepareOverlay` (7a086d477, 31be678a2). World panels, render
  texture canvases and the scene tier draw inside passes and keep layers off. Fixed on the way: the
  pipeline variant tables were one kind short since V4 (7f179a1d3). Probes: two overlapping opaque
  reds in a half layer read 188 linear and 128 encoded, overlap and not; a half layer in a half
  layer is a quarter; clips inside a layer land in place; the same multisampled.
  Filters (5c60c362c, 0f29861a9): a layer may be blurred (a Gaussian sigma, separable, in the
  canvas's blend space, up to 32), and `DrawBackdropBlur` blurs what the canvas drew behind a
  rounded shape: everything drawn so far in the current target moves into a layer (layers are
  then ordered by depth, not index) whose blurred copy the shape samples. The backdrop is the
  canvas's own drawing, not the scene under a game's UI, which would need the host's target; and
  none is drawn inside a clip path. Sheets reach both as `filter: blur()` and
  `backdrop-filter: blur()`, and the sheet's `opacity` now applies (dc10e620d). The UI sandbox has
  a Layers tab to look at (3cea2ecab). Desaturate and colour matrices are left for a need.
- **V4 An analytic box.** A rounded box with border and shadow drawn as one signed-distance
  primitive (fill, border and shadow from one coverage function), so the three cannot disagree, a
  thin border has no halo, and small radii are exact; multiple shadows and per-side borders follow
  from it. The tessellated path stays for arbitrary paths. Tests: pixel probes against the
  tessellated result; a border with no outer halo. Starting point: GPUI's quad and shadow shaders
  (`zed/crates/gpui_wgpu/src/shaders.wgsl`): per-corner radii, per-side borders whose inner edge is
  a quarter ellipse where neighbouring widths differ, dashed borders, a half-pixel coverage ramp, no
  fringe on a side of zero width, and the analytic rounded-box shadow (an erf blur), inset shadows
  too; adapted under its licence. GPUI draws fill, border and shadow as separate primitives over
  the same snapped bounds; ours keeps them one coverage function where it can.
  As built on `vg` (726dbc5a3, a946cc595): `VGContext::DrawBox` (per-corner radii, per-side borders)
  drawn by `vg_box` (GPUI's coverage and border, credited in ThirdParty/gpui/), box vertices after
  the slice's ordinary ones in the same buffer; both UI hosts turn it on, rounded fills and strokes
  route through it, and the theme's rounded drawable is one box. Shadows stay their own analytic
  primitive (vg_shadow, the same erf idea); multiple shadows and dashed borders are later.
- **V5 Device pixels under scaling.** A scaled canvas (a game UI at a reference resolution) lays out
  at its real scale, as DPI does, so edges land on device pixels; strokes of odd device width snap
  their centres. (This touches `foundation.ui`'s canvas scaler as well as VG.) Tests: a 2 px border
  at 0.48 scale lands on whole device pixels. GPUI's rules are the model: lengths rounded before
  layout, edges rounded in absolute device space after it, a stroke never thinner than one device
  pixel, and a clip that covers whole pixels rather than cutting them.
  As found and built on `vg`: the screen tier already lays a scaled game UI out at its real scale
  (its DpiScale) and VG snaps axis-aligned rects and boxes to device pixels; the box's border widths
  now snap too (whole pixels, at least one). Clips still cut at fractional bounds (scissors round
  inward); left until a case shows it.

Each phase lands with its tests and is used by `foundation.ui` where it applies (its shadow radius
bug and its unused `opacity` are fixed alongside, from the backlog).

## Part 1b: F, text

The same kind of work as VG: underneath both frameworks, so `foundation.ui` gains it at once (user
2026-10-15: "before lab work"). It adopts the text track of `ui-text-and-gaps.md` (proposed
2026-09-23, its decisions kept: the glyph model changes before the shaper does; HarfBuzz is a
backend behind the existing `SetShaperFactory`, not a dependency of Fonts; rich text is a sibling
of `Label`, which keeps its single-run contract and measure cache), with inline runs brought ahead
of bidi, since mixed colour and weight in one line is what the games ask for first. That spec's
phases and gotchas hold; F1 to F3 are its P0 to P2, re-cut.

- **F1 The glyph model.** `GlyphPosition` carries a cluster, a glyph id and offsets; the stb
  shaper fills them degenerately; hit testing, cursor movement and the 21 files that use
  `GlyphPosition` move to clusters. Tests: every text test green with no visual change; cluster
  behaviour pinned for a one-to-one script before any shaper can vary it.
  As built on `vg` (5b35833eb): `GlyphPosition` is {cluster, glyphId, x, y, advance, glyphInfo,
  clusterLength, xOffset, yOffset}. Clusters count codepoints, not bytes (the older spec said
  byte offset but also that stb fills `cluster = stringIndex`, which counted codepoints; editing
  works in characters, and a shaper fed UTF-32 reports clusters in that unit). Hit tests answer
  the cluster and its end, so EditText no longer translates glyphs to characters; every
  `GlyphPosition` user moved, and the whole clang battery is green.
- **F2 Inline runs and rich text.** A `TextDocument` of runs (text, family, size, colour,
  weight, italic, underline) and inline objects (an icon on the baseline), laid out into lines by
  an inline formatter, behind a rich text control beside `Label`. A small markup for it, the same
  from SML and from script (a colour, bold and italic, a size, an icon, a link with an id the
  script hears), and a script surface that sets the markup or the runs: "Your mark wears [a rose
  gown]" in its colour is one label. Tests: a paragraph mixing two sizes and three colours measures
  and wraps; an icon sits on the baseline; a malformed tag is shown as text and logged, never
  dropped; both script backends set runs.
  As built on `vg`: runs and the markup (1684246d5): [b] [i] [u] [color=] [size=] [font=] [link=]
  closed by [/name] or [/], [icon=path], "[[" for a bracket; malformed text kept and reported with
  its offset. The inline formatter (edd929d80) breaks after spaces, shares a baseline across sizes,
  sits icons on it; bold and italic take a "<family> Bold" / "Italic" face when the font service
  has one (`IFontService::HasFamily`, 291bafa65) and are synthesized when not (bold drawn twice,
  italic slanted). `RichText` (c3413fe25) beside Label, in SML as `<RichText markup="...">`, with
  OnLink and a hand pointer over links; scripts get `ui.findRichText` with setMarkup, plainText,
  addRun, and onLink reading `clickedLink` (21a0332c8). The UI sandbox has a Rich Text tab
  (e5fd473a0). Kerning across a run boundary and breaking inside an over-long word are left out.
- **F3 Real shaping.** `Fonts.HarfBuzz` behind the factory, `ITextShaper` widened to a script, a
  language, a direction and a font list, with itemization and fallback inside the shaper; then the
  bidi algorithm for mixed-direction paragraphs. HarfBuzz is not hard-coded anywhere (user
  2026-10-15): it is one implementation of our shaper interface, and the interface evolves to
  carry what shaping needs (script, language, direction, a font list, OpenType features, clusters
  and offsets back) in our own types, designed from what text layout asks rather than from
  HarfBuzz's API. No HarfBuzz type or header crosses it; the stb shaper implements the widened
  interface too and stays a first-class backend (the headless and the minimal builds, and the
  baseline the tests compare against), and another shaper could replace HarfBuzz without touching
  a consumer. Its licence is checked, and its text kept beside
  it, when it is added. Tests (that spec's): Arabic joins and its glyph count differs from its
  codepoints; an `fi` ligature is one glyph over a shared cluster; an RTL paragraph with embedded
  Latin lays out and hit tests; the stb backend still passes F1's suite. The atlas question that
  spec names (bake by glyph id, or by codepoint with a runtime fallback) is decided here.
  Decided 2026-10-15 (the user): atlases key by glyph id. Bakers bake the glyphs a codepoint set
  maps to plus every glyph its substitutions can reach (the closure), cooked fonts carry the font
  tables the shaper reads, the stb shaper writes glyph ids too, and the font data version bumps
  with a migration, so shipped games shape as the editor does. HarfBuzz 14.5.1 is vendored
  (42ff2e55e).
  As built on `vg` so far: atlases answer GlyphFor(codepoint) and look up by glyph id (b66cb7e6e);
  every atlas, baker and cooked font keys by glyph index, the TrueType atlas packing glyph indices
  itself, and every shaper writes them (410ab8e40; FontResource 3); fonts expose their file and
  cooked fonts keep it (46f33ae29); a TextShaperFactory and a shared ClusterTextShaper (1c7bc2499);
  Fonts.HarfBuzz, HarfBuzz behind ITextShaper only (f1fb65f6d); bakers bake what a shaper's
  substitutions reach, through ITextShaper::CollectReachableGlyphs (652a889ec; font builder 8);
  the engine's UI, the editor and the UI sandbox install it (a29edfef9). Then: ITextShaper::Shape
  takes a ShapeRequest (a font list with fallback, a direction, an ISO 15924 script, a BCP 47
  language; 17bdff2ba); VG shapes single-line text through the font's shaper, cached across
  frames (c53fd4dbe); the distance-field cook keeps every baked glyph (620290d46, a bug the game's
  title showed); SheenBidi 3.0.0 is vendored (b76db8a01, Apache-2.0; the project enables C for
  it); the bidi seam (IBidiResolver, ReorderLine for L1/L2) and shapers that lay lines out in
  visual order, with right-to-left carets and hit tests (4854bf5b1); Fonts.Bidi, SheenBidi behind
  IBidiResolver only (fdee57524); the hosts install it (fdd809607). Left: a label measures with
  its font's own MeasureString (unshaped), so a ligated label is laid out a little wide; the basic
  shaper's wrapping knows no bidi; editing right-to-left text (caret movement by arrow keys,
  selection across a direction change) is untested.
- **F4 Text effects.** Outline, shadow and glow per run, from the distance-field fonts the engine
  already bakes (`Fonts.DistanceField.Baker`), so a HUD's text reads over any scene without a
  panel behind it. Tests: an outline's width and a shadow's offset probed in pixels; effects on one
  run of a rich paragraph and not its neighbours.
  As built on `vg`: the distance-field baker writes MTSDF, the true signed distance in alpha beside
  the multi-channel field (f7a6e6776; font builder 10, editor cache 4), so contours past the edge
  round a corner. `VGContext::SetTextEffects` (e7443d4bd) puts a `TextEffects` (outline, shadow,
  glow) on the state; a text draw emits its glyphs once per pass, shadow, glow, outline, then the
  glyphs, each pass carrying two values per vertex (`VGVertex::effect`: the edge moved out and its
  ramp widened, in field units converted from the text's pixels per glyph), and the shader reads
  alpha for those passes and the median for the glyphs. An effect reaches as far as the field
  holds (half its range) and is scaled down past that; a coverage font draws a shadow's offset copy
  only. Probed on Vulkan and WebGPU: a 1.5 px outline adds 1.5 px each side of a 48 px stem and
  leaves the stem, a shadow lands its offset. Markup and sheets (4d56aef7a): `[outline=<w>
  [colour]]`, `[shadow=<x> <y> [blur] [colour]]`, `[glow=<r> [colour]]` per run, the parts a run
  leaves out the control's; RichText draws every run's effects before any run's glyphs
  (`SetTextEffectsOnly`); `text-shadow`, `text-outline` and `text-glow` in sheets go onto the VG
  state as a view draws, reaching its children as CSS's text-shadow does, `none` turning one off.
  A field's range is a load option and a font asset field (3c77244bc and after; 4 texels by
  default), so an effects font bakes a wider field; the UI sandbox's Rich Text tab shows effects
  over bright stripes from a 12-texel Roboto. Left: a coverage font (a raster-ramp asset, the
  sandbox's other faces) draws a shadow's copy only; a glow is a linear ramp, not a Gaussian.

## Part 2: the GUI lab (`Code/Experimental/GUI`)

- **G1 Reflection and script.** Every widget and its style-facing properties reflected through our
  RTTI (`REFLECT_*`), so the engine's reflection-driven script binding (AngelScript and Luau)
  reaches the node tree with no separate UI-script layer: create and find nodes, set properties,
  add classes, listen to events, from either language. Markup, sheets, scripts and an inspector all
  read the same reflected table. Tests: both backends build a small tree, style it and receive a
  click.
  **Handles** (user question 2026-10-14, GPUI's model): the tree owns its elements; what a script or
  a callback holds is a weak handle, a slot index and a generation into the GUI's element table,
  resolved on every access. A destroyed element's handle reads as invalid (`isValid()` false) and
  any other use of it is the script error a removed component's handle raises ("the object is
  gone"), never a silent no-op: one rule for entities, components and UI. A callback a widget holds
  captures the script object weakly, so a script and its widgets never keep each other alive.
  Destroying an element from a script is queued to the end of the UI's update, so nothing is freed
  under a running handler. Creation names its place (`parent.add(...)`), since a detached element
  would have no owner. GPUI does the same with `Entity<T>` and `WeakEntity<T>` over a slot map of
  counts (`zed/crates/gpui/src/app/entity_map.rs`), deferring releases to its effect flush; unlike
  GPUI, a stale use here is an error, not ignored. Tests: a handle to a destroyed element is invalid
  and its use is a script error in both backends; a slot reused by a new element does not answer
  to the old handle; a destroy inside a click handler completes after it.
  Decided with the user 2026-10-16: no state the lab assumes one of. Each GUI owns its element
  table; handles retain it (an anchored resolve-mode Variant), so one outliving its GUI reads as
  gone. Element types are not a list in the lab's core: any library (a toolkit, a game kit, a
  viewport) registers its reflected widgets, a type's knowledge being process-wide as reflection
  is, and each GUI decides what it may create. Native code is first-class: the typed API stays
  as it is, and holds widgets weakly through the same table.
  As built on `vg`: script backends share one gone-handle rule, `isValid()` on every handle and
  any other use an error, Luau raising on a gone read and write as AngelScript did (fc1cb1d7c);
  AngelScript binds a type's bases' members as Luau does (309b1d3a0); a resolve-mode Variant may
  retain an anchor (5dffbb5a1); reflection takes a StringView API as it is and declares creators
  (`TypeBuilder::Creator`, never a script constructor; 75623f639). The lab (17f145f05): each
  SceneNode's ElementTable, `Element<T>` natively and handles in scripts; `ElementTypes`
  (register a reflected Node type with a creator under its tag, the `element` attribute or the
  type name; a taken tag refused, loudly) and a GUI's `ElementSet` (all, or allowed tags with
  aliases), through which markup and a script's `add` create, from the GUI's allocator; the
  widgets' native API reflected (geometry, visibility, opacity, id, classes, text, checked,
  value, progress; add, find, parent, childAt, destroy, `on(event, fn)`, `X.of(node)`), and
  `gui.root()` from a per-context binding. Integration.GuiScript (GUI.Script.Tests until 2026-10-23) runs the G1 tests in both languages;
  GUI.Tests covers the native half (separate tables, reuse, a GUI going, the registry's refusals,
  sets and aliases, a library's widget in markup).
  Left: unloading a library while elements of its types live is not yet refused (it must be,
  before dynamic extensions ship widgets); the lab's script names (Label, Button) would collide
  with foundation.ui's handle types were both bound in one manager, so they want a namespace;
  sheets and an inspector reading the reflected table come with G2; Entity handles could take
  the same anchor for their Scene (not done, the user to decide).
- **G2 The applier catches up with the parser.** `box-shadow` (several), `transform` (translate,
  rotate, scale, skew, with an origin), gradients from sheets with angles and stops, per-side
  borders, `var()` everywhere. Tests per property.
  Decided with the user 2026-10-16: replace, not extend, the string applier: typed values parsed
  once, one property table, the box as a value on the node; then each SceneRoot scaled on its own
  (one per window or render target; SceneNode renamed SceneRoot, `GetSceneRoot()` beside
  `GetRootNode()`).
  As built on `vg`: VG boxes take a border colour per side (e220e7d42). The style system
  (76c806a41): declarations expand into longhands and parse once into typed StyleValues (cached on
  a declaration without var()); the cascade sets longhands; var() is substituted anywhere,
  nested, with fallbacks, custom properties inheriting from the parent's style; StyleProperties
  names the properties and grammars, and a property it does not name reaches a widget's
  reflected property by its `css` attribute; the box properties become the node's NodeBox
  (background colour and gradient or image layers, a border per side, radii, box shadows) drawn
  through VG, owned by the sheet; `transform` and `transform-origin` a style transform whose
  percentages follow the size; a border with no style draws none, as CSS has it. SceneRoot and
  its display scale (5f7005068): logical units mapped to device pixels by the root's own
  transform, so drawing and hit testing agree; the media context gives logical size and density;
  media queries take resolution. Per-property tests and a sandbox row of four cards (c7c3199ab).
  Left: text at a scale is crisp through distance-field fonts only, as in foundation.ui (see Open
  limits).
- **G3 Motion.** Every animatable property transitions, with real easings (`cubic-bezier`,
  `steps`, the named ones); `@keyframes` animate any property, not only opacity and colour. Tests:
  sampled curves against the formula.
  As built on `vg` (0424cce0e): easing (cubic-bezier solved for x, steps() with every jump
  position, linear and the named curves, ease the default); interpolation over the typed values
  (numbers, lengths of one unit, colours, shadow lists padded with transparent shadows, transform
  lists of the same functions, origins, gradient layers of one shape; the rest switches halfway);
  one StyleAnimation per moving widget, an action on its SceneRoot's clock holding the target and a
  track per property (transitions retargeting from a running track; keyframes with per-stop
  timing, implicit end keys, delay, iteration count, direction, fill mode), ended tracks returning
  to the target or the property's initial value, the widget held weakly. The sandbox's cards lift
  on hover and the tilted one sways (cd30de36e). Then the follow-up the first pass left: transform
  lists of different functions move through their decomposed matrices (translation, rotation the
  short way round, skew, scale), lengths in different units in pixels (a percentage against
  another unit still switches), the transition and animation shorthands expand into their
  longhands, which cascade on their own (shorter lists repeating), and animation-play-state
  pauses an animation's clock.
- **G4 Effects through VG.** Group opacity and filters through V3's layers, rounded clipping
  through V2, boxes through V4.
  First, cheap frames (user 2026-10-16: only relayout or rerender what changed): a state, class,
  tag, id, focus, enabled or parent change marks the widget's style dirty with its subtree (a
  descendant selector may read it), and the StyleManager restyles only marked subtrees, the
  children of a widget whose custom properties changed, and everything after a new sheet, media,
  root font size or fonts. Inside a SceneRoot a container's layout waits for the root's one pass
  a frame (parents first, again while a child's new size moves its parent; input flushes it
  before a hit test); a container outside a scene still lays out at once. Setting geometry to
  what it is changes nothing. `SceneRoot::NeedsFrame` says whether anything is waiting (a redraw,
  a running action, a layout), and GUISandbox skips a frame that needs none (drawing while a
  screenshot waits; `--always-redraw`, `--frame-stats`) (57b1b0d45, a68414659).
  Then the effects: `filter: blur()` (blurs in a list add as variances) and `backdrop-filter:
  blur()`, which animate, `mix-blend-mode` (normal, multiply, screen, plus-lighter: the blends VG
  composites), and `overflow`, all written to the node's box by the sheet. A node with a filter or
  a blend, or a faded node with children, draws through one VG layer, so a group fades as one; a
  faded leaf multiplies its opacity into its draws. A backdrop blur goes under the box's
  background, in its shape. Clipped children clip to the box's rounded corners through a clip path
  (the bounds' scissor without stencil fills). VGPassTargets (VG.Renderer) holds the quality pass's
  multisampled colour and stencil targets for any host; the UI runtime moved onto it, and
  GUISandbox now draws with everything VG has (analytic boxes, layers and blurs, stencil fills), in
  a row of effect cards over stripes. A screenshot of it found repeating gradients repeating the
  whole line rather than their stops' span; fixed (b5ba2d50c, d51dd3f2a, 6e0e8d1ad, f01fa4c40).
- **G5 Data binding.** Properties that update what depends on them (AUI's model: dependency
  tracking, no manual subscriptions), bound from markup and script. Tests: a bound label follows
  its source.
  As built on `vg`: the reactive core (ffcf72a9b), a Property recording its readers, a Computed
  recomputing lazily, an Effect rerunning when what it read changed, its sources found again on
  every run; effects wait on their SceneRoot's scheduler and run once per frame however many
  changes reach them (outside a scene, at once). The evaluating observer is per thread, not per
  GUI. A node owns its bindings (989eab239): any reflected property binds by name, the value
  converted to its type, so a library's widget binds with no code of its own; a property the user
  changes (`twoWay`: slider value, checkbox, text field text) writes back; the user-set widget
  state is observable. Each SceneRoot holds a DataModel of named values. Markup binds `{{name}}`
  (52adcf108): a name alone follows and writes back, text around names follows the text built from
  them. Scripts bind through reflection (982c7925d): `node.bind(property, function)`,
  `node.bindData(property, name)`, `gui.getNumber`/`getText`/`getBool` (reads a binding follows;
  typed because AngelScript cannot return an untyped value) and `gui.set`, the same story tested in
  AngelScript and Luau. An f32 bound to text reads as its own shortest form (c4555d215). GUISandbox
  shows a bound row with no handler wired (65518d27a).
- **G6 A showcase.** The sandbox becomes a small scripted demo (a game menu with transforms,
  shadows, gradients, blur and animated states, written in AngelScript), the proof that it looks
  the part.
  As built on `vg`: `GUISandbox --showcase` (a6fe2508f) shows a main menu from
  `Data/Assets/ui/showcase`: markup for the widgets, a sheet for every look (a radial background,
  drifting orbs under blur filters, a distance-field title with a breathing glow, gradient buttons
  that lean in on hover and give on a press; the title fixed in the upper third with one slot under
  it that the main menu and the frosted settings take in turn, loading as a screen of its own, and
  the quit question as a modal over the dimmed and blurred menu, each rising or fading in), and an
  AngelScript that switches the pages, screens and modal, binds two labels to functions of the GUI's
  data and fills a loading bar through `update(dt)`, which the host calls only while it asks.
  Native code sizes the layers, seeds the data and closes the window once Leave has faded;
  `--showcase-call <function>` opens another panel for a screenshot. The sheet's text effects came
  first (89f000618: text-shadow, text-outline and text-glow, inherited through the VG's state and
  animatable). Building it found four defects, each fixed with a test: a container never heard a
  child resize, so a flex layout kept the zero basis it met a child with before the sheet sized it
  (e4c7ea85e); the lab's `X.of(variable)` did not compile in AngelScript, its Variant parameter a
  `?&in` sink there (b2b318cf0 lets a method declare a Variant parameter's type, 78222f33c uses it);
  and an explicit handle passed to a Variant sink was read one level short, which crashed
  (c0a11098c).

- **G7 Navigation and commands (must-haves, user 2026-10-14; every other phase designs for them).**
  `foundation.ui` has a spatial picker with code-only directional overrides (`View::NextFocusUp`
  and the rest, `FocusManager::MoveFocus`), an `ICommand` buttons bind to, and keyboard-only
  shortcuts (`ShortcutManager`); the lab has focus but no directional navigation.
  - **Spatial navigation, automatic by default**: a geometric picker takes the best candidate in the
    pressed direction (overlap on the cross axis preferred, distance weighed, hidden, disabled and
    clipped widgets skipped), so most screens need no authoring.
  - **Overrides where the geometry is wrong**: `nav-up`, `nav-down`, `nav-left`, `nav-right` (a
    widget's id, `none` to stop, `auto`) and `nav-next`/`nav-prev` for the tab order (document
    order otherwise), from markup, sheets and script.
  - **Focus scopes**: a container says what happens at its edge (pass to the neighbours, the default;
    wrap, for menus and carousels; contain, for dialogs); re-entering a scope returns to its last
    focused child; each screen or scope names its default focus.
  - **Gamepad feel**: a held direction repeats with acceleration; focus scrolls into view; the focus
    ring shows in gamepad and keyboard use only; a widget that uses the directions while active (a
    slider, a list) gives them back at its ends. A debug overlay draws the navigation graph.
  - **One command model**: a command has an id, a label, an icon, enabled and checked states and an
    execute, reflected so scripts define and run them; buttons, menu items, shortcuts and gamepad
    buttons bind to commands by id, so one disabled command greys every control bound to it.
  - **Bindings**: keys and gamepad buttons alike, scoped (global, a screen, the focused subtree) and
    routed by focus (the innermost scope wins), and fillable from the game's input map (Back is
    Escape and the East button). Once proven, `foundation.ui`'s `ICommand` and `ShortcutManager`,
    and perhaps the editor's action registry, converge on it.
  - **Dispatch, after GPUI** (`zed/crates/gpui/src/key_dispatch.rs`, `keymap.rs`,
    `keymap/context.rs`): a command runs through the focus path, capture then bubble, a bubbling
    handler stopping it unless it says otherwise; a command is available (enabled, its controls
    lit) when a handler for it is on the focus path, so moving focus greys and lights the right
    menu items with no bookkeeping; each scope may declare a context, and a binding names the
    contexts it applies in with a small predicate (`&&`, `||`, `!`, `==`), the deepest matching
    context winning and then the later binding; a binding to nothing unbinds a key; multi-key
    chords; the focus ring shows only after keyboard or gamepad input. GPUI has no directional or
    gamepad navigation, no focus scopes or focus memory, and no label, icon or checked state on an
    action; those stay as specified above.
  Tests: the picker over grid and irregular layouts, overrides and scopes (wrap, contain, focus
  memory), repeat timing, and a command's state reaching a button, a menu item and a binding.
  As built on `vg`: directional navigation (6462fdfe0), a picker preferring neighbours overlapping
  on the cross axis, then the nearest along it, skipping hidden, disabled and clipped-away widgets
  (a scroller's clip excepted) and the focused widget's own ancestors; focus scopes that pass, wrap
  or contain and remember their last focus; keys to the focused widget first, its unused arrows
  (KeyEvent::Handled) moving focus, so a list gives them back at its ends (a slider keeps left
  and right even there: holding the d-pad to its end must not throw focus off it, user ruling); Return or
  Space activating as a click. Overrides and scopes from sheets, markup and scripts alike
  (72f1e43b4: nav-up and the rest, nav-scope, nav-default, reflected properties carrying their CSS
  names). A held direction repeating after 0.4 s from 0.16 s down to 0.05 s, focus scrolling into
  view, and `:focus-visible` for a keyboard or gamepad focus ring (04625cf82). One command model
  dispatched along the focus path, capture then bubble, available while enabled with a handler on
  the path, its state reaching a bound button, checkbox and menu item as a binding follows it, and
  scripts defining, switching, handling and running commands (049c4e109). A keymap after GPUI's,
  keystrokes and chords bound to commands under context predicates, the deepest match then the
  later binding winning, unbinding by binding to nothing; the lab's KeyCode grew every letter,
  digit and F1 to F12; the shell bridge routes the d-pad, the left stick (with hysteresis) and the
  buttons, and BindFromInputMap fills a keymap from the game's input map (d16b70e28). Focusing a
  page lands on its entry, and Tab stays in a containing scope (cabcfadfd). The showcase plays by
  keyboard and gamepad, its Back on Escape and the East button (cd36ded44). Answering whether the
  reactive core is re-entrant found an effect without a scheduler rerunning inside its own run;
  fixed (0c14186e8).

- **After G7: GUIContext (user direction 2026-10-16).** One GUI of several roots (windows, render
  targets, a panel in the world). The context holds what is the GUI's: one animation clock (each root
  pausing or slowing its own time on it, user ruling), deferred tree edits, one bindings flush a
  frame for every root, the data, the commands and keymap, and the element table (moved from the
  root, user ruling: a weak handle survives its node moving between roots) and element set; it
  tracks the active root, the one last given input, which keystrokes, the gamepad and command
  availability follow (user ruling). A root keeps its dispatcher, display scale, viewport, media
  context, layout and drawing. Every root is made in a context and the host advances the GUI through
  GUIContext::Update; the first cut's shims for single-root code (a private context, a forwarding
  SceneRoot::Update and accessors) went (e3848daca, user direction: the lab has no users to keep
  them for). Before it, actions
  learnt to hold their target weakly and run on a per-target time scale (9ae4e9370); the context
  itself is af65a9d67. Playing the showcase found four defects, each fixed with a test: a held
  d-pad direction skipped the focused widget (950b75377); a click focused the full-window backdrop,
  so the first arrow found nothing (251292e28: a click focuses what takes focus, navigation from
  what does not starts from the entry, and focus shows until the pointer is used); a slider gave its
  arrows back at its ends (fadf8c341: it keeps them, user ruling); and Right took a wide field in
  the row above (6a58abbea: a candidate must start past the focused widget's edge).

## Open limits

What the phases left, gathered so each has a home (from each phase's "Left"); an item leaves this
list when it is done or the user rules it out.

- **G1, before dynamic extensions ship widgets:** unloading a library while elements of its types
  live is refused, not only documented.
- **G1, when the lab's script surface and foundation.ui's are bound in one manager (the move into
  foundation.ui):** the lab's script names (Label, Button) take a namespace, as they would collide
  with foundation.ui's handle types.
- **G1, with the showcase or the move:** an inspector reading the reflected table (sheets read it
  since G2, through the `css` property attribute).
- **The user to decide:** Entity handles taking the anchored Variant for their Scene, so an entity
  handle outliving its scene reads as gone rather than dangling.
- **Accepted for now, revisit if a game needs it:** text at a display scale is crisp through
  distance-field fonts only (a coverage font is stretched), as in foundation.ui; a coverage font's
  text effects are a shadow's copy only; a glow is a linear ramp, not a Gaussian.
- **G4, when a screen's restyles show in a profile:** a state change restyles the whole subtree
  even when no sheet rule reads that state through an ancestor; the sheet could say which states
  and classes its non-subject selectors use, and a change to any other restyle the widget alone.
  An animation keeps frames coming while its widget is scrolled out of view or clipped, and a
  frame redraws the whole tree, not only the regions that changed.
- **G4, when a screen asks for them:** filters other than blur (brightness, contrast, grayscale,
  drop-shadow, ...) need a colour pass in VG's layer composite; blend modes beyond VG's four; a
  faded leaf is not grouped (its background shows through its own text); gradient stops take
  percentages only (CSS also takes lengths); a repeating radial gradient whose first stop is past
  0 repeats from the centre, and a repeating conic gradient does not repeat its span.
- **G4, as CSS has it:** a node drawing through a layer draws its own backdrop blur first, onto
  what lies behind it at its opacity (frosted glass fading in blurs as it fades); a backdrop blur
  inside an ancestor's layer (a faded ancestor, a filter) sees only what that ancestor drew, as
  CSS's backdrop root makes it.
- **G5, when a screen asks for them:** a template formats a value as it is (no `{{volume:0%}}`
  format, no expressions: a native or script binding computes those); a widget's other properties
  (position, size, visibility, a list's selection) are not observable yet, so a binding reading
  them does not follow them; lists bound to a data collection (a repeated template per item) wait
  for the model/view layer's turn.
- **G6, with G7:** the menu takes keyboard and gamepad navigation when G7 brings it; the host
  sizes the showcase's layers to the window by hand (a fill-the-parent size, or anchors in markup,
  would let the sheet say it).
- **G7, when a game asks:** a held keyboard arrow repeats at the system's key repeat, not the
  lab's curve; a command's label and icon do not yet reach the controls bound to it (their text is
  their own); the keymap matches the lab's keys only (no punctuation, keypad or media keys); an
  input map's composite and axis bindings are not keystrokes, so they are skipped; Back is the
  script's to define, not built in.
- **The reactive core, as a rule to keep:** a binding must not yield (a script coroutine paused
  inside a bound function would leave the evaluating observer set while other code runs on the
  thread; nothing enforces it yet); values are linked to their readers without locks, so two GUIs on
  different threads must not share a value (one DataModel, one widget), and a change crossing
  threads is to be posted to the other thread's queue.
- **F3, after the lab (foundation.ui text):** a label measures with its font's unshaped
  MeasureString, so a ligated label lays out a little wide; the basic shaper's wrapping knows no
  bidi; editing right-to-left text (caret movement by arrow keys, selection across a direction
  change) is untested.

## Moving into `foundation.ui`

Once a lab feature is proven (tests, the showcase, the user's look), it moves into `foundation.ui`
as its own layer, the editor and the samples keeping working at each step. The order is decided
then; likely the CSS front end and the motion first, the reflected object model last.

## Open questions

- V3: the offscreen pool's budget on the web build.
- G1 (decided in G1, 2026-10-16): the lab's widgets are reflected directly, their native API
  as it is (no facade, no script twins).
- G1: whether `foundation.ui`'s script handles, which own their views today (a stale one is a quiet
  no-op), move to the lab's weak handles when lab features move over: one rule everywhere, but a
  change for existing scripts (a stale handle becomes an error).
