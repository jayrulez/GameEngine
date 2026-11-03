# UI core audit - good to great (Foundation/UI)

> ARCHIVED 2026-09-01: still-open items live in Documentation/Plans/week-2026-09-05.md
> ("Backlog folder absorbed"). This archive keeps the full detail.


> Fable, 2026-08-15. Four-lens audit (box model/layout, styling, focus/input,
> cross-cutting architecture) of Code/Foundation/UI only - toolkit and leaf
> libs deliberately out of scope. Every claim below was verified against code
> with file:line evidence by the audit pass; anchors kept for the load-bearing
> ones. This document is the synthesis + the prioritized plan.

## Verdict in one paragraph

The library is genuinely good where it counts: virtualization (ListView/
GridView/TreeView over FlattenedTreeAdapter + ViewRecycler) is architecturally
sound at 10k rows; the manager layer is dangling-safe by ViewId everywhere;
the Editing stack, Markup, Animation, and the draw/hit-test transform stack
are complete and correct; there are ~11k lines of real tests. What keeps it
from great is that three foundational contracts were never made canonical:
the BOX MODEL (four competing definitions), the FRAME PIPELINE (dirty
tracking exists but nothing reads it - full re-measure/re-style/redraw of
every window every frame), and FOCUS/MODALITY (focus is correct but the
visual and the modal keyboard scope were never separated from it). Styling
is NOT the problem it appears to be - see Q2.

## The owner's three questions, answered

### Q1: "measurements with and without borders; consistency of sizing"

Confirmed, and it is structural: **there is no canonical box model - there
are at least four.**

- Border is PAINT-ONLY. No measure path reads StyleProperty::BorderWidth;
  VG strokes are centered on the bounds edge (VG/Context.cppm:513-516), so
  half of every themed 1px border lies outside the element; ClipsContent
  clips it off (Core/View.cppm:899); content padding never reserves for it.
- Padding has THREE channels with disjoint consumers: ViewGroup::Padding
  (containers + markup), StyleProperty::Padding (leaf controls only - a
  stylesheet `padding:` on a Panel silently no-ops), Drawable::
  DrawablePadding (consumed by Panel alone - nine-slice chrome reserves
  space in Panels but not Buttons).
- SizeSpec (Fixed/Match/Wrap) is honored by 3 of 8+ containers; `width=
  "100px"` works in FrameLayout and silently becomes wrap-content in
  Dock/Flow/Grid/Panel. Two containers double-apply DPI to Fixed sizes
  (MakeChildConstraints + FlexLayout resolve Dp with DpiScale INSIDE
  logical space, then draw scales again); AbsoluteLayout resolves with 1.0
  (correct). VERIFY on a 2x monitor - at 1x both paths coincide, masking it.
- Fill-style leaves (Separator/ProgressBar/Slider/ScrollView...) measure to
  MaxWidth/Height and explode to kFloatMax under unbounded parents
  (FlowLayout/GridLayout measure children with Expand()).
- Margin honored by 5 containers, ignored by 4 (incl. base ViewGroup, which
  measures children but never arranges them - inherits empty OnLayout).
- No layout rounding anywhere; StrokeRoundedRect is an unsnapped path
  stroke - the blurry 1px borders on themed controls are exactly this.

**THE FIX (the audit's biggest structural item): adopt border-box with a
single chrome-resolution point in View.**

1. `BoxMetrics { margin, padding, border }` resolved ONCE per pass in a
   protected View helper: margin from LayoutParams; padding = max(field,
   style, DrawablePadding) - Panel::EffectivePadding's max-merge hoisted to
   the base (it is the right idea, applied in one class today); border from
   style.
2. `View::Measure` becomes a non-virtual template method: deflate margin,
   apply the view's OWN SizeSpec (self-side, dpi=1 logical - kills the three
   rival parent-side helpers AND the DPI double-scale), deflate
   padding+border, call new `OnMeasureContent`, re-inflate + clamp.
   Every container honors Fixed sizes for free; parents only place boxes.
