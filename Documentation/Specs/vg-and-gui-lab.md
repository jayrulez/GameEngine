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
  difference is in translucency. The game's screen UI followed (fda100af8): the overlay pass draws
  through the target's UNORM view when the host gives one (the player's back buffer, the Game tab's
  viewport, now 8-bit sRGB like the player's window); world panels and render-texture canvases stay
  Linear, lit as part of the scene.
- **V2 Nested clips.** Clip paths nest (an intersection, not a replacement), so a rounded card can
  hold a scrolled list of rounded items. Tests: two nested rounded clips probed at their corners.
  As built on `vg` (519edf76d): a nested clip writes its winding inside the mask and applies over
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
  and one composite quad (a9f623693); `VGRenderer::RenderLayers` draws each layer, innermost first,
  into a target of its own (the renderer's format, sample count and stencil, so clips work inside;
  64-pixel size buckets from a pool per frame slot) before the host's pass, and the composite
  scales the premultiplied target by the opacity through `vg_layer` (26a152b6c). Layers are a host
  opt-in (`SetLayers`), and past 128 in a batch a layer is an opacity push. `foundation.ui` fades a
  view group through a layer (a leaf keeps the opacity push) and a fading popup (308714c46); the
  editor's windows draw them, and the game's screen UI does through a new step before the shared
  overlay pass, `IScreenOverlay::PrepareOverlay` (7ac90c055, f5fda55fb). World panels, render
  texture canvases and the scene tier draw inside passes and keep layers off. Fixed on the way: the
  pipeline variant tables were one kind short since V4 (204a59959). Probes: two overlapping opaque
  reds in a half layer read 188 linear and 128 encoded, overlap and not; a half layer in a half
  layer is a quarter; clips inside a layer land in place; the same multisampled.
  Filters (95acfd514, 960fdaa48): a layer may be blurred (a Gaussian sigma, separable, in the
  canvas's blend space, up to 32), and `DrawBackdropBlur` blurs what the canvas drew behind a
  rounded shape: everything drawn so far in the current target moves into a layer (layers are
  then ordered by depth, not index) whose blurred copy the shape samples. The backdrop is the
  canvas's own drawing, not the scene under a game's UI, which would need the host's target; and
  none is drawn inside a clip path. Sheets reach both as `filter: blur()` and
  `backdrop-filter: blur()`, and the sheet's `opacity` now applies (b7ad83366). The UI sandbox has
  a Layers tab to look at (d1fab9534). Desaturate and colour matrices are left for a need.
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
  As built on `vg` (3f260b613, 66f136e37): `VGContext::DrawBox` (per-corner radii, per-side borders)
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
  As built on `vg` (d1d63d14e): `GlyphPosition` is {cluster, glyphId, x, y, advance, glyphInfo,
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
  As built on `vg`: runs and the markup (07f54ef0d): [b] [i] [u] [color=] [size=] [font=] [link=]
  closed by [/name] or [/], [icon=path], "[[" for a bracket; malformed text kept and reported with
  its offset. The inline formatter (861930e0a) breaks after spaces, shares a baseline across sizes,
  sits icons on it; bold and italic take a "<family> Bold" / "Italic" face when the font service
  has one (`IFontService::HasFamily`, 99ee0925b) and are synthesized when not (bold drawn twice,
  italic slanted). `RichText` (a19fb788d) beside Label, in SML as `<RichText markup="...">`, with
  OnLink and a hand pointer over links; scripts get `ui.findRichText` with setMarkup, plainText,
  addRun, and onLink reading `clickedLink` (cdbb1bd57). The UI sandbox has a Rich Text tab
  (f50d7d4f0). Kerning across a run boundary and breaking inside an over-long word are left out.
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
  (ac3396dab).
  As built on `vg` so far: atlases answer GlyphFor(codepoint) and look up by glyph id (730e3cf3d);
  every atlas, baker and cooked font keys by glyph index, the TrueType atlas packing glyph indices
  itself, and every shaper writes them (c80aec10e; FontResource 3); fonts expose their file and
  cooked fonts keep it (3f65b0d16); a TextShaperFactory and a shared ClusterTextShaper (3276b36b0);
  Fonts.HarfBuzz, HarfBuzz behind ITextShaper only (57d622573); bakers bake what a shaper's
  substitutions reach, through ITextShaper::CollectReachableGlyphs (23d4e26a7; font builder 8);
  the engine's UI, the editor and the UI sandbox install it (a40662a37). Then: ITextShaper::Shape
  takes a ShapeRequest (a font list with fallback, a direction, an ISO 15924 script, a BCP 47
  language; a6c260383); VG shapes single-line text through the font's shaper, cached across
  frames (63e94cfa2); the distance-field cook keeps every baked glyph (795d7f4b0, a bug the game's
  title showed); SheenBidi 3.0.0 is vendored (be3970ad4, Apache-2.0; the project enables C for
  it); the bidi seam (IBidiResolver, ReorderLine for L1/L2) and shapers that lay lines out in
  visual order, with right-to-left carets and hit tests (c98a1618d); Fonts.Bidi, SheenBidi behind
  IBidiResolver only (a18c658fd); the hosts install it (ae6c58ece). Left: a label measures with
  its font's own MeasureString (unshaped), so a ligated label is laid out a little wide; the basic
  shaper's wrapping knows no bidi; editing right-to-left text (caret movement by arrow keys,
  selection across a direction change) is untested.
- **F4 Text effects.** Outline, shadow and glow per run, from the distance-field fonts the engine
  already bakes (`Fonts.DistanceField.Baker`), so a HUD's text reads over any scene without a
  panel behind it. Tests: an outline's width and a shadow's offset probed in pixels; effects on one
  run of a rich paragraph and not its neighbours.
  As built on `vg`: the distance-field baker writes MTSDF, the true signed distance in alpha beside
  the multi-channel field (6cf7438a7; font builder 10, editor cache 4), so contours past the edge
  round a corner. `VGContext::SetTextEffects` (dd8e7fc70) puts a `TextEffects` (outline, shadow,
  glow) on the state; a text draw emits its glyphs once per pass, shadow, glow, outline, then the
  glyphs, each pass carrying two values per vertex (`VGVertex::effect`: the edge moved out and its
  ramp widened, in field units converted from the text's pixels per glyph), and the shader reads
  alpha for those passes and the median for the glyphs. An effect reaches as far as the field
  holds (half its range) and is scaled down past that; a coverage font draws a shadow's offset copy
  only. Probed on Vulkan and WebGPU: a 1.5 px outline adds 1.5 px each side of a 48 px stem and
  leaves the stem, a shadow lands its offset. Markup and sheets (1403695e1): `[outline=<w>
  [colour]]`, `[shadow=<x> <y> [blur] [colour]]`, `[glow=<r> [colour]]` per run, the parts a run
  leaves out the control's; RichText draws every run's effects before any run's glyphs
  (`SetTextEffectsOnly`); `text-shadow`, `text-outline` and `text-glow` in sheets go onto the VG
  state as a view draws, reaching its children as CSS's text-shadow does, `none` turning one off.
  A field's range is a load option and a font asset field (0d8c3da3b and after; 4 texels by
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
- **G2 The applier catches up with the parser.** `box-shadow` (several), `transform` (translate,
  rotate, scale, skew, with an origin), gradients from sheets with angles and stops, per-side
  borders, `var()` everywhere. Tests per property.
- **G3 Motion.** Every animatable property transitions, with real easings (`cubic-bezier`,
  `steps`, the named ones); `@keyframes` animate any property, not only opacity and colour. Tests:
  sampled curves against the formula.
- **G4 Effects through VG.** Group opacity and filters through V3's layers, rounded clipping
  through V2, boxes through V4.
- **G5 Data binding.** Properties that update what depends on them (AUI's model: dependency
  tracking, no manual subscriptions), bound from markup and script. Tests: a bound label follows
  its source.
- **G6 A showcase.** The sandbox becomes a small scripted demo (a game menu with transforms,
  shadows, gradients, blur and animated states, written in AngelScript), the proof that it looks
  the part.

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

## Moving into `foundation.ui`

Once a lab feature is proven (tests, the showcase, the user's look), it moves into `foundation.ui`
as its own layer, the editor and the samples keeping working at each step. The order is decided
then; likely the CSS front end and the motion first, the reflected object model last.

## Open questions

- V3: the offscreen pool's budget on the web build.
- G1: whether the lab's widgets become `Object`s with reflected properties directly, or are
  described to the binding by a reflected facade; to decide in G1's first layer.
- G1: whether `foundation.ui`'s script handles, which own their views today (a stale one is a quiet
  no-op), move to the lab's weak handles when lab features move over: one rule everywhere, but a
  change for existing scripts (a stale handle becomes an error).
