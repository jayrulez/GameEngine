# UI layout and style model v2 - uniform layout style, a real cascade, transitions

> ARCHIVED 2026-11-02, obsolete: its subject was replaced or removed. Non-authoritative: the
> original spec, kept as the record of what was built and why; the code and the tests are the truth.

> STATUS: P0 BUILT 2026-09-09 (LayoutStyle on View; LayoutParams classes, CreateDefaultLayoutParams,
> RegisterLayoutParam deleted; four lanes green). P1 BUILT 2026-09-09 (ordered cascade + computed
> cache, selector chains, inherit/initial, custom properties + var(), palette as root variables,
> %/em/calc lengths in sheets and markup). P2 BUILT 2026-09-09 (box model: z-index draw/hit
> order, Position::Absolute in any container, box-shadow through a VG DF shadow mode, overflow,
> min/max, sheet-driven LayoutStyle, Float consumers read Length values). P3 BUILT 2026-09-09
> (transitions: `transition` lists, per-view retargeting transitions overlaying ResolveStyle,
> the state cross-fade through Drawable::Draw, UA defaults on the interactive controls).
> P4 BUILT 2026-09-09 (Flex line wrapping + align-content, gaps on both axes, flex-basis,
> `text-overflow: ellipsis`). The spec's phases are COMPLETE; the visual iteration at the desk
> is next.
> P4 as built (deviations from section 5, all deliberate):
> - FlexLayout is restructured around a LINE model (both passes collect in-flow children into
>   items, break them into lines - one line when `Wrap` is off - and work per line). `Wrap`
>   (bool; markup `wrap="wrap|nowrap"`), `AlignContent` (Start/End/Center/SpaceBetween/
>   SpaceAround/Stretch; default Stretch = CSS `normal`, lines share the free cross space);
>   a single line without wrap ignores align-content and IS the container (today's stretch
>   behaviour kept). Growing children take their share per line; a line's cross size is its
>   tallest item, Match-cross children re-measure at their line's size. Layout re-breaks
>   against the ARRANGED main size with the measured (grown) sizes, half-pixel tolerant; if a
>   parent arranges the container at a size other than it measured, grow shares are stale
>   for that frame (the pre-P4 limitation, unchanged).
> - Gaps: `Spacing` (main axis) and `LineSpacing` (between lines) stay the code-side names;
>   the CSS AXIS names `RowGap`/`ColumnGap` (Optional overrides, mapped by Direction:
>   `MainGap()`/`CrossGap()`) are what markup `gap="<row> [<column>]"`, `row-gap`,
>   `column-gap` write. `gap` is container data, not a sheet property.
> - `flex-basis` (`LayoutStyle::FlexBasis`, a Unit; zero = auto): the main-axis starting size.
>   Auto = the content size, or 0 for a GROWING child (the Sedulous/`flex: <grow>` shorthand
>   semantics every existing grow test encodes - CSS `flex-basis: auto` with grow would be
>   content + share). A basis without grow IS the main size. % resolves against the available
>   main size (0 when unbounded), em against the child's font; from the sheet through the P2
>   effective-layout refresh; markup `flex-basis="..."`.
> - `flex-shrink` is still not read (nowrap overflow runs past the edge, as before).
> - `text-overflow: ellipsis | clip` (String keyword) reaches Label through
>   `Label::EffectiveEllipsis()` (the Ellipsis property wins when set); the truncation itself
>   is the existing `fonts::TruncateToWidth` (measured by the font, codepoint-aligned, never
>   wider than the box beyond the 1px snug tolerance) - now pinned by Fonts.Tests.
> - NOT built: `flex-flow`/`flex` shorthands, `wrap-reverse`, `order`, `align-content: space-
>   evenly`, ellipsis on multi-line/wrapped text (word-wrap keeps today's behaviour).
> P3 as built (deviations from section 4, all deliberate):
> - `transition: none | <property|all> <duration> [<easing>] [<delay>] {, ...}`; times in
>   `ms` / `s` (a bare number is ms); easings linear / ease / ease-in / ease-out /
>   ease-in-out (quadratic curves, not CSS cubic-beziers); later entries override earlier ones
>   (CSS); an unknown property drops its entry; `none` is an EMPTY list (it still wins over a
>   UA `all`). Value kind `StyleValue::Kind::Transitions` (a shared `TransitionList`).
> - No Storyboard: a transition is a per-view entry `{property, from, to, clock}` in
>   `View::m_transitionState`, overlaid by `ResolveStyle` (`ComputeStyle` is the cascade
>   value without the overlay). Colors, floats, thicknesses, lengths (a Float mixed with a
>   Length becomes a dp Length) and box shadows interpolate (`LerpStyleValue`); every other
>   kind switches at the midpoint. Animatable set: `IsAnimatableStyleProperty`.
> - Trigger: the style-cache rebuild in `EnsureStyleCache`. When only the generation or the
>   control state moved (same sheet epoch, same summed sheet versions - the old cache's rule
>   pointers are provably alive), the OLD cache is read first for every listed animatable
>   property that had a winner (or inherits), through the overlay - so a re-trigger starts
>   from the CURRENT mid-flight value and never snaps; then the new value is computed and a
>   differing one starts or RETARGETS the entry. A sheet swap (`UIContext::SetStyleSheet`,
>   `View::SetLocalStyleSheet` -> the context's sheet epoch) or a rule edit rebuilds without
>   animating and ends running transitions. `View::InvalidateStyle` therefore keeps the cache
>   valid-but-stale (the generation bump rebuilds it); detached views never transition.
> - Tick: `UIContext::BeginFrame` advances the listed views (`View::AdvanceTransitions`),
>   which mark redraw damage for visual-only properties and layout damage otherwise
>   (`IsVisualOnlyStyleProperty` - the PROPERTY's kind, never the rule's). Detach de-lists.
> - State-aware drawables (`state-colors`, StateList/Layer/Inset) pick colors INSIDE Draw,
>   invisible to the cascade, so a control-state change cross-fades the whole background: the
>   old state at full strength under the new state at the eased opacity (no mid-fade coverage
>   dip on opaque backgrounds). `Drawable::Draw(ctx, bounds, state)` is now the NON-virtual
>   entry that applies the draw context's `DrawBlend` (set per child by DrawChildren from
>   `View::CurrentDrawBlend`) once and dispatches to the new protected virtual `DrawState`;
>   a `background:` drawable change cross-fades the same way (old drawable under new).
>   Opacity, not compositing: overlapping translucent layers double-blend mid-fade.
> - Motion is THEME DATA (user ruling 2026-09-09: the engine never hard-codes a list of
>   control types). There is no user-agent sheet and `UIContext::SetStyleSheet` prepends
>   nothing. Each shipped theme (dark, light, rounded-dark, breeze) declares in its `View`
>   defaults block `transition: background | text-color | text-dim-color | border-color |
>   accent-color | box-shadow | opacity 120ms ease-out` - visual kinds only, geometry never
>   animates; a control opts out with a later `transition: none`. A theme-wide rule is
>   affordable because the trigger compares WINNING-RULE POINTERS per property after a
>   rebuild (the same rule under the same sheets is the same value) and resolves only the
>   properties whose winner changed; a child with no rule of its own inherits the parent's
>   mid-flight value through the overlay without an entry.
> - NOT built: `transition-property/-duration/...` longhands, `cubic-bezier()`, `steps()`,
>   transition events, a per-transition Storyboard object (nothing needed one).
> P2 as built (deviations from section 2, all deliberate):
> - No `VisualStyle` value object: the per-view computed-style cache from P1 IS the visual
>   style; DrawChildren reads `box-shadow` / `overflow` and the view its `corner-radius` /
>   `background` from the cascade at draw time (state-keyed, so `:hover { box-shadow }` works).
> - `LayoutStyle` fields are `Declared<T>` (assignment declares; `->` for member access;
>   `IsDeclared()`): `View::Layout()` is the EFFECTIVE style = every declared inline field plus
>   the cascade's `width/height/margin/min-*/max-*/position/top/right/bottom/left/z-index/
>   flex-grow/flex-shrink/align-self` for the undeclared ones (inline wins even when it equals
>   the default - CSS inline-style rule); `View::DeclaredLayout()` is the inline value alone.
>   Refreshed by SetLayout and at the top of every Measure (self + children, since containers
>   read child->Layout() before measuring). `width: match | wrap` keywords work in sheets.
> - New fields: `Position` (Static | Absolute), `Right/Bottom` insets (f32 like Left/Top;
>   sheet insets resolve dp/px/em - a PERCENT inset has no reference box at refresh time and
>   reads 0), `ZIndex` (i32), `MinWidth/MinHeight/MaxWidth/MaxHeight` (Unit; zero = none;
>   %/em resolve like a Fixed size; max wins over min when they cross, and a Fixed size is
>   clamped too).
> - Absolute in ANY container: the six layouts and the base ViewGroup skip `!View::IsInFlow`
>   children (Gone or Absolute); `View::Measure` then measures them shrink-to-fit inside the
>   declared insets (both insets on an axis pin both edges) and `View::Layout` places them
>   against the content box after OnLayout (Right/Bottom anchor the far edge). They never
>   feed the container's MeasuredSize. Custom containers outside the six that iterate
>   children themselves keep laying absolute children out as before until they adopt
>   `IsInFlow`.
> - Draw order: `DrawChildren` walks ascending z-index (stable, child order within a level);
>   `ViewGroup::HitTest` walks the same order front to back. Zero z-index everywhere = the old
>   behaviour, no sort.
> - `box-shadow: x y [blur [spread]] color [inset]` / `none` -> `StyleValue::Kind::Shadow`
>   (`BoxShadow`), drawn by `VGContext::FillBoxShadow` (new `VGDrawMode::BoxShadow`, ONE
>   shader `vg_shadow.ps.hlsl`: rounded-box SDF x erf Gaussian, four quadrant quads reaching
>   3 sigma, sigma = blur / 2) under the child (outset) or over it (inset), rounded by the
>   child's `corner-radius` + spread. Renderer hosts pass the shader module; when absent the
>   commands are skipped (no crash, no shadow). Web: the WGSL cook picks the new .hlsl up.
> - `overflow: hidden` -> `View::EffectiveClipsContent()` (the ClipsContent field OR the
>   sheet). `overflow: scroll` is not a thing here (ScrollView is the scrolling container).
> - Opacity stays PER-VERTEX (PushOpacity multiplies down the tree; parent 0.5 x child 0.5 =
>   0.25 on every vertex) - the VG has no offscreen layer, so overlapping siblings inside a
>   translucent subtree double-blend. Composited opacity waits for a VG layer seam.
> - Consumer migration: `View::ResolveStyleFloat` now resolves a Length value (em/px/pt/dp,
>   calc) against the view's font size, so `font-size: 1.2em` / `corner-radius: 0.5em` in a
>   theme reach every control without per-control edits; percent reads 0 on that path.
> - Flex `align-items: stretch` no longer overrides a child's FIXED cross size (CSS: stretch
>   applies to an auto cross size only).
> - NOT built (section 2 items with no consumer yet): per-edge `border`, per-corner
>   `border-radius`, gradient backgrounds, `cursor` / `visibility` from the sheet.
> - Markup attributes added: `position right bottom z-index min-width min-height max-width
>   max-height` (the one table `MarkupRegistry::LayoutAttributeNames`).
> P1 as built (deviations from section 3, all deliberate):
> - Selectors: `#id` (View::Name), descendant + child `>` combinators (right-to-left with
>   backtracking), `:first-child :last-child :empty`, the CSS aliases `:active`(pressed)
>   `:focus`/`:focus-visible`(focused) beside the existing state names. An unknown TYPE name is
>   consumed and matches nothing (it used to be mistaken for a property). CSS specificity
>   (id 100, class and each pseudo-class 10, type and pseudo-element 1) - `:hover` now weighs
>   like a class, so `Type:hover` vs `Type.primary` is decided by source order, as in CSS.
>   NOT built: `[attr=value]` (views keep no attribute bag - add when a consumer needs it) and
>   `:selected` (no control state carries it; `:checked` is what tabs/lists use).
> - Cascade: per view, the matching rules of the context sheet, the ancestors' local sheets
>   (outermost first) and the inline sheet, each stable-sorted by specificity, concatenated in
>   that order; last declaration wins PER PROPERTY. Cached on the view (winning rule per
>   property + the rule list) and keyed on (context style generation, the SUMMED versions of
>   the sheets in that view's chain, control state). Sheets carry their own version; a rule
>   edit bumps its owning sheet - so an editor context and an embedded game context with
>   different themes never invalidate each other, and an inline edit invalidates one view.
>   Pseudo-element (part) lookups stay uncached.
> - Inheritance: text-color, text-dim-color, font-size, font-family, word-wrap take the
>   parent's COMPUTED value when unset; `inherit` works on any property; `initial` unsets
>   (no inheritance either). An unset `var()` with no fallback behaves like unset.
> - Variables: `--name: value` on any rule (typed by its literal: color, drawable factory,
>   number/length, string, bool, or another var()); `var(--name[, fallback])` on any property,
>   fallback in the property's own syntax, nested references and fallbacks (depth 8). Custom
>   properties inherit through the parent chain; `View::CustomProperty(name)` reads them (the
>   custom-layout extension point). The loader prepends a `View { --<palette-key>: color }`
>   rule when it has a palette, so `$name` (parse time) and `var(--name)` (compute time)
>   read the same colors. Note `View { --x }` re-declares on EVERY view (like CSS `* {}`):
>   declare theme-wide variables on `RootView`.
> - Units: `Unit` is a component SUM (dp, px, pt, %, em) so `calc(a +/- b)` is a value;
>   `SizeSpec::Fixed` resolves % against the containing box on that axis (0 when unbounded)
>   and em against the computed font size in View::Measure; markup `width="50%"`, `"2em"`,
>   `"calc(100% - 20dp)"` parse (StyleValueParser::ParseLengthText). In sheets a plain
>   number stays a Float (px/dp/pt as before); %, em and calc() become a Length value read
>   through `View::ResolveStyleLength(prop, referenceSize)` (em of the view's own font size;
>   font-size itself in em of the parent's). Controls still read Float, so em/% in THEME
>   sheets wait for the consumer migration that comes with the P2 box model; sheet-driven
>   LAYOUT properties (width/height/margin into LayoutStyle) also land with P2.
> - Still open from P0: unknown markup attributes remain warnings.
> P0 as built (deviations from the sketch below, all deliberate):
> - Field names follow the house PascalCase: `Width/Height/Margin/FlexGrow/FlexShrink/AlignSelf/
>   Gravity/Dock/Left/Top/GridRow/GridColumn/GridRowSpan/GridColumnSpan`. `FlexShrink` keeps the
>   old default 0 (FlexLayout does not read it yet); min/max/flexBasis/position/insets/zIndex
>   arrive with P1/P2 when their consumers land.
> - `View::Layout()` (const ref) + `View::SetLayout(style)` (marks layout damage only when the
>   value changed); `AddView(child)` KEEPS the child's style, `AddView(child, style)` sets it.
>   Same pair on `InsertView`, `Expander::SetContent/SetActions/SetHeaderActions`, `Docking
>   SetContent`. No mutable accessor: read, copy, change, `SetLayout`.
> - Grid auto-flow resolves cells per pass (`GridLayout::ResolvePlacements`) and never writes
>   the placement back; a child's `GridRow/GridColumn` stay -1 so reordering re-flows.
> - `Dock` and `Align` enums moved to `:layout_style` (the style needs them before the layouts).
> - Markup: `MarkupRegistry::LayoutAttributeNames()` + `ApplyLayoutAttribute` are the one
>   vocabulary (loader, completion, tests); every element accepts every layout attribute
>   whatever its parent, so `width/height/margin` on a ROOT element now take effect (they
>   were silently dropped before - screen overlays and badges sized by markup changed).
> - Unknown attributes stay cook WARNINGS (the tree still builds) rather than the hard load
>   error in section 3: the editor's live preview and the pipeline's warning contract depend
>   on partial loads while typing. The old `grow=`/`shrink=`/`dock=`/`gravity=` spellings are
>   unknown attributes (no aliases). Promoting unknown attributes to a load error is P1's call.
> - Hit-test visibility stays self-only (ToastHost/tool-float layers rely on children staying
>   hittable); the passive-badge test now probes outside the badge.
> Existing-implementation check (per the CONVENTIONS spec rule), everything cited
> was read on 2026-09-09:
> - Layout: `UI/Layout/LayoutParams.cppm` (base: Width/Height SizeSpec + Margin) and
>   FIVE per-layout subclasses (Absolute/Dock/Flex/Frame/Grid LayoutParams); every
>   View carries `RefPtr<LayoutParams>` minted by `View::CreateDefaultLayoutParams`
>   (one heap object per view, an RTTI cast per child per pass). `FlexLayout` has
>   no wrap. `SizeSpec` = Fixed/Match/Wrap. `Unit` = Dp/Px only.
> - Markup: `MarkupRegistry::RegisterLayoutParam(parentTag, attr, setter)` binds
>   child attributes to the PARENT's param type ("Flex","grow"); `MarkupLoader`
>   mints the param object per child from the parent tag.
> - Styling: `.sss` sheets (SSSParser vocabulary: background/border/text/font/
>   radius/padding/margin/opacity/width/height...), `StyleSelector` with type/
>   class/id + pseudo-class/pseudo-element and a `Specificity()`; `StyleSheet`
>   resolves each property by BEST specificity (ties: last wins) - a per-property
>   pick, not an ordered cascade; no inheritance, no variables (Palette only),
>   no calc/percent. Themes are moving to `.sss` (ui-theme-migration.md).
> - Animation: `UI/Animation` Storyboard/ViewAnimator/Easing - the base for
>   transitions; nothing today animates a STATE change.
> - Text: partial ellipsis (4 files), no shaping/RTL. Toolkit (20k lines, docking,
>   code editor, node graph, property grid) is NOT touched by this spec.
> Direction ruling (user 2026-09-09): EVOLVE our UI - not port-as-is to Beef, not
> greenfield. Do this in Raptor first; the Beef UI port follows it.

## Why

Measured against Godot's scene/gui, Lunarsong's XML+CSS UI, AUI and RmlUi, our UI
is missing the things that make a toolkit feel great rather than good: motion on
state changes, a complete box model on every view, a cascade that behaves like
CSS (specificity order, inheritance, variables, units), flex wrap + gap, and text
ellipsis. The per-child LayoutParams shape also blocks two of those: layout
attributes cannot be styled from a sheet or written in markup without knowing
the parent's type.

## Rulings

1. **Layouts are CORE.** Absolute, Dock, Flex, Flow, Frame, Grid ship with the
   engine. There are no per-layout parameter TYPES. The extension point for an
   exotic layout is a custom container (a ViewGroup that positions its children
   from the uniform properties) plus CUSTOM PROPERTIES (`--name`, below) for the
   rare per-child value the uniform set lacks.
2. **One uniform `LayoutStyle` value ON EVERY VIEW** replaces `RefPtr<LayoutParams>`
   and its five subclasses. Plain data, no allocation, contiguous, read by every
   layout for the subset it honours (what Yoga, RmlUi, AUI and Godot's size
   flags + anchors all do). eepp keeps its layout state on the widget too - the
   "dictionary on the layout" idea is a misremembering and is REJECTED (a hash
   lookup per child per pass, lifetime coupling on removal).
3. **CSS-proximate names everywhere.** Sheet properties, markup attributes and
   the C++ setters use the same names (`flex-grow`, `align-self`, `gap`,
   `grid-column`, `box-shadow`, `transition`). Proximity to real CSS is what
   RmlUi gets right; we take the vocabulary, not the web's layout engine.
4. **One layout, no legacy** (the serializer rule applies to sheets and markup):
   the old parent-typed attributes and the `LayoutParams` classes are DELETED,
   not aliased. Every `.sss` and `.sml` in the tree is rewritten in the same
   commit; the markup reader refuses an unknown attribute loudly.

## Design

### 1. LayoutStyle (P0)

```
struct LayoutStyle            // on View, value, ~96 bytes
{
    SizeSpec width, height;   // Fixed(dp|px|%) / Match / Wrap (unchanged semantics)
    Length minWidth, minHeight, maxWidth, maxHeight;   // Auto | Length
    Thickness margin;
    f32 flexGrow = 0, flexShrink = 1;  Length flexBasis = Auto;
    Optional<Align> alignSelf;
    Gravity gravity;          // Frame/Flow/Dock placement (unchanged)
    Dock dock;                // Dock layout side
    Position position = Static | Absolute;  Insets insets;  // absolute in ANY container
    i32 gridRow = -1, gridColumn = -1, gridRowSpan = 1, gridColumnSpan = 1;
    i32 zIndex = 0;
};
```
- `View::Layout()` (the value) + `View::SetLayout(const LayoutStyle&)` mark
  layout damage through the existing `InvalidationKind` routing (the layout-gate
  split of 2026-09-01 stays intact: `zIndex` and `position` are LAYOUT kinds).
- Container-owned data stays on the container: Grid TRACKS (`Columns`/`Rows`/
  `AutoFlow`/spacing), Flex `direction`/`justify`/`align-items`/`wrap`/`gap`,
  Dock `LastChildFill`. Only per-child PLACEMENT moves to the view.
- Layouts read fields directly; the RTTI cast and `CreateDefaultLayoutParams`
  vanish. `MarkupRegistry::RegisterLayoutParam` and the parent-tag lookup go;
  layout attributes are ordinary view attributes named as in section 3.
- Custom container: a `ViewGroup` subclass overriding Measure/Arrange reads
  `child.Layout()` and, when it needs more, `child.CustomProperty("--ring-angle")`.

### 2. Box model on every view (P2)

Every view owns a `VisualStyle` (value) the cascade fills: `background` (solid /
gradient / drawable), `border` per edge (width, color), `border-radius` per
corner, `box-shadow` (offset, blur, spread, color; inset variant), `opacity`
(composited, not per-vertex), `overflow` (visible | hidden | scroll), `cursor`,
`visibility`. Draw order within a container = `z-index` then child order.
Opacity and clipping compose through the VG batch's existing clip stack; shadow
is a blurred rounded-rect drawn by the VG renderer (a new VG draw mode, ONE
shader - the VG DF blur path is the seam).

### 3. Sheet vocabulary + cascade (P1)

- Selectors: type, `.class`, `#id`, `[attr=value]`, pseudo-classes `:hover
  :active :focus :focus-visible :disabled :checked :selected :first-child
  :last-child :empty`, pseudo-elements the controls already expose,
  combinators descendant and child `>` (sibling combinators are OUT).
- Cascade: collect every matching rule, sort by (specificity, source order),
  apply declarations in that order - last wins PER PROPERTY. Replaces the
  per-property best-specificity pick. Computed style is cached per view and
  invalidated by class/pseudo-state change (the existing dirty machinery).
- Inheritance: text properties (color, font-family, font-size, line-height,
  text-align, word-wrap) inherit from the parent's COMPUTED value unless set.
  `inherit` and `initial` keywords.
- Variables: `--name: value` on any rule; `var(--name, fallback)`; stored on
  the view's computed style by `StringHash`; `View::CustomProperty(name)` reads
  it (this is ALSO the custom-layout extension point of ruling 1). The Palette
  becomes a root rule of variables.
- Units: `px`, `dp`, `%` (of the containing box on the same axis), `em` (of the
  computed font size); `calc(a + b)` over lengths with one nesting level.
  `Unit` grows accordingly; SizeSpec::Fixed takes a Length.
- Properties (final names): layout - `width height min-width min-height
  max-width max-height margin(-top/-right/-bottom/-left) flex-grow flex-shrink
  flex-basis align-self gravity dock position top right bottom left grid-row
  grid-column grid-row-span grid-column-span z-index`; container - `flex-
  direction justify-content align-items flex-wrap gap row-gap column-gap grid-
  template-columns grid-template-rows padding`; visual - section 2 list;
  text - `color font-family font-size font-weight line-height text-align
  text-overflow (clip|ellipsis) word-wrap`; motion - section 4.

### 4. Transitions (P3)

`transition: <property> <duration> [<easing>] [<delay>], ...` and
`transition: all 120ms ease-out`. On a computed-style change from a
pseudo-state or class toggle, each listed animatable property (colors, opacity,
lengths, radii, shadow, transform offsets) runs through a `Storyboard` from the
old computed value to the new one; a re-trigger mid-flight retargets from the
CURRENT value (no snap). Layout-kind properties animate through the existing
property routing (they mark layout damage per frame - allowed, but the UA sheet
only transitions VISUAL kinds). The UA default sheet ships `transition: all
120ms ease-out` on the interactive controls; themes may override.

### 5. Flex wrap + gap, ellipsis (P4)

`flex-wrap: nowrap | wrap`, `gap` (both axes), `flex-basis`; lines pack by
`align-content`. `text-overflow: ellipsis` on single-line text views measures
with the font's ellipsis glyph and clips; `word-wrap` keeps today's behaviour.

### 6. Markup

`.sml` attributes ARE the sheet names (`flex-grow="1"`, `align-self="center"`,
`grid-column="2"`, `style="..."` inline). Unknown attribute = load error naming
the element and the attribute. The `MarkupCompletion` provider (toolkit) reads
the same vocabulary table, so completions never drift from the parser.

## Phases (each battery-green, independently landable)

- **P0 - LayoutStyle.** The struct on View; the six layouts read it; delete the
  five LayoutParams classes, `CreateDefaultLayoutParams`, `RegisterLayoutParam`;
  rewrite every `.sml` and every C++ `MakeRef<...LayoutParams>` site in the tree
  (Editor.*, Samples, Tests). Tests: each layout's existing cases pass unchanged
  through the new fields; a custom-container test positions children from
  `Layout()` + a custom property; a reparent test keeps a child's intent.
- **P1 - cascade + vars + units.** Ordered cascade, inheritance, variables,
  units + calc, the attribute selector and the added pseudo-classes. Tests:
  specificity order over 3 competing rules; inheritance through 3 levels;
  var() with fallback; calc; percent against the containing box.
- **P2 - box model.** VisualStyle + shadow/opacity/overflow/z-index/position
  absolute. Tests: draw-order by z-index; an absolute child inside a Flex; a
  shadow emits the DF quad with the right bounds; opacity composes children.
- **P3 - transitions.** Property parsing, the retargeting storyboard, the UA
  sheet defaults. Tests: hover in/out retargets mid-flight; a class toggle
  animates only listed properties; duration 0 = instant.
- **P4 - wrap, gap, ellipsis.** Tests: wrap line count and align-content; gap on
  both axes; ellipsis width never exceeds the box.
- Visual iteration WITH the user at the desk after P2 and P3 (the import dialog
  and the inspector are the yardsticks - both were called "disliked").

## Files (P0 anchors)

`UI/Core/View.cppm` (LayoutParamsPtr -> LayoutStyle), `UI/Layout/*.cppm` (six
layouts + delete LayoutParams.cppm), `UI/Markup/MarkupRegistry.cppm` + `MarkupLoader.cppm`,
`UI/Styling/StyleSheet.cppm` + `StyleSelector.cppm` + `Parser/SSSParser.cppm`,
`UI/Animation/Storyboard.cppm`, `UI.Toolkit/MarkupCompletion.cppm`, every
`.sml`/`.sss` under Data/ and Code/Editor/*, the theme sheets from
ui-theme-migration.md.

## Out of scope (follow-ups, in this order)

Accessibility tree (Godot-level), right-to-left + shaping, data binding (our
adapters stay), in-game sheet hot reload, sibling combinators, CSS grid
templates beyond track sizes.

## Gotchas

- The layout-gate split assumes state rules are colour-only today; after P1 a
  state rule MAY change geometry, so visual-vs-layout classification must come
  from the PROPERTY's InvalidationKind, never from the rule kind.
- One layout, no legacy: no attribute aliases, no LayoutParams shim - a stale
  `.sml` fails to load and says why.
- Beef: the UI port waits for P0-P4 here; porting the old model first would be
  double work.
