# The GUI lab's evolution: a core that can stand where foundation.ui's does

> STATUS: APPROVED 2026-10-16 (user: "Let's build it"). Follows `vg-and-gui-lab.md` (G1 to G7
> done on `vg`). Read CONVENTIONS.md first.

## Why

On 2026-10-16 the lab was measured against foundation.ui (its core systems, and its controls by
function rather than by name), Godot, eepp, RmlUi, GPUI, AUI, Zero and the UI systems of traktor,
LunarSong, Lumix, ezEngine and doriax. The assessments are kept locally, not in the repository
(`Code/Experimental/GUI/Assessments/`, `README.md` the index). They agree on one picture:

- The lab is behind on the foundations: nothing measures itself (a label never fits its text,
  margins are stored and never read), events reach one node and stop, text editing is thin,
  popups are one at a time and placed blindly, and its controls are thinner than foundation.ui's
  nearly everywhere.
- The lab is ahead on the layer games feel: navigation by keys and pad with scopes and memory,
  commands along the focus path and a keymap with contexts and chords, bindings that track what
  they read, CSS motion and effects, several roots on one clock, and scripting through reflection.

The bugs the assessments found are fixed (lab and foundation.ui, 2026-10-16). This document is the
design for the rest: closing the gaps against foundation.ui without porting it, so the lab's core
can stand where foundation.ui's does.

## Principles

- **Not a frankengui.** Each concern gets one mechanism, chosen for the lab and its future, with
  the references as evidence, not as parts to bolt on. A feature that only one framework has and
  no screen of ours asks for is left out.