3. `View::Layout` subtracts margin once and rounds Bounds to the device
   grid; borders draw INSET so paint stays inside bounds.
4. Migration is mostly DELETION: ~30 controls drop hand-rolled deflate
   boilerplate, 6 containers drop their spec-switch blocks. FrameLayout is
   the behavioral reference (the only container composing all three
   correctly today, minus the DPI bug). LayoutTests golden values will
   churn - that churn IS the spec change; review it, don't avoid it.

Keep untouched: BoxConstraints itself, the DrawChildren transform/clip
stack + its HitTest inverse, VG's device-space snapping, ScrollView's
reserved-mode two-pass measure.

### Q2: "styling is very bespoke - is going closer to CSS better?"

**No - because the styling system already IS a typed CSS-subset, and a good
one.** Typed StyleProperty/StyleValue (wrong-typed style = compile error),
type/class/state/pseudo-element selectors with CSS specificity + source
order, three origins (inline > ancestor-local > context) with inheritance
for text props, a text format (.sss) with @palette variables + @import + an
extensible drawable factory (state-list/nine-slice/svg - richer than CSS
background), cooked UITheme assets, 7 test files, and a shipping 290-line
Data/Assets/ui/themes/breeze.sss proving expressiveness.

What actually feels bespoke is that the codebase BYPASSES its own system:

- The four built-in C++ themes are 1,769 lines of structural clones -
  DarkTheme vs LightTheme differ by ZERO rules (86 identical Set calls,
  different literals). Adding a theme = copying ~400 lines of C++;
  every tweak is a rebuild.
- Controls carry inline fallback defaults at 102 ResolveStyle call sites
  (EditText repeats Thickness{6,4} SEVEN times) - there is no default
  "user-agent sheet", so a theme omitting a property falls to whatever
  each call site says.
- Hand-rolled hover/pressed/disabled ladders with hardcoded DARK-theme
  literals in ~10 control files - theme-blind fallbacks.

**THE FIX: finish eating the dog food, do not adopt CSS.**

1. Port the built-in themes to .sss (breeze is the template); keep
   ThemePalette as @palette seed; keep IThemeExtension for the few things
   text can't express (editor's runtime-baked icon atlas). Kills ~1,500
   duplicated lines; theme edits become rebuild-free (the game path
   already hot-swaps sheets).
2. Add a lowest-priority built-in UA sheet; delete per-call-site defaults.
3. Route control fallback ladders through StateListDrawable + UA rules;
   controls stop calling Palette::Compute* directly.

Full CSS is not justified (nothing is blocked on combinators; it would
trade away typed properties and demand a real invalidation story). The
Experimental GUI's CSS engine is a decent implementation of the WRONG
model for this tree (push-styles-through-setters - styles fight program
state; raw-pointer style cache violating the versioning rule; whole-subtree
re-apply for :hover). Lift ideas, never the code. Preserve at all costs:
typed properties, RTTI type selectors, pull-based resolution, StateList
flag-stripping fallback, the drawable object model, the cooked asset path.

### Q3: "focus after click + restore after modal - not retain, or not draw?"

**Retain focus; gate the VISUAL (focus-visible semantics).** Click-focus is
load-bearing: WantsTextInput reports off the focused view (no click-focus =
no typing), ListView/Tree/Grid arrow-nav depends on click self-focus, tab
continuity keys off the retained FocusedId. Every mature framework retains
and gates the ring (web :focus-visible, WPF keyboard cues).

