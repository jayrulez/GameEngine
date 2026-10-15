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
  difference is in translucency. Left: the game's UI, which draws inside the renderer's passes
  (its HUD over the scene, render-texture canvases, the Game tab), needs a pass of its own to draw
  Encoded.
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
- **F2 Inline runs and rich text.** A `TextDocument` of runs (text, family, size, colour,
  weight, italic, underline) and inline objects (an icon on the baseline), laid out into lines by
  an inline formatter, behind a rich text control beside `Label`. A small markup for it, the same
  from SML and from script (a colour, bold and italic, a size, an icon, a link with an id the
  script hears), and a script surface that sets the markup or the runs: "Your mark wears [a rose
  gown]" in its colour is one label. Tests: a paragraph mixing two sizes and three colours measures
  and wraps; an icon sits on the baseline; a malformed tag is shown as text and logged, never
  dropped; both script backends set runs.
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
- **F4 Text effects.** Outline, shadow and glow per run, from the distance-field fonts the engine
  already bakes (`Fonts.DistanceField.Baker`), so a HUD's text reads over any scene without a
  panel behind it. Tests: an outline's width and a shadow's offset probed in pixels; effects on one
  run of a rich paragraph and not its neighbours.

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