- **The core is the core.** Editor widgets (docking, property grids, colour pickers, curve,
  timeline and node graph canvases, toolbars, split views, file dialogs) go to a future lab
  toolkit; game screens (screen stacks, toasts, tickers, button prompts, menu lists) to a future
  lab gamekit. Both are libraries over the core's public API that register their element types
  (G1's `ElementTypes`). The core holds what foundation.ui's core holds: the systems and the
  controls in `Foundation/UI/Controls`.
- **CSS-shaped.** Where CSS has a name and a behaviour for something, the lab uses them, so what
  an author knows from the web holds here. Where CSS has a document model the lab does not want
  (block and inline flow, floats, tables), the lab does not copy it.
- **Reflected, scriptable, tested.** Every new property is reflected and reachable from sheets,
  markup and both script languages; every behaviour lands with a test; every phase ends with the
  sandbox showing it.
- **No compatibility shims.** The lab has no users: a design that moves something changes the call
  sites (user ruling 2026-10-16).

## Rulings so far (user, 2026-10-16)

1. Layout: support every layout foundation.ui supports, designed to fit the lab and its future,
   not ported. The design below is the proposal (E2).
2. Events: capture and bubble.
3. Text and editing: a mixture of taking and writing, for the best future of the lab.
4. Data binding stays in code and scripts: no binding attributes or expressions in markup beyond
   the `{{name}}` text templates the lab has.
5. Hosting (a scene component, world-space canvases, reference-resolution scaling) is not a
   concern yet.

On the draft's open questions (user, 2026-10-16): the layout defaults as proposed; scrolling as
recommended, `overflow` on any node; the text engine copied into the lab, not shared; one popup
layer per root, as foundation.ui's `RootView` has; image tests that the user approves; E9's three
parts before 0.1, accessibility through a bridge the host provides. Work starts once this spec is
approved.

## The phases

Each phase is a layer with its tests and a sandbox page or section. The order follows what each
needs: events and layout first, since every widget's behaviour and size rests on them.

### E1 Events: capture, bubble and a hover path

Facts: the dispatcher sends each pointer and key event to one node (`Scene/EventDispatcher.cppm`);
a node's handlers are its `On*` overrides and its listeners; only the wheel walks up, to the
nearest node that wants it. `:hover` matches the node under the pointer only. Script handlers get
no event data. Disabled widgets are filtered since 337969cc3.

Design:

- **Phases.** Pointer, key, text and wheel events travel the path from the root to the target and
  back: a capture phase (root first) and a bubble phase (target first), each listener told its
  phase, any of them able to mark the event handled, which ends the walk. The target sees both.
  The widget's own handling (`OnMouseDown` and the rest) runs at the target in the bubble phase,
  so a parent listening in capture can intercept a child's input (a list taking a drag from its
  rows) and one listening in bubble only sees what the child left. The path is snapshot as weak
  handles before the walk, so a handler that removes nodes does not break it (LunarSong's
  bubbling over instance ids; the lab's handles make this cheap).
- **Keys and text bubble from the focus**: a key the focused widget leaves reaches its ancestors,
  then the keymap, then navigation, the order G7 set, now through the bubble phase instead of
  special cases.
- **A hover path.** The dispatcher keeps the chain from the root to the hovered node; enter and
  leave go to each node that joins or leaves it, and `:hover` matches every node on it, as CSS
  does. `:active` follows the pressed path the same way. (Godot's `mouse_over_hierarchy`, RmlUi's
  hover chain.)
- **Pointer capture.** Explicit `CapturePointer()` and `ReleasePointer()`, with a lost-capture
  event when it is taken away (a popup opening, the node leaving), on top of the implicit capture a
  press gives today.
- **`pointer-events`.** A CSS property, `auto` or `none`: a node with `none` is skipped by hit
  testing and its children are still tested (decorations over controls). With the phases, this
  covers what Godot's stop, pass and ignore filter is for, without a second mechanism.
- **Click count and gestures** in the event: a second press near in time and space is a double
  click, a third a triple, with the thresholds on the context (Zero's root gestures). TextField's
  own double-click detection moves here.
- **Results for the host.** Every `Inject*` returns whether the GUI used the input, and the root
  answers whether a point is over something that takes the pointer, so a game knows when a click
  belongs to the UI.
- **Script handlers get the event**: position, button, modifiers, key, text, click count, phase,
  and a `handled` the script can set.
- **Cursors**: a `cursor` CSS property read from the hovered path, reported to the host.

Tests: capture and bubble order and stopping; a handler removing its own ancestors mid-walk;
`:hover` on ancestors and enter and leave along the path; capture and lost capture;
`pointer-events: none`; double and triple click; `Inject*` results; script handlers reading event
data in both languages.

As built on `vg` (2026-10-16): the route (`Node::RouteEvent`, d8a87b8e2): capture listeners from
the root down, then at each node from the target up its own handler and its bubble listeners;
everything at a node runs and Handled ends the route after it (the DOM's stopPropagation); the
path is held for the walk. The hover and press paths set `Node::IsHovered` and `IsPressed`
(pointer state moved from UINode to Node); enter and leave go to each node joining or leaving.
Explicit capture with `PointerCaptureLost`, released with the press; a click on the nearest node
holding the press and the release; click counts (0.45 s, 6 device pixels) on the dispatcher's
clock; `Node::TakesPointer` (a painted box, focus, or pointer listeners) behind every `Inject*`
result and `IsPointerOver`. `WantsWheel` is gone: scrollers mark the wheel handled only when they
moved, and act on keys only when focused. `pointer-events` and `cursor` are reflected with their
CSS names, the dispatcher answers `GetCursor`, and the shell bridge applies it and reports use
from `Dispatch` (1da80863d). Script handlers get a constructible `GuiEvent` value and take the
event by returning true (cac74aaff). The sandbox's Input page (bc1ad7a4a).
Left: drag and drop still goes to its target alone (E7); Escape closing a popup is still the
dispatcher's special case (E5); the first hover restyles the hovered top-level node's subtree
(E3's restyle cost); the click thresholds are device pixels, not scaled; the sandbox has no plain
container element, so its page uses layouts for boxes (E2).

### E1b Touch

Facts (2026-10-16): the shell turns fingers into `TouchDown`, `TouchMove` and `TouchUp` events
with an id, a position normalized over the window and a pressure (`SDL3ShellImpl.cpp`), and its
InputSurface maps them into the content it fits. Neither foundation.ui's bridge nor the lab's
forwards them (`UiInputBridge.cppm`: "gamepad / touch not routed to the UI"); a tap reaches both
only as the mouse events SDL makes from it by default.

Design (user, 2026-10-16: the shell forwards touch through the input bridge):

- **The first finger is a pointer.** The bridge forwards touch to the dispatcher, which drives
  the same route as the mouse: a press, moves, a release and a click, the events marked as touch
  (a pointer type and a finger id on the event and in a script's `GuiEvent`), so every widget
  works without knowing. Other fingers are ignored by the core; gestures of several fingers wait
  for a screen that asks (a gamekit concern).
- **No hover without a finger.** A finger hovers only while it is down: the hover path follows it
  and empties when it lifts, so `:hover` does not stick after a tap and no tooltip appears.
- **Tap tolerance.** Click counting takes a wider distance for touch than for the mouse.
- **Cancel.** `CancelPointer()` ends the press in progress: the pressed node hears
  `PointerCancel` (a widget drops its drag), no click follows, the press path and capture clear.
  A scroller that starts panning under a finger calls it (E6).
- **SDL's mouse copies are dropped.** While the bridge forwards touch, mouse events that arrive
  with a finger down, or after it at the finger's last place, are taken for SDL's copies of the
  touch and ignored; a mouse that moves elsewhere is the mouse again.
- **The on-screen keyboard** follows text-field focus through the bridge's text input, as now.

Tests: a tap is a press, a release and a click marked as touch, with no hover left behind;
moves follow the finger; a second finger is ignored; double taps within the wider distance;
`CancelPointer` mid-press; the bridge forwarding touch from events and from a surface, and dropping
the mouse copies.

As built on `vg` (2026-10-16): the dispatcher's `InjectTouchDown`, `Move` and `Up` share the
mouse's pointer paths, with `PointerType` and the finger's id on each event and `pointer` in a
script's `GuiEvent`; taps count within 24 device pixels (clicks 6); `CancelPointer` and
`PointerCancel`, which Slider, ScrollBar, a window's drag handles and TextField handle (33edce325).
The bridge forwards touch from `Dispatch` (placed with `SetWindowSize`) and from a surface's polled
fingers, and drops SDL's mouse copies (18b6df7f8). Not yet tried on a touch screen.
Left: long press (E5) and panning (E6).

### E2 Measure and arrange

Facts: no node measures itself; layouts place children at the sizes they have; `LinearLayout`,
`FlexLayout` (no wrap, shrink, basis property or align-self; grow from C++ only), `GridLayout`
(equal columns, no spans) and `RelativeLayout` (anchors from C++ only) are the containers; margins
are never read; `visibility: hidden` removes a node from layout; the lab schedules layout per dirty
container, parents first, which foundation.ui does not.

Design, one protocol and a few layout kinds:

- **The protocol.** Every node answers `Measure(constraints)` with the size it wants within a
  minimum and maximum width and height, then is placed with `Arrange(rect)`. The base class owns
  everything every layout would otherwise repeat: margin, padding, border, `width` and `height`,
  `min-*` and `max-*`, `box-sizing`, and the units (px, %, em, rem, vw, vh, and `calc()` of them).
  A widget supplies only its content's size through `MeasureContent(available)`: a label its
  shaped text wrapped to the available width, a button its label plus its padding, an image its
  pixels, a text field one line of its font. A size is `auto` (the content's size, or what the
  parent's layout gives), a length, a percentage of the parent's content box, or `fit-content`;
  "fill the parent" is the layout's business (`flex-grow`, `align-self: stretch`, a grid cell),
  as in CSS. (foundation.ui's contract, with AUI's and traktor's shape: a preferred size within
  a hint.)
- **Caching and damage.** A node's measure is cached against its constraints and kept until its
  content, style or children change; each style property is declared layout-affecting or visual
  in the property table, so hover, press, focus and colour changes never measure or arrange. The
  dirty-container scheduling stays: a node of definite size whose content changes arranges its own
  children without disturbing its parent.
- **Device pixels.** Arranged edges round to the device grid of the root's display scale, so
  borders and text sit on whole pixels at any scale.
- **Layout kinds, chosen by `display`**:
  - `flex`, by the CSS rules: `flex-direction` (with reverse), `flex-wrap`, `justify-content`,
    `align-items`, `align-self`, `align-content`, `gap`, `row-gap`, `column-gap`, `flex-grow`,
    `flex-shrink`, `flex-basis`, `order`, and baseline alignment (each node reports a baseline: a
    text node its first line's, any other the bottom of its content box). It absorbs
    `LinearLayout` (one direction, no wrap) and foundation.ui's Flow (flex with wrap).
  - `grid`: `grid-template-columns` and `-rows` with px, %, `fr`, `auto`, `minmax()` and
    `repeat()`; `grid-column` and `grid-row` with spans; `grid-auto-flow` row or column; gaps;
    `justify-items` and `align-items` with per-cell `justify-self` and `align-self`. Named areas
    wait for a screen that asks.
  - `stack`: children laid over each other in the content box, each aligned by its
    `justify-self` and `align-self` (start, end, center, stretch). It replaces foundation.ui's
    Frame and covers most of what anchors are for.
  - `dock`: children docked by a `dock` property (left, top, right, bottom, fill) in child order,
    the last filling what remains. It is what editors and HUD frames want, and it is small.
  - `none`: the node and its subtree take no space and do not draw.
- **Positioning on top of any kind.** `position: absolute` takes a node out of its parent's flow
  and places it by `left`, `top`, `right`, `bottom` (lengths or percentages of the parent's
  padding box; left and right together stretch it), which covers foundation.ui's Absolute layout
  and the lab's RelativeLayout. `position: relative` offsets a node after layout without moving
  its siblings. `z-index` orders siblings for drawing and hit testing.
- **Visibility.** `visibility: hidden` keeps the node's space and stops it drawing and taking
  input; `display: none` removes it from layout (AUI's three flags, under CSS's two names).
- **Scrolling is `overflow`, on any node.** `overflow` (and `overflow-x`, `overflow-y`):
  `visible`, `hidden` (clip to the padding box), `scroll` (always scrollable, bars shown) or
  `auto` (scrollable with bars when the content overflows). A scroll container measures its
  content unbounded on its scrolled axes, keeps a scroll offset, takes the wheel, keys and the pad
  stick, and brings a focused descendant into view (G7, now for every scroll container); its bars
  are parts (`::scrollbar`, `::scrollbar-thumb`, `::scrollbar-track`) styled like any other, with
  `scrollbar-width` and an overlay mode. A flex or grid container scrolls itself, with no wrapper.
  `ScrollView` stays as a markup element meaning a node with `overflow: auto`, not a class of its
  own; its internal content node and `GetChildHost`, and `auto-measure`, go.
- **Defaults.** A plain container is `display: flex; flex-direction: column; align-items:
  stretch`, which reads like a CSS block (children stacked, as wide as the parent) while staying
  one algorithm (Yoga's default). `flex-shrink` defaults to 1 with `min-width: auto` meaning the
  content's minimum (a label's longest word), so an overfull row shrinks without crushing text,
  as in CSS. (foundation.ui keeps 0 for its existing screens; the lab has none to keep.)
- **What goes.** `LinearLayout`, `RelativeLayout`, `GridLayout`'s equal columns, `SetChildGrow`,
  `SetAnchor`, the ScrollView class and `auto-measure`; their call sites (the sandbox, the
  showcase, tests) move to the properties above.

Not taken: CSS block and inline flow, floats and tables (inline flow belongs to rich text, E4);
Godot's anchors and offsets as a separate model (insets and `stack` cover them).

Tests: per property, against hand-computed boxes; the CSS flex resolution cases (grow, shrink with
minimums freezing, basis, wrap with align-content, baseline); grid tracks with `fr`, `minmax` and
spans; dock order; absolute insets; `visibility` against `display`; a label and a button sizing
to their text and re-measuring when the text or font changes; measure cache hits on a hover;
pixel rounding at scales 1, 1.25 and 2; scroll containers (`overflow` per axis, content
measured unbounded, offsets clamped, the wheel and keys, a focused descendant brought into view,
styled bar parts, a flex container scrolling itself). A debug overlay for bounds, padding, margin and baselines
comes with this phase, since layout work needs it.

As built on `vg` (2026-10-16): a `LayoutStyle` on every node (sizes as auto, px, %, the content
sizes or none; margin; box sizing; overflow per axis; position and insets; the display kind and
its properties) filled from sheets, markup or code; `Measure(constraints)` cached against its
constraints, `MeasureContent(available)` per widget (a label its wrapped text, a button its label,
a text field a line, the rest a size of their own), `Arrange(rect)` running `OnLayout`, and
`RequestLayout` climbing until a node of definite size; each GUI lays out once a frame, a tree in
no GUI when asked (`UpdateLayout`). Flex by the CSS rules (the automatic minimum is the content's,
no more than the specified size), grid (px, %, auto and fr tracks, `repeat()`, `minmax()`, spans,
auto flow), stack, dock, absolute and relative placement; `visibility: hidden` keeps its space and
`display: none` (code's `SetDisplayed`, a script's `displayed`) gives it up. Scrolling is
`overflow` on any node: the offset moves the children through their transform, with the wheel,
keys, drawn and draggable bars and `ScrollIntoView`; `Panel` is the plain element and `ScrollView`
a node with `overflow: auto`; LinearLayout, FlexLayout, GridLayout and RelativeLayout are gone
(4aa0e07c0, d992d2d58). Baselines: every node answers `BaselineAt(size)` (a label, button, text
field and combo box their first line's, any other the bottom of its content box) and a flex row
lines them up, growing to hold them (38cf9c409). Arranged edges round to the root's display scale,
each edge on its own, with relative offsets and the scroll shift; a new scale lays the whole tree
out again (78a25bd6c). The layout overlay draws border boxes, padding, margins and text baselines
over a root's tree (`SceneRoot::SetLayoutOverlay`; F9 or `--layout-overlay` in the sandbox,
9af53b4a1). The sandbox's Layout page shows all of it in markup (30cb3883f).
Left: a container's baseline is its content box's bottom, not its first child's as in CSS, and
grid and stack place `baseline` as start; text is not snapped to the pixel grid, only boxes;
`z-index` is parsed but does not yet order drawing or hit testing; the scroll bars are not styled
parts and `scrollbar-width` and an overlay mode wait; no `calc()`; a node scrolling on both axes
measures its content unbounded on both, so `fr` columns there take their content's width (scroll
one axis to keep them shared); markup's inline properties apply once and a sheet rule overrides
them (E3 makes inline the highest origin).

### E3 Styling structure

Facts: properties applied through widget setters (size, padding, opacity, visibility, colour,
font) keep their last value when the rule that set them stops matching; `color` and fonts do not
inherit, so six widgets needed a per-widget font hook (8e9964e39); parts take only
`background-color`; there are no sibling, structural, attribute or `:not` selectors; an unknown
pseudo-class is ignored, so a rule with a typo matches more than it should.

Design:

- **Computed style, recomputed whole.** A restyle computes every property a node uses from the
  cascade, defaults included, and the node reads its computed style; setters from code or script
  are the highest-priority origin (inline), not a write that a sheet later fails to undo. A rule
  that stops matching stops applying.
- **Inheritance.** The property table marks the CSS-inherited properties (`color`, the `font-*`
  family, `line-height`, `letter-spacing`, `text-align`, `white-space`, `visibility`, `cursor`);
  `inherit`, `initial` and `unset` work on any property. The per-widget `SetThemeFont` and
  `SetThemeTextColor` hooks go: a widget reads its inherited font and colour.
- **Origins.** A user-agent sheet (the built-in theme, recoloured through custom properties on
  `:root`) at the lowest origin, then the GUI's sheets, then a subtree's own sheet (a page's
  `.css`), then inline. Themes become a user-agent sheet plus a palette of custom properties.
- **Selectors.** Adjacent and general siblings (`+`, `~`), `:first-child`, `:last-child`,
  `:nth-child()` and `:nth-of-type()`, `:empty`, `:not()`, `:is()`, attribute selectors (`[type]`,
  `[type=value]`), and the state pseudo-classes the controls need (`:checked`,
  `:indeterminate`, `:disabled`, `:enabled`, `:focus-within`, `:read-only`, `:placeholder-shown`).
  An unknown pseudo-class makes its rule match nothing (eepp's rule).
- **Parts take every property.** `slider::fill`, `scrollbar::thumb` and the rest take the box,
  text and motion properties, not only a background colour.
- **Restyle cost.** A state change restyles the subtree only when a rule reads that state through
  an ancestor (the sheet records which states its non-subject selectors use); otherwise the node
  alone. (The spec's G4 open limit.)

Tests: a rule that stops matching restores every property it set, setters included; inheritance
and the three keywords; each selector form; an unknown pseudo-class; parts with box and text
properties; restyle counts for a hover with and without an ancestor-state rule.

As built on `vg` (2026-10-17): selectors take `+` and `~`, the structural pseudo-classes with
`:nth-child()` and `:nth-of-type()` (an+b), `:empty`, `:not()`, `:is()` and `:where()`, attribute
tests against the id, the classes and reflected properties by CSS name, `:focus-within`,
`:enabled`, and the widgets' own `:checked`, `:indeterminate`, `:read-only`, `:read-write` and
`:placeholder-shown` (UIWidget::GetWidgetStates; CheckBox gains an indeterminate state, TextField
a read-only one); matching backtracks; an unknown pseudo-class matches nothing; a change of
children restyles the siblings (242d76f5c). The cascade orders by origin: the user agent's sheet,
the GUI's, a subtree's (held weakly by its root, an inner over an outer), then the node's inline
style, which markup's unclaimed attributes fill; the sandbox loads its theme as the user agent's
and each page's sheet for its page (9ef0a66c2). A setter called from code pins its property as
code's (UnpinStyle gives it back); the applier writes in a Node::StyleWriteScope; what no rule
sets returns to the node's own value, its default (widgets write theirs in a scope) or what code
set before a sheet first styled it (7729bbf84). Every node carries a text style (colour, font
with family and size, alignment) that inherits, read by widgets through OnTextStyleChange; the
SetTheme hooks are gone; the table marks the inherited properties; inherit, initial and unset
work (1c0acc07b). A part's style is a box and an opacity, each aspect over the widget's own look
(c86ac3372). A state change restyles the node alone unless a rule reads that state through an
ancestor (its subtree) or a sibling (its later siblings) (ffa899849). Found on the way: a flex
row clamped a non-stretched item's height to the row's (6e8bdd980). The Styling page shows the
selectors, inheritance and parts (e0cb7daee).
Left: parts take no motion (a transition or keyframes on `slider::thumb`) and no text
properties; `inherit` on a property that does not inherit takes the parent's cascaded value, not
its live one; a parent's animated colour reaches its children at their next restyle, not each
frame; line-height, letter-spacing and white-space wait for E4's text, and visibility, though
marked inherited, hides the subtree as before; a reflected property code set before a sheet first
styled the node loses to the sheet (code is seen only by a value other than the sheet's last);
attribute selectors read reflected properties, not arbitrary markup attributes; a class, id or
tag change still restyles the subtree; when the pointer enters the window the root's hover flips,
so a rule reading `:hover` through an ancestor restyles the whole tree once.

### E4 Text and editing

Facts: the lab's `Text` is one font and one colour per string, measured unshaped, with word wrap
and no ellipsis, line height or `white-space` (`Text/Text.cppm`); `TextField` is one line with
selection, clipboard, double-click word select, placeholder and a length limit, and no undo, word
movement, horizontal scroll, multiline, password, read-only, submit or IME composition.
foundation.ui has, from F1 to F4: `TextRuns` (styled spans, inline icons, a small markup) and
`TextLayout` (shaped lines, mixed sizes on one baseline); and an editing engine:
`TextEditingBehavior`, `UndoStack` with coalescing, `InputFilter`. They depend on core, fonts and
vg, with three ties to foundation.ui: a colour parser, the key enums and the clipboard interface.

Design, take and write:

- **Take, by copy.** `TextRuns`, `TextLayout`, `UndoStack` and `InputFilter` are copied into the
  lab (its namespace, its colour parsing, no foundation.ui key enums or clipboard), with their
  tests, and from then on are the lab's own; foundation.ui is not touched (user ruling: copy, not
  a shared module). When the lab and foundation.ui come together (see "The end state"), one copy
  goes.
- **Write the editing model.** A text document with a selection, edited by named commands (insert,
  delete backward and forward by grapheme, by word and to the line's ends, move by grapheme, word,
  line and page, select the same, undo, redo, cut, copy, paste), on byte offsets kept on grapheme
  boundaries, word boundaries from the shaper's segmentation. Keys map to commands through the
  keymap (G7), so editing keys are rebindable and shown in menus like any other command (eepp's
  text input over a document and a command map). foundation.ui's behaviour is the reference for
  what each command does; the model is new because the lab's offsets, keymap and multiline needs
  differ.
- **Text everywhere goes through the copied layout**: a label measures shaped text (no more
  unshaped widths), wraps by `white-space` (`normal`, `nowrap`, `pre`, `pre-wrap`), sets
  `line-height` and `letter-spacing`, ends an overflowing line by `text-overflow` (`clip`,
  `ellipsis`, and an ellipsis at the start or middle for paths), and clamps lines
  (`line-clamp`). Rich text is a label whose text is runs (`rich="true"` or a `RichText`
  element), with links reported as events.
- **The fields.** `TextField` on the model: horizontal scroll with the caret kept in view,
  `read-only`, an input filter, `changing` on each edit and `changed` on commit (Return or focus
  leaving), `submit`, a context menu (cut, copy, paste, select all) once E5 has popups, IME
  composition shown in place (the shell already reports text input; it adds the preedit). A
  `password` mode draws a mask over the same model. `TextArea` is the multiline field (wrapping,
  line movement, page keys, scrolling). `NumberField` (E6) builds on it.

Tests: the copied tests come with the copied code; the editing model per command, on multi-byte and
combining text; undo coalescing; filters; ellipsis and clamp; label sizes from shaped text;
password masking; preedit; `changing` against `changed`.

As built on `vg` (2026-10-17): TextRuns, TextLayout, UndoStack and InputFilter are copied into
the lab with their tests, under the lab's names, colours parsed as its sheets parse them, the
undo stack in byte offsets, and the layout drawing plain runs in the label's own font, asking a
font service only for a run that sets its own (83dae18d1). Neither fonts nor core segments text,
so graphemes (combining marks, joined emoji, flags, CR LF) and words are the lab's own, over
tables of the common ranges; the editing model runs named commands over them (move, select and
delete by grapheme, word, line, page and document; undo, redo, cut, copy, paste), with a
filter, a grapheme limit, read-only and copy-forbidding rules and undo that folds a run of
typing or deleting; every GUI binds the editing keys in the "textinput" context, the vertical
keys and Return in "textinput && multiline" (fd817554a). The Text every widget draws with lays
out through the copied layout, which gains a line height with half-leading, letter spacing, a
clamp, an ellipsis at a line's end, start or middle, and byte offsets for every glyph and line;
white-space, line-height and letter-spacing inherit through the text style, text-overflow and
line-clamp are a label's own; a label is rich with `rich="true"` or as a RichText element, its
links `link` events; a GUI context carries the font service and resources rich text needs
(5d2458385). TextField edits through the model and the keymap, scrolls across, filters, masks a
password and reports `input`, `change` (a commit: Return or leaving with a new value) and
`submit`; a font with no shaper places characters by measuring (f5aaa9f16). The shell reports
an IME's composition when the application asks (WindowSettings::drawsTextComposition) and takes
where text is typed, for the candidates (63e064258). An ellipsis or a password bullet the
font's atlas lacks falls back to full stops or a middle dot, and overflow clips a label's own
text (d59c99328). TextArea and TextField share TextEditBase; the text area wraps, keeps its
column across shown lines, pages and scrolls; a composition shows underlined in place and the
bridge tells the window where the field types (20976429c). The Text page shows it all
(9fb8acfd7).
Left: the context menu waits for E5's popups; the web shell reports no composition and ignores
the input area; segmentation covers the common ranges, not all of UAX #29 (no Hangul jamo or
Indic conjunct rules), and words are not the shaper's; carets assume left-to-right text (E9); a
label's own white-space is nowrap, so a label stays one line until a sheet wraps it (CSS's
initial is normal); the ellipsis passes over text a font could not shape, and a clamp ends in an
ellipsis at its end whatever text-overflow says; a password takes no composition; a link shows
no pointer cursor and is styled only by its markup; menus, combo boxes and tooltips take the
font and colour but not white-space, line-height or letter-spacing; no real IME has been typed
through yet.

### E5 Overlays and the keyboard in composite widgets

Facts: the dispatcher holds one popup and one modal root; ComboBox, Menu and Tooltip place
themselves with no edge awareness; Menu has no keyboard at all; ComboBox cannot be opened from the
keyboard or a pad; MessageBox has no Escape or default button. Popups are moved to the root, so
they lose their owner's styles (the widgets copy fonts across by hand), a dropdown opened inside a
modal cannot be clicked, a root child added later draws over an open popup, and nested modals
break confinement.

Evidence (surveyed 2026-10-17, read-only; the reports are kept locally). Where a popup lives: a
window of its own (Godot, AUI, traktor) makes behaviour depend on the platform; a node moved to
the root (eepp, Zero, foundation.ui, the lab) loses inheritance and event ancestry and leaves
owner pointers to dangle (foundation.ui closes nothing when an owner goes); a node left with its
owner and drawn in a later pass (GPUI's `deferred`, LunarSong's overlay layers, the web's top
layer) keeps both. Nesting is re-invented per widget elsewhere (`ChainContains`, eepp's related
widgets, LunarSong's popup groups); the web derives it from the tree. Dismissal inferred from
focus (Godot, eepp, RmlUi, Zero) brings event-ordering hazards and timing guards; a manager that
owns the policy (LunarSong, foundation.ui) does not. Only Godot reports why a popup closed. Focus
is saved per popup only by foundation.ui and LunarSong; GPUI restores it only while focus is still
inside, and falls back to the nearest surviving ancestor. No retained system re-places a popup
when its anchor moves (RmlUi closes on scroll instead); the web's anchor positioning and gpui-ce's
anchor, gravity and slide, flip and resize flags are the model. Tooltips: text found up the
ancestors (Godot, LunarSong), a warm-up and reset window (LunarSong), hidden on any press.

Design (approved 2026-10-17), the web's top layer on the retained tree:

- **A popup stays in its owner's subtree.** Opening promotes the node into its root's top layer:
  drawn after the whole tree in opening order, unclipped by its ancestors, positioned in the
  root's space, hit-tested first. It keeps inheriting its owner's text style, sheets match it
  where it is (`combobox > listbox`), its events bubble to its owner's listeners (not to the
  owner's own handlers: a click in a menu is not a click on the button that opened it), and
  removing the owner removes it. The hand-copied fonts go where a sheet styles the tree.
- **Nesting from the tree.** An entry's popup ancestors are the open popups holding it. Opening an
  auto popup closes the auto popups that are not its ancestors; closing one closes its
  descendants first.
- **Kinds, and a policy the layer owns.** `auto` closes on a press outside it and on Escape;
  `manual` closes only from code; `hint` (a tooltip) never closes the others, never takes focus
  and passes the pointer through. A press closes the auto popups above the one it lands in; a press
  that closes some and lands in no open popup is consumed, with the rest of its gesture, so closing
  a menu never acts on what lies under it, and a press on the opener closes without reopening (user
  ruling 2026-10-17); a press in a popup that stays (a parent menu closing its submenu) goes on to
  it, as LunarSong's and native menus do. Escape reaches the focused widget first; only when
  nothing takes it does the topmost auto entry close. Every close reports its reason (escape,
  outside press, code, owner removed) in a `toggle` event, open or closed.
- **Modal entries.** Everything below the topmost modal entry is inert: no pointer, no keys, no
  bindings. Popups opened from inside it stack above it and work. Its backdrop is a styleable
  `::backdrop` part drawn just under it.
- **Several roots.** Each root (an OS window, a render target) has its own top layer: a popup is
  placed, shrunk and clamped within its root and never leaves it (a host may later give a popup a
  borderless root of its own; hosting is not a concern yet, ruling 5). Input in one root closes the
  auto popups and hints of the others, as a press outside them would; a modal makes only its own
  root inert.
- **Focus.** A focus-taking entry saves the focus it took, as a weak reference, and on closing
  gives it back only while focus is still inside the popup; a saved node that has gone or been
  disabled hands focus to its nearest surviving ancestor. The topmost focus-taking entry is a
  containing navigation scope (G7), so Tab, the arrows and the pad stay inside it. Focus moves at
  once.
- **Placement, declarative and kept.** An entry carries an anchor (a node, a rectangle or a
  point), a side, an alignment, a gap and the fallbacks: flip to the other side, shrink into the
  larger side (scrolling within its maximum size), clamp into the root. It is placed again every
  layout pass, so it follows its anchor through scrolling and resizing.
- **Menus.** A submenu is a child popup of its item. Godot's safe triangle keeps a submenu open
  while the pointer moves toward it. Context menus come from a `contextmenu` event that bubbles
  from a right press, a long press or the Menu key, the first handler to open a menu taking it.
- **Tooltips.** Hint entries: the text found up the parent chain, any node as content, placed by
  the helper, shown on the context clock after a warm-up that a recent tooltip shortens, hidden on
  any press.
- **Keyboard and pad**: Menu (arrows, Return, Escape, Left and Right through submenus, a disabled
  item skipped, shortcut text from the keymap's binding for the item's command); MenuBar (Left and
  Right between menus, a key to enter the bar); ComboBox (open on activate or Down, arrows while
  closed change the selection, type-ahead, the current item highlighted when it opens);
  MessageBox (Escape is the cancel button, Return the default).
- **The keymap's leftovers** (from the GPUI assessment): a descendant operator (`>`) in context
  predicates, the bindings for a command (for shortcut text), and a command's label and icon
  reaching the controls bound to it.

Not taken: a window per popup (platform-dependent behaviour; Godot's embedded and native popups
differ on whether the dismissing press passes through); dismissal from focus loss; layers fixed by
kind (LunarSong's enum) rather than opening order; the dismissing press passing through.

Tests: drawing and hit order of the top layer over later siblings and clipping ancestors;
inheritance into an open popup; nesting from the tree and closing descendants; the consumed
dismissing press and the opener's toggle; Escape reaching the focused widget first; close
reasons; modal inertness including bindings, with a popup opened inside it working; focus restore
including a node that went away or was disabled; placement flips, shrinks and clamps at every edge
and at display scales, and follows a scrolled anchor; tooltips found from a child; keyboard and
pad walks through a menu bar, a submenu, a combo box and a message box.

As built on `vg` (2026-10-17): the design above was rewritten after a survey of how the others
design popups (4074b78f2). A popup opens as a child of its owner and is promoted into its root's
top layer: drawn after the tree, hit-tested first, laid out where the layer places it (on the
device grid, every layout pass, so it follows its anchor), styled and routed where it is, its
events reaching the owner's listeners and not the owner's own handlers. The dispatcher's stack
replaces the single popup and the modal root; ComboBox, Menu, MenuBar, the modal Window and the
tooltip moved onto it; the menu's chain bookkeeping and the window's scrim went (196466558). A
press is consumed only when it closes popups and lands in none (a press in a parent menu
closing its submenu goes on to it). Menus, the menu bar (F10 to enter, `menu.enter-bar`), the
combo box and the message box take the keyboard and the pad; a list tells an item chosen from
one selected; a pad's East, unbound, is Escape (62f970c48). Tooltips are the dispatcher's, on the
GUI's clock, from the nearest text up the parent chain, any node as content; a right release,
Shift+F10 (`menu.context`) or a long press is a `contextmenu` event; the text fields have their
context menu (bbb573f6e). The keymap takes `>` and gives shortcut text; controls bound to a
command follow its label, icon and keys (d5f28bee4). Godot's safe area keeps a submenu open while
the pointer heads for it (774cf699b), and a submenu's item stays lit while it is open
(2fe5ae7e9). The Menus page shows the keys and the popups (e6f51183c).
Left: a popup never leaves its root (no OS popup windows) and modality is per root; a closed
combo box steps its selection with Up and Down and opens with Alt+Down or activation (the
Windows convention, where the draft said Down opens); type-ahead jumps by first letter only, with
no typed prefix; a menu shrunk to the room it has clips its rows rather than scrolling them;
tooltips show for the pointer only (none for keyboard focus) and are not interactive; F10 enters
the first menu bar in the tree, with no Alt mnemonics; scripts hear `contextmenu` and `toggle`
but cannot build a menu yet.

### E6 The controls, to foundation.ui's depth

The foundation.ui controls assessment lists, control by control, what foundation.ui's do that the
lab's do not; this phase closes that list, with the systems above under it. In outline:

- **A range base** (`min`, `max`, `step`, `page`, `value`) under Slider (vertical, ticks, keyboard
  steps, wheel), ScrollBar and ProgressBar (indeterminate). (Godot's `Range`.)
- **Buttons hold content**: a Button's text is shorthand for a label child, so any content (an
  icon and a label) is markup, not a class; `toggle` makes a button a toggle with `:checked`;
  `repeat` makes it fire while held. foundation.ui's IconButton, ContentButton, ToggleButton and
  RepeatButton become these. ToggleSwitch is its own control. CheckBox gains the indeterminate
  state; radio groups come from a `group` name in markup.
- **New controls**: NumberField (a TextField with a filter, steps, arrows and wheel), PasswordField
  (E4's mask), EditableLabel, Expander, Separator and Spacer as elements, and GridView (a
  virtualised grid of items).
- **Item views** (List, Tree, Table, Grid): rows from a template (native or markup) instead of a
  fixed row widget, variable row heights, activation and context events, type-ahead, an editing
  hook, horizontal scroll; models report inserted, removed, moved and changed ranges instead of a
  whole reset (Zero's model with stable ids); drag reordering after E7.
- **ScrollView feel**: momentum, Shift and wheel for the other axis, drag to pan for touch and pad
  sticks (a finger moving past a threshold pans and cancels the press under it, E1b's
  `CancelPointer`, so a row is not clicked at the end of a scroll), scrollbar policies (auto,
  always, never, overlay), stick-to-end.
- **TabWidget**: closable tabs, reordering, overflow scrolling.
- **Image**: `src` from markup, fit (`object-fit`: contain, cover, fill, none, scale-down), tint.
- **Every control is a markup element** (9 of the 25 widget types are not today).

Tests: a feature matrix per control, the same rows as the assessment's, each a test.

### E7 Drag and drop

Facts: the lab drags a payload with enter, over, leave and drop; no effects, no preview, no
threshold, no cancel.

Design: a typed payload (a type name and a Variant); a drag starts past a distance threshold from a
press (E1's gestures); each move asks the node under the pointer whether it accepts the payload
and with which effect (none, copy, move, link), and the answer sets the cursor and the preview
(Zero's drop test, Godot's `can_drop_data`); the source gives a preview node that follows the
pointer; Escape cancels; the source is told the outcome. A control may claim a press for its own
drag before a draggable ancestor sees it (a scrollbar thumb inside a draggable row).

Tests: threshold, acceptance per move, effects, cancel, the source's outcome, nested claims.

### E8 Tooling and performance

- **An inspector** built from the lab's own widgets in a second root of the same context: the
  tree, a node's computed style with the rules that set each property and their sheet and line,
  the box model, pick by hover, toggling pseudo-classes, and the bindings and commands on a node.
  It reads the reflected table (G1). (RmlUi's debugger as documents in a second context; eepp's
  inspector server, whose protocol would also map onto the engine's MCP tools.)
- **The debug overlay** grows: focus path, navigation candidates and the chosen target, tab order,
  redraw regions.
- **Redraw only what changed**: damage rectangles from the nodes that changed, merged per frame,
  drawn with a clip; a node can cache its drawing in an offscreen layer (VG has layers since V3).
  Animations scrolled out of view or clipped stop asking for frames. (The spec's G4 open limit.)
- **Test helpers**: find by id, text or tag, click, type, press keys, hold a direction, over the
  injection API (AUI's `By::` and LunarSong's semantic replay; GUISandbox's `--keys` and `--sweep`
  are the start), and image tests of sandbox pages at fixed frames: the user approves each
  reference image, and a changed image fails until approved again.

### E9 Translation, right to left, and accessibility groundwork

Before 0.1 (user, 2026-10-16), all three.

- **Translation**: text and templates resolved through a translator the host sets on the context
  (an interface, like the clipboard), auto-translation inherited down the tree and switched off
  per node (Godot's `AutoTranslateMode`), the language a media feature so a sheet can restyle for
  it. A language change re-resolves every translated text.
- **Right to left**: `direction: rtl`, inherited, mirrors flex rows, grid columns, docking, insets
  given as `inset-inline-start` and `-end`, and text alignment (`start` and `end` follow the
  direction); bidi in wrapping comes from the lab's text layout. Navigation's left and right follow
  what is on screen, so they need nothing.
- **Accessibility, the host's bridge** (user question 2026-10-16: an interface the host provides,
  like the clipboard? Yes, with the data flowing both ways):
  - The lab owns an accessibility tree per root, built from the reflected table: each node's role,
    name, value, states (focused, checked, disabled, expanded, selected), bounds and the actions
    it supports. A widget type declares its role and which reflected properties give its name and
    value; nothing is a list of type names.
  - The host sets an `IAccessibilityBridge` on the context. The lab pushes tree updates to it (the
    whole tree when it becomes active, then only the nodes that changed, and focus moves), and the
    bridge sends back action requests (focus, activate, set a value, increment, decrement, scroll
    into view), which the lab routes as input.
  - The bridge says whether anything is listening; the tree is built and kept only while it is, so
    an unused bridge costs nothing.
  - This is AccessKit's model (tree updates out, action requests in) without depending on it: the
    platform adapters (UI Automation, NSAccessibility, AT-SPI) are host work, with hosting. The
    same interface lets a game narrate focus changes with text to speech, which suits consoles and
    the Deck better than a desktop screen reader.

Tests: translated text and templates, a language change, auto-translate inheritance; each layout
kind mirrored under `rtl`; the accessibility tree for each control, incremental updates after a
change, actions routed as input, and no tree built while the bridge is inactive (a mock bridge).

## Not taken

Left out on purpose, with where it comes from: CSS block, inline, float and table layout (RmlUi);
binding attributes and an expression language in markup (RmlUi; ruling 4); a widget per entity
(doriax, Zero's UiWidget, Lumix's removed ECS GUI); an immediate-mode API (GPUI's elements);
BBCode (Godot; the text runs markup already exists); theme sounds (Godot; a gamekit concern if
ever); per-character text effect plug-ins (Godot; the F4 effects suffice until a screen asks).

## The end state

When E1 to E6 are done, the lab's core does what foundation.ui's core does, the editor-only
pieces aside, with the lab's lead kept. Then the toolkit and gamekit start, and the question the
first spec left ("Moving into `foundation.ui`") becomes concrete: whether the lab replaces
foundation.ui's core, with the editor and samples moving over, or pieces move across one at a time.
That is decided then.

## Open questions

None left from the draft; new ones go here as the phases meet them.