Implementation (~30 lines core): `FocusSource { Programmatic, Pointer,
Keyboard }` on FocusManager; producers - FocusNearestFocusable -> Pointer,
FocusNext/Prev/MoveFocus -> Keyboard, rest Programmatic; the focus stack
saves {ViewId, FocusSource} so a modal restore brings a clicked button back
RINGLESS while a Tab-focused one keeps its ring (the exact complaint dies
there). One consumer helper `View::IsFocusVisible()` (text-input views
always show); switch the three ControlState producers (View, ButtonBase,
ToggleButton GetControlState) from IsFocused to IsFocusVisible; leave
EditText/NumericField border+caret on IsFocused. Hosts redraw every frame,
so zero invalidation plumbing today - but also Invalidate() in SetFocus
while there, to survive redraw gating later.

**The audit found something WORSE than the ring while in there - modal
keyboard leak:** popups are not children of PopupLayer's collected tree, so
with a modal Dialog open, Tab cycles BACKGROUND views behind the backdrop
and Return can activate a background button through the modal; the dialog
itself opens keyboard-dead (Escape only works after clicking inside).
Fix in the same batch: GetFocusRoot() returns the topmost focus-pushing
popup; CollectFocusable enumerates popup entries (modals trap Tab AND
dialog buttons become reachable); Dialog::Show sets initial Programmatic
focus (ContextMenu already does); tie focus-stack entries to their popup
(kills out-of-order-close cross-restore); validate restore targets
(focusable + enabled + visible).

## Cross-cutting findings (the fourth lens)

1. **The invalidation system is decorative.** Invalidate() sets flags NOTHING
   reads (zero NeedsRedraw consumers); UIRuntime re-measures, re-styles,
   re-shapes, and redraws every window every frame. Idle editor burns CPU
   proportional to total docked-panel complexity. Per-frame churn on top:
   Label re-shapes text every measure AND re-allocates line splits + ellipsis
   String every draw (EditText's m_glyphsDirty cache is the house pattern
   Label never got); ListView REBINDS every visible row every layout pass;
   ComboBox measures every item per measure; style resolution is an uncached
   three-origin walk per property per draw.
2. **Event/Property bindings cannot unsubscribe.** Event has Add/Clear only;
   Property::BindTo captures raw Property* both ways - either view dying
   first leaves the survivor firing into freed memory. The mutation queue
   captures raw View* (QueueDelete also never resets IsPendingDeletion -
   a removed-not-destroyed view becomes immune to future queued removals).
3. **The mutation-queue rule is caller discipline, not architecture** -
   RemoveView is freely callable mid-dispatch; input bubble walks a
   pre-built raw-pointer chain. (The ViewId-based manager layer is safe;
   dispatch-in-progress is not.)
4. **SelectionModel stores flat indices** that nothing remaps on data
   change: collapse a tree node above your selection and the highlight
   jumps rows. TreeView's events are nodeId-based but its selection is
   positional.
5. **LocalToScreen/ScreenToLocal ignore ViewTransform** while HitTest
   honors it - clicks mis-map inside any scaled/animated subtree.

## The plan - priority order

**P0 - correctness batch - SHIPPED 2026-08-15 (Fable):**
FocusSource (Pointer/Keyboard/Programmatic) + View::IsFocusVisible; the
three ControlState producers gate the ring on it (text-input views always
show); focus save/restore moved INTO PopupEntry (SaveAndClearFocus/
RestoreFocus - out-of-order closes cannot cross-restore; restore validated
against destroyed/disabled/hidden targets and carries the original source,
so a clicked button returns from a modal ringless); GetFocusRoot scopes to
the topmost focus-taking popup (modals trap Tab AND popup content became
keyboard-reachable); Dialog::Show sets initial Programmatic focus (dialogs
open keyboard-alive - Escape works immediately) + Dialog IsFocusable;
SetFocus/ClearFocus Invalidate (redraw-gating-proof); draw-phase tree
mutations DIAGNOSTIC_ASSERT (layout-phase stays legal - virtualization);
the three Queue* lambdas capture RefPtr (dangling-capture class killed) +
QueueDestroy/QueueDelete reset IsPendingDeletion; SelectionModel::
PruneFrom on NotifyDataChanged + TreeView expand/collapse remaps selection
by nodeId (ToggleNodePreservingSelection + FlattenedTreeAdapter::
PositionOfNode). ~10 new UI.Tests cases; 714 green; full battery 242 runs
x 0 fails. NOTE: gamepad MoveFocus routes as Keyboard - pad nav draws
rings, as it must. GAP LOGGED: no Systems doc covers foundation.ui core -
write one when this track completes.
DEFERRED from P0: collapsing the three Queue* spellings into one (API
churn across consumers - fold into P4's connection-token work).

**P1 - quick perf wins:**
- SHIPPED 2026-08-15 (P1a, Fable): ListView rebind-every-frame branch
  deleted (GetOrCreate binds on acquire; OnItemRangeChanged/OnDataSetChanged
  cover data changes); Label gets the EditText treatment - two value-keyed
  ShapedCache entries (measure-side + draw-side so differing widths never
  ping-pong), lines borrow the cache's own text copy, ellipsis string
  cached; ComboBox max-item-width cached (items generation + family/size
  value keys - never font pointers); TabView title widths cached
  (EnsureTitleWidths; RebuildTabRects runs per layout AND per draw).
  Shaping paths execute only with a real font service, so coverage is the
  render-level suites; unit tests hold the null-service paths.
- P1b/P4 DAMAGE GATE SHIPPED 2026-08-16: frame-scoped gate in UIHost - when
  nothing invalidated and no window resized/rescaled, Update skips layout
  and RenderWindow re-encodes the RETAINED VG batch (present pipeline
  untouched, no app-loop changes). Producer sweep: hover on/off
  invalidates, animations + focused-text caret mark in BeginFrame, drag
  adorner marks, ListView long-press self-chains (momentum already chained
  via ScrollBy), ViewportView keeps live-3D windows damaged (refine with
  render-side damage later). Resize/DPI auto-detected as structural.
  Escape hatch SetDamageGatingEnabled(false) + FramesDrawn/Skipped
  counters. Layout dirty stays COARSE (any invalidate = relayout+redraw -
  interactions never worse than the old every-frame behavior; idle = zero
  tree work). Game UISubsystem path unchanged (games animate constantly).
  Remaining P4 items: ShapedTextBlock, Event/Property connection tokens,
  Queue* collapse + deleteChild param drop.
- SUPERSEDED (P1b original note): consume NeedsRedraw in UIRuntime::RenderWindow. NOT the
  one-file change it looks like - needs the producer sweep first: animator
  field writes, hover-change invalidation, EditText caret blink, ListView
  OnDraw momentum/long-press timers (move to BeginFrame), scrollbar fades.
  Do it WITH instrumentation + an escape-hatch toggle for dogfooding.
- DEFERRED: dropping the advisory deleteChild params (signature churn
  across all consumers - bundle into P4).
- NOTE: Button/CheckBox per-draw truncation still uncached - fold into the
  shared ShapedTextBlock (P4), which subsumes these per-control caches.

**P2 - the box model** - SPEC LOCKED (Documentation/Archive/ui-box-model-history.md)
+ **CRISP CHROME FRAMEWORK-WIDE 2026-08-16** (user-spotted: baked icons were
editor-only WIRING - ThemeIconSet in foundation.ui shares one bakeable
BakedSVGDrawable per chrome glyph (+ tinted variants); all 4 themes acquire
from it; UIHost initializes the set, lazily bakes at first window attach,
BakeThemeIcons(scale) for DPI rebakes; editor rebakes both sets together.
Headless/tests keep unbaked fallbacks - no lifetime games.)
+ **P2d SHIPPED 2026-08-16 - P2 (BOX MODEL) COMPLETE** (device-grid rounding
in base Layout, edges independently; VG StrokeRoundedRect crisp-snap; the
viewport DPI golden updated deliberately - integral RT regions at 1.25x.
Measure dirty flags + wrap-remeasure RE-SCOPED to the P1b/P4 damage unit -
ListView virtualization depends on per-frame relayout, one producer sweep).
+ **P2c batch 2 SHIPPED 2026-08-16** (BoxConstraints::IsBounded/BoundedMax* -
the ONE unbounded test; all 7 fill-style leaves on bounded defaults
(Separator/ProgressBar/Slider/ScrollBar/ListView/GridView/Expander);
FlexLayout's two `< 100000` tests unified; FlowLayout's 100000 sentinel
becomes honest kFloatMax. P2c is COMPLETE except stragglers found in P2d.)
+ **P2c batch 1 SHIPPED 2026-08-16** (DefaultStylePadding fallback seam - the
per-call-site inline defaults stated once per control; Button/ToggleButton/
ContentButton/IconButton/EditText on OnMeasureContent; EditText's SEVEN
inline {6,4} sites -> one override + ContentInset(); IconButton padding bug
fixed - measure reserves what draw insets. Remaining P2c batch 2: fill-leaf
bounded defaults + the rest of the leaves).
+ **P2b SHIPPED 2026-08-16** (the contract cut: base Measure template method
owns margin + Fixed at logical dpi; Px = physical px; margin-box Layout;
AvailForChild replaces the three spec-interpreter clones; all 8 containers
migrated; ViewGroup measure/arrange asymmetry fixed; acceptance tests green -
Fixed works in EVERY container, Flow honors margins, Grid leaves bounded).
+ **P2a SHIPPED 2026-08-15** (BoxMetrics + three-channel padding merge + inset
border strokes + DrawablePadding on RoundedRectDrawable + Panel delegation;
style padding now works on containers; 4 new BoxMetricsTests). P2b-P2d
phased per the spec. Original sketch:
border-box + BoxMetrics + template-method Measure/Layout per Q1. Verify
the HiDPI double-scale on a 2x monitor first (it decides how loud the
release note must be). Golden-test churn reviewed deliberately.

**P3 - styling dogfood:** themes to .sss + UA default sheet + state-ladder
removal per Q2. Independent of P2; mostly deletion; each step shippable.

**P3 SCOPING INVESTIGATED 2026-08-16** (user asked how sss-only works when
"icons/images are in C++"). VERDICT: the remembered blocker dissolves -
icons are inline SVG STRINGS referenced BY NAME, and .sss already has an
`svg(name, tint=$c)` factory; "names live in C++, sss references them" is
the correct end state, not an obstacle. breeze.sss already covers every
icon slot the C++ themes register. TexturedTheme embeds NO images (it is a
generic factory over caller-supplied images; only UISandbox uses it, with
PROCEDURAL pixels) - keep it C++, exclude from migration. Gap table (all
small except loading):
1. [S, REAL BUG] `svg(name)` silently NULLS for the 10 builtin glyphs at
   runtime/cook (only UISandbox registers names) AND returns fresh unbaked
   vectors - fix both by falling back to ThemeIconSet::Acquire in the
   factory + cook-FAIL on unresolved names.
2. [S] per-corner radii in rounded-rect()/state-rounded() (spin buttons).
3. [S] disabled()/focused() color functions (hover/pressed = lighten/darken
   already; state-colors()/state-rounded() ramps already call Palette).
4. [S-M] toolkit control types into UITypeRegistry (prereq for a
   toolkit.sss fragment; the IThemeExtension hook itself stays).
5. [M] LOADING SHAPE DECIDED: EMBEDDED STRING (author real .sss in-tree,
   embed at build) - cooked assets are chicken/egg for editor+project-
   manager theming; file-next-to-binary repeats the DXC-sidecar staging
   lesson. Theme Create(palette) signatures keep working (SetPalette +
   Load(embedded)).
STAYS C++ (fine): ThemeIcons strings + ThemeIconSet bake machinery (the
backing store), ThemePalette presets, clear color, font setup, the
editor's post-parse code overrides (a parsed sheet is the same StyleSheet
object - injection keeps working).
PLAN: Phase 0 = gaps 1-3 (2-3d, fixes the live svg-null bug); Phase 1 =
dark.sss embedded + rule-diff parity test + delete the C++ body
(GameTheme/GameLightTheme come free as palette reskins); Phase 2 = light
(tinted path) then rounded-dark (editor's live theme - visual verify);
Phase 3 optional = toolkit.sss via MergeFrom. Total ~5-8 days. Still
USER-DEFERRED - this is the scope, ready when un-deferred.

**P4 - structural (after P1 proves the seams):** damage-driven frame
pipeline (route the existing-but-discarded InvalidationKind into
relayout/redraw context flags; move ListView's OnDraw-as-tick timers to
BeginFrame); shared ShapedTextBlock cache type; RAII connection tokens for
Event/Property (prerequisite for any richer editor data-binding).

**Untested areas worth tests when touched:** DragDropManager (design is
complete, coverage is an accessor check), TooltipManager, Debug/ overlay,
and the UIRuntime frame loop itself (its lack of tests is how the dead
NeedsRedraw went unnoticed).

## Do not touch (verified solid)

Virtualization stack (ListView/GridView/TreeView/ViewRecycler), ViewId
manager safety + Unregister fan-out, Editing stack (TextEditingBehavior/
UndoStack/EditText glyph cache), Markup loader+registry (diagnostics-
friendly, tested), Animation manager (reentrancy-safe), DragDrop design,
RefPtr tree ownership + MoveView + popup-layer-last invariant,
BoxConstraints, the DrawChildren transform stack, VG device-space snapping.

## Styling-bypass audit (2026-08-16, consistency pass)

Full sweep of UI + UI.Toolkit for styling the theme system cannot reach
("all styling goes through themes"). Tier B (ResolvePart*/ResolveStyle*
fallback constants that fire only when no sheet rule exists) is FINE while
the shipped sheets cover every such path - the P4 UA sheet centralizes
them later. FIXED in the pass: MenuBar / BreadcrumbBar / ContextMenu /
Toolbar-button / DockablePanel-header font sizes now resolve
StyleProperty::FontSize (rules were dead before); .contextmenu sheets set
font-size 14. Verified false alarms: Dialog / TooltipView / ComboBox-
dropdown "hardcoded borders" only draw on the no-theme fallback path.

**FIXED in the toolkit fixup batch (post-audit):** CurveCanvas +
GradientEditor + DockDragPreview now resolve background (raw color via
background-color) / border / dim-text from the theme and are registered
selectable (tripwire 16); ToastHost severities resolve the NEW
success-color/warning-color/error-color style properties (View rules map
them to the palette in every core sheet); the View-16 font inheritance
regression (newly-wired FontSize resolves picking up the base rule) is
pinned by explicit 12s on DockablePanel/Toolbar/BreadcrumbBar + gates.
Semantic constants stay by design: key/tangent handle colors, VectorFields
axis colors (red X / green Y / blue Z), selection-yellow markers.

**ALSO FIXED (second fixup batch):** DockZoneIndicator arrow resolves
TextColor (alpha ramp preserved); DragAdorner ghost + ModalBackdrop scrim
resolve Background (background-color rules; field/code defaults remain
the fallback); RadioButton gained the ::box width part hook (CheckBox
parity); CodeEditView gutter markers + error-line tint resolve the
semantic ErrorColor/WarningColor properties (resolved once, outside the
line loop).

**Remainder - accepted / needs framework features:**
- CodeEditView FontSize/FontFamily plain fields + syntax token palette:
  BY DESIGN for now (field API is page-owned; a .sss syntax-palette
  needs a property family of its own).
- Child-view text is unreachable by container rules (no descendant
  selectors / style inheritance): StatusBar section labels,
  PropertyGrid row labels (12); Toolbar buttons resolve via their
  Toolbar ancestor instead.
- NodeGraphCanvas title/subtitle/port font sizes + geometry constants;
  NodeGraphTypes per-node colors are caller-owned by design.
