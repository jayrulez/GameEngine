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
no event data. Disabled widgets are filtered since 57011941d.

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

As built on `vg` (2026-10-16): the route (`Node::RouteEvent`, 9a313a648): capture listeners from
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
from `Dispatch` (6753d542c). Script handlers get a constructible `GuiEvent` value and take the
event by returning true (6731150c1). The sandbox's Input page (d2fe0f6cb).
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
`PointerCancel`, which Slider, ScrollBar, a window's drag handles and TextField handle (4ca0b15e2).
The bridge forwards touch from `Dispatch` (placed with `SetWindowSize`) and from a surface's polled
fingers, and drops SDL's mouse copies (dfa9aa570). Not yet tried on a touch screen.
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
(3d0c69729, 8c7d6e4e9). Baselines: every node answers `BaselineAt(size)` (a label, button, text
field and combo box their first line's, any other the bottom of its content box) and a flex row
lines them up, growing to hold them (e6db1fec3). Arranged edges round to the root's display scale,
each edge on its own, with relative offsets and the scroll shift; a new scale lays the whole tree
out again (1d369d485). The layout overlay draws border boxes, padding, margins and text baselines
over a root's tree (`SceneRoot::SetDebugOverlay` since E8c; F9 or `--layout-overlay` in the sandbox,
9d9c4d5bc). The sandbox's Layout page shows all of it in markup (22d67ab76).
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
inherit, so six widgets needed a per-widget font hook (3e766ab7b); parts take only
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
children restyles the siblings (ec6ed7171). The cascade orders by origin: the user agent's sheet,
the GUI's, a subtree's (held weakly by its root, an inner over an outer), then the node's inline
style, which markup's unclaimed attributes fill; the sandbox loads its theme as the user agent's
and each page's sheet for its page (a4e94f2ac). A setter called from code pins its property as
code's (UnpinStyle gives it back); the applier writes in a Node::StyleWriteScope; what no rule
sets returns to the node's own value, its default (widgets write theirs in a scope) or what code
set before a sheet first styled it (fb87619cd). Every node carries a text style (colour, font
with family and size, alignment) that inherits, read by widgets through OnTextStyleChange; the
SetTheme hooks are gone; the table marks the inherited properties; inherit, initial and unset
work (ad3c1da49). The built-in themes still write their colours directly: the palette of custom
properties the design proposed is not built (in the 0.2 backlog). A part's style is a box and an opacity, each aspect over the widget's own look
(1fceed0e8). A state change restyles the node alone unless a rule reads that state through an
ancestor (its subtree) or a sibling (its later siblings) (d21ff1e93). Found on the way: a flex
row clamped a non-stretched item's height to the row's (9cf7b51d1). The Styling page shows the
selectors, inheritance and parts (92cae52d4).
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
font service only for a run that sets its own (da55baf42). Neither fonts nor core segments text,
so graphemes (combining marks, joined emoji, flags, CR LF) and words are the lab's own, over
tables of the common ranges; the editing model runs named commands over them (move, select and
delete by grapheme, word, line, page and document; undo, redo, cut, copy, paste), with a
filter, a grapheme limit, read-only and copy-forbidding rules and undo that folds a run of
typing or deleting; every GUI binds the editing keys in the "textinput" context, the vertical
keys and Return in "textinput && multiline" (92e81d3e5). The Text every widget draws with lays
out through the copied layout, which gains a line height with half-leading, letter spacing, a
clamp, an ellipsis at a line's end, start or middle, and byte offsets for every glyph and line;
white-space, line-height and letter-spacing inherit through the text style, text-overflow and
line-clamp are a label's own; a label is rich with `rich="true"` or as a RichText element, its
links `link` events; a GUI context carries the font service and resources rich text needs
(a2643ac1f). TextField edits through the model and the keymap, scrolls across, filters, masks a
password and reports `input`, `change` (a commit: Return or leaving with a new value) and
`submit`; a font with no shaper places characters by measuring (4d7d4e4fc). The shell reports
an IME's composition when the application asks (WindowSettings::drawsTextComposition) and takes
where text is typed, for the candidates (bc40975c2). An ellipsis or a password bullet the
font's atlas lacks falls back to full stops or a middle dot, and overflow clips a label's own
text (ffc2146b4). TextArea and TextField share TextEditBase; the text area wraps, keeps its
column across shown lines, pages and scrolls; a composition shows underlined in place and the
bridge tells the window where the field types (71777463e). The Text page shows it all
(7534c4acb).
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
design popups (f89fac712). A popup opens as a child of its owner and is promoted into its root's
top layer: drawn after the tree, hit-tested first, laid out where the layer places it (on the
device grid, every layout pass, so it follows its anchor), styled and routed where it is, its
events reaching the owner's listeners and not the owner's own handlers. The dispatcher's stack
replaces the single popup and the modal root; ComboBox, Menu, MenuBar, the modal Window and the
tooltip moved onto it; the menu's chain bookkeeping and the window's scrim went (0dfd468a2). A
press is consumed only when it closes popups and lands in none (a press in a parent menu
closing its submenu goes on to it). Menus, the menu bar (F10 to enter, `menu.enter-bar`), the
combo box and the message box take the keyboard and the pad; a list tells an item chosen from
one selected; a pad's East, unbound, is Escape (c211601a7). Tooltips are the dispatcher's, on the
GUI's clock, from the nearest text up the parent chain, any node as content; a right release,
Shift+F10 (`menu.context`) or a long press is a `contextmenu` event; the text fields have their
context menu (fb80bb47a). The keymap takes `>` and gives shortcut text; controls bound to a
command follow its label, icon and keys (cdcac506c). Godot's safe area keeps a submenu open while
the pointer heads for it (5928e6884), and a submenu's item stays lit while it is open
(9d2b08698). The Menus page shows the keys and the popups (90cf40dca).
Left: a popup never leaves its root (no OS popup windows) and modality is per root; a closed
combo box steps its selection with Up and Down and opens with Alt+Down or activation (the
Windows convention, where the draft said Down opens); type-ahead jumps by first letter only, with
no typed prefix; a menu shrunk to the room it has clips its rows rather than scrolling them;
tooltips show for the pointer only (none for keyboard focus) and are not interactive; F10 enters
the first menu bar in the tree, with no Alt mnemonics; scripts hear `contextmenu` and `toggle`
but cannot build a menu yet.

### E6 The controls, to foundation.ui's depth

The foundation.ui controls assessment lists, control by control, what foundation.ui's do that the
lab's do not; this phase closes that list, with the systems above under it.

Evidence (surveyed 2026-10-17 by concern, read-only: Godot, GPUI and gpui-ce, eepp, RmlUi, AUI,
Zero, traktor, LunarSong, foundation.ui and the lab; the reports are kept locally). Ranges: Godot
shares one `Range` (min, max, step, page, value, exponential, allow greater and lesser) under
Slider, ScrollBar, ProgressBar and SpinBox, though progress inherits a page it cannot use and
orientation is fixed by subclass; eepp, RmlUi and foundation.ui re-implement each (foundation.ui
has four APIs, two unreflected); Godot, AUI and LunarSong split continuous from committed change.
Buttons: Godot's `BaseButton` holds toggle, action-on-press or release, group and shortcut, its
check box a toggle button styled; AUI, zed and RmlUi make a button a container of any content;
RmlUi and eepp expose `:checked`, RmlUi groups radios by `name`; foundation.ui duplicates click and
toggle in five classes and groups radios by layout. Item views: GPUI's `list` estimates heights
and measures lazily in a sum tree, anchors scrolling to an item and an offset, and splices
inserts; Zero and LunarSong keep stable ids with selection and expansion as id sets, LunarSong's
changes as versioned id sets falling back to a reset; eepp's models notify by row ranges and its
table has the fullest column model; the lab is fixed-height and reset-only, foundation.ui O(N)
per rebuild. Scrolling: the lab, Godot and LunarSong take the wheel only when they move, RmlUi adds
`overscroll-behavior`; gpui-ce's tap-or-pan machine (8 px) with closed-form, frame-rate free fling
physics is the best touch model; Godot and RmlUi cancel the press under a pan; RmlUi and eepp let
smooth targets accumulate; eepp and AUI style the scrollbar policy, Zed fades overlay bars; AUI's
stick-to-end re-latches; none scrolls with a pad's stick. Tabs: Godot splits a TabBar from the
container, keeps pages alive and reorders by group; zed and eepp make each tab an element with
slots; foundation.ui draws its tabs. Images: RmlUi and AUI put fit in styles. Number fields: Godot
and LunarSong scrub, evaluate expressions on commit and snap to the step.

Design (approved 2026-10-17):

- **Ranges.** A `RangeValue` (min, max, step, page, value, exponential, allow-greater,
  allow-lesser) that Slider, ScrollBar, ProgressBar and NumberField each hold, not a base class, so
  progress takes no page; the same reflected properties on each; orientation a property; `input`
  while dragging or typing and `change` on commit, as E4's fields. Slider: ticks, page keys, the
  wheel when focused or hovered. ProgressBar: indeterminate.
- **Buttons.** One button core: activation, `toggle`, `repeat` (delay and interval, on the GUI's
  clock), `group`, action on press or release. A button is a container: `text` and `icon` are
  shorthands for label and image parts, and markup may put anything inside. CheckBox, Radio and
  ToggleSwitch are that core with a styled indicator part (user ruling 2026-10-17); the check is
  tri-state (`:checked`, `:indeterminate`); groups by a `name` in markup or a `ButtonGroup` from
  code, the arrows moving within a radio group. IconButton, ContentButton, ToggleButton and
  RepeatButton are attributes, not classes.
- **Item views.** The model/view split stays. Rows come from a template, a native factory or a
  markup `<template>` with `{{item.x}}` bindings (user ruling: both), instantiated into a pool per
  row type and recycled. Heights are estimated, measured on demand in a sum tree, and scrolling is
  anchored to an item and an offset. Models report inserted, removed, moved and changed ranges,
  else a reset; selection and expansion are id sets. Type-ahead; `activate` and `contextmenu`
  events from rows; an edit hook (begin, commit, cancel). Tables: columns in px, % or auto, drag
  to resize, horizontal scroll, a sort indicator. A grid is a layout mode of the same view.
- **Scrolling.** Any overflow node scrolls, with a handle API (scroll to, scroll into view with an
  alignment, scroll to the end); `overscroll-behavior: contain`; Shift and the wheel for the other
  axis, a vertical wheel moving a horizontal-only scroller; smooth wheel scrolling opt-in by
  `scroll-behavior: smooth` (off by default, user ruling); a tap-or-pan state machine with
  time-based exponential decay, a new touch catching a fling, the pressed node cancelled; a
  pad's right stick scrolls the nearest scroller around the focus (user ruling); scrollbars as
  styled parts (`::scrollbar`, `::scrollbar-thumb`) with the policy (auto, always, never,
  overlay that fades) from a style property; stick-to-end that re-latches; a `scroll` event.
- **Tabs.** A TabBar of real tab elements (label, icon and close parts) and a TabView pairing it
  with pages; pages kept alive by default, optionally built on first show; close a request event
  with a display policy; reorder by dragging across a neighbour's midpoint within a strip (between
  strips with E7); overflow scrolls the strip and adds an all-tabs dropdown; Ctrl+Tab,
  Ctrl+Shift+Tab, Home and End.
- **Smaller controls.** Image: `object-fit` and `object-position`, an intrinsic aspect ratio for
  layout, `src` from markup, tint as a style property (nine-slice stays with backgrounds).
  NumberField: on E4's field core with a RangeValue; drag to scrub past a threshold (Shift fine,
  Ctrl coarse), repeating arrows, the wheel while focused, expressions evaluated on commit,
  snapped to the step with decimals from it, prefix and suffix, one f64 field with an `integer`
  flag. EditableLabel: double-click or F2 to edit, Enter and blur commit, Escape cancels, a
  validation hook that can keep it editing. Expander: a header with a disclosure chevron part
  (TreeView reuses it), `open` as HTML's `<details>`, exclusive groups by `name`, lazy content, an
  optional height animation through a clip. Separator: orientation from its parent's flex
  direction, colour and thickness from CSS. Spacer: fixed or flexible.
- **Every control is a markup element.**

Order: ranges, buttons, scrolling, the smaller controls, tabs, item views, then markup coverage;
each a layer with its tests and a sandbox section.

Tests: a feature matrix per control, the same rows as the assessment's, each a test.

As built on `vg` (2026-10-17): ranges are a RangeValue held by a RangeWidget base that the slider,
the scroll bar and the progress bar derive from, the page protected (a progress bar has none);
the slider is horizontal or vertical with ticks and the wheel, the progress bar indeterminate
(223991675). One button core, ButtonBase: toggle, action on press, repeat on the GUI's clock,
groups by a ButtonGroup or a `name` scoped to the GUI, the arrows round a group; its text, icon
and indicator are child parts, a check box's indicator sized by a sheet as `checkbox > indicator`;
RadioGroup went (18d20a5c4); a checked box draws a tick, the mixed state a bar (5f854c46f); the
text part follows the button's alignment (b8b4c103b). Scrolling is Node's: a handle API, the
`scroll` event, Shift and the wheel, `overscroll-behavior: contain`, smooth wheel scrolling by
`scroll-behavior: smooth` (1d1fc531b); `::scrollbar` and `::scrollbar-thumb` are parts with
states of their own (a pseudo-class after a part is the part's state), `scrollbar-width` and an
overlay that fades (d8deeaaf6); a tap-or-pan machine with a closed-form fling and a catch
(75ccc1c3e); the pad's right stick (cee3d206a); stick-to-end that re-latches (0c0b24844). The
smaller controls: Image by object-fit and object-position, `src` through the resource provider,
`image-color` (9f2a65c53); Separator and Spacer (42c429ced); NumberField as expressions,
scrubbing (moves carry modifiers, cc5b6d4ec) and repeating arrows (3432ea4a0); EditableLabel
(798b536ee); Expander, its chevron the tree's (6f079c3de). Tabs: a TabBar of Tab elements with a
close part, close a request (`tabclose`) under a display policy, reorder by a neighbour's
midpoint, an overflowing strip that scrolls and lists every tab, Ctrl+Tab; a TabView of TabPages
kept alive or built on first show; TabWidget went (0b6d5aa69). Models report row ranges and give
rows ids; editing and sorting are capabilities (726011918). ItemView replaces AbstractItemView:
a Node scroll container (Node gained SetScrollExtent and OnScrollChange) realising the rows in
view from pools per row type, heights fixed or measured in a Fenwick sum tree, scrolling anchored
to an item, selection a set of ids, `:selected`, `activate`, type-ahead, editing with hooks
(db250d553); rows from a markup `<template>` with `{{item}}`, `{{item.Column}}` and `{{index}}`,
picked by `row-type-column` (c74c427ca, 327096941); table columns in px, % or auto, resizable,
scrolling across, sorted with an indicator (f223dd117); a grid layout mode (b08eb3da4). Menus are
markup (`<Menu>`, `<MenuItem>`, `<MenuSeparator>`, nested menus, `<MenuBar>`), with
`context-menu="id"` and a bubbling `activate` whose event carries the target's id (9a850dae2);
TreeView, TableView, ScrollBar and MessageBox are elements, the views taking their data from
markup and a message box shown from its page and coming back answered (e50f40169). The Controls
and Data views pages and the Menus page show it all.
Left: the range is a base class, not a member each control holds as the design said, so a
progress bar carries a page it hides; every radio in a group is a tab stop, and so is every tab
(press focus and tab focus are one setting, so a single stop on the checked one waits for them
to part); markup attributes are trimmed as inline CSS is (a number field's `prefix` and `suffix`
are taken as written by the widget itself); type-ahead reads letter and digit keys, not text
input, which the shell starts for fields alone; the tree resets on every model range (selection
and expansion survive by id) and only flat lists follow ranges; the scroll anchor holds in a
list, not in a grid; a template's attributes are written once a row is bound (no live binding
inside a row) and a row whose style is pending is measured again when styled; list scrollbars
overlay the rows' right edge; tabs reorder within one strip (between strips is E7's), and
closing a tab from the keyboard has no key; a markup table's cells are text parsed as numbers
where they read as numbers; a tooltip is an attribute, not an element.

### E7 Drag and drop

Facts: the lab drags a payload (a type and a string) that a widget starts by hand from its own
moves; the dispatcher sends enter, over, leave and drop to the nearest ancestor whose predicate
accepts the type, and a drop on release. There is no threshold, effect, preview, cancel key,
outcome for the source, insertion feedback, auto-scroll, OS drop or keyboard path; the tab strip
reorders with a live swap of its own. The shell reports an OS file drop only once it lands (a
window, a position, a path), with nothing while a file is dragged over the window.

Survey (2026-10-17): thresholds are Godot 10 px (per viewport), Zero 6, LunarSong 6, foundation.ui
4, GPUI 2, eepp, RmlUi and AUI none. Godot offers the drag to the pressed control and then up its
parents, a control that stops the mouse ending the walk; GPUI's bubble order lets the deepest
`on_drag` win; RmlUi walks a CSS `drag` property (`none` passes up, `block` stops an ancestor,
`drag`, `drag-drop` and `clone` claim); foundation.ui stops at a view that owns its press drag (a
scroll bar, a slider). Payloads: Godot an untyped Variant, GPUI a Rust value matched by TypeId,
LunarSong a typed `std::any` with a label and an icon, foundation.ui a DragData object with a
format string, AUI MIME data, eepp and RmlUi the dragged element itself. Acceptance per move:
Godot's `can_drop_data` walks up the parents and sets the can-drop or forbidden cursor; Zero sends
each move a test drop (`MetaDropTest`, `Testing`) whose target writes the text of what a drop would
do, or why it cannot, into a tooltip (red when refused); LunarSong's `CanDrop` gives allowed, a
reason, a CSS class and whether to hide the ghost; foundation.ui alone returns an effect (none,
move, copy, link), and Zed fakes copy with a modifier. Previews: Godot a control the source makes,
its corner at the pointer; GPUI a view keeping the grab offset; RmlUi a clone in a cursor document
styled with `:drag`; foundation.ui an adorner in the popup layer; LunarSong an icon, label and
badge. Ending: Godot cancels on Escape, a right press or `gui_cancel_drag`, and the source asks
`is_drag_successful` at DRAG_END; foundation.ui tells the source the effect and whether it was
cancelled; GPUI, eepp, RmlUi, AUI and Zero tell the source nothing. Insertion: Godot's Tree drops
above, on, below or as first child by quarters of a row, drawing lines on the hierarchy; Zero and
LunarSong use edge bands with Before, On and After, falling back when a mode is refused, and reject
a parent under its own child with a reason; Godot's TabBar and eepp move tabs between bars of one
group, eepp tears a tab off after 64 dp across and splits a panel on an edge drop, Zed draws a
border on the side a tab would land. Opening on hover: Godot's Tree, Zed's panel and LunarSong
expand a collapsed node after a pause; Godot's TabBar switches tabs under a drag. Auto-scroll:
Godot's ScrollContainer (opt-in) and Tree, Zero's tree and LunarSong's lists, each a band at the
edge with a speed ramp; GPUI, eepp, RmlUi, AUI and foundation.ui none. OS drops: GPUI turns an OS
drag into the same drag (a typed `ExternalPaths` value), LunarSong replays a dropped file as a drag
and a drop at the cursor, the others take files apart from the drag. Keyboard: Godot alone, an
action that picks up from the focus and drops at the focus, positions given as infinity.

Design (approved 2026-10-17):

- **Starting.** A press that moves past `drag-threshold` (6 px, a GUI setting; a finger after a
  long press, since a finger's move pans a scroller first) offers the drag to the pressed node and
  then up its ancestors (Godot's walk); the first that gives a payload starts it. A node that owns
  its press (a scroll bar's thumb, a slider, a text field selecting, a number field scrubbing, a
  tab strip reordering) ends the walk, so a draggable row never takes its scroll bar's drag; markup
  says the same with `drag="none | auto | block"` (RmlUi's property: `block` stops the walk at a
  node that drags nothing). The pressed widget's press is cancelled (E1's PointerCancel), so no
  click follows.
- **The payload.** A type (a short name such as `text`, `files`, `lab/tab`, `asset/mesh`), a
  Variant value (a string, a number, a list, a weak node handle), a label for the preview and for
  accessibility, and the effects the source allows (copy, move, link, a set). Markup:
  `drag-type`, `drag-value` (which may be a `{{name}}`), `drag-label`; scripts hear `dragstart`
  and may fill the payload there.
- **Acceptance on every move.** Each move, each change of the modifiers, and each scroll under a
  still pointer asks the node under the pointer and then its ancestors what a drop would do (Godot's
  walk, Zero's test drop): an effect (none, copy, move or link, from those the source allows, the
  modifiers choosing as the platform does: Ctrl copy, Shift move, both link), a reason (what it
  would do, or why not), and where (a position the target draws). The first that takes it is the
  target; one that refuses with a reason ends the walk and shows it. Markup's `drop-types="card
  files"` accepts those types with the source's preferred effect; a script's `dragover` handler
  may set the effect and the reason. The cursor follows the effect (`copy`, `alias`, `move`,
  `no-drop`: new cursor names), the target shows `:drop-target` while it would take the drop, and
  the reason shows as a hint by the pointer (Zero's tooltip, red when refused).
- **The preview.** The source may give a node; otherwise a label of the payload's label in the
  `dragpreview` style. It lies in the top layer at the pointer, keeping the grab offset (GPUI),
  takes no hits, and shows the effect as a badge (`dragpreview::effect`); the source shows
  `:drag-source` while it is dragged.
- **Ending.** A release over a target drops (`drop`, the effect and the position given); anywhere
  else, or on Escape, a right press, the pad's East, the source leaving the tree, or the GUI
  losing focus, it is cancelled. The source is told the outcome (`dragend`: the effect, none when
  cancelled), so a move removes what was moved (foundation.ui's `OnDragCompleted`).
- **Auto-scroll and opening on hover.** A scroll container under the pointer, near its edge (a
  band, `drag-scroll-band`, speed ramping with depth, on the GUI's clock), scrolls, the innermost
  that can first, so a drag reaches rows out of view; acceptance is asked again as the content
  moves. Resting a drag on a closed tree node, an expander's header or a tab for a moment opens
  or selects it.
- **Item views as sources and targets.** A view drags its selected rows (their ids, the payload
  type its own or the model's). As a target it finds where in a row a drop lands: before, on or
  after, by bands (a quarter at each edge, halves when the model takes no drop onto a row), falling
  back when a place is refused (Zero, LunarSong), and draws an insertion line (`::drop-indicator`)
  or lights the row. The model has the capability: what a drop at an index and a place would do,
  and doing it; a list moves rows within itself, a tree reparents and refuses a node under its own
  child with a reason.
- **Tabs between strips.** A tab dragged out of its strip past a tear distance (eepp's resistance)
  becomes a drag of type `lab/tab`; strips of one `group` take it (Godot's rearrange group), the
  insertion mark between the tabs it would land between; within a strip the live swap stays.
- **OS files.** A file the OS drops comes through the same path, as LunarSong replays one: a
  payload of type `files` (the paths) asked at the drop's position and dropped there, effect copy;
  one no target takes goes to the GUI's own handler. (The shell gives no hover, so nothing shows
  while a file is dragged over the window.)
- **The keyboard and the pad.** `drag.pick` on the focused widget starts a drag of what it would
  drag; focus then moves as usual (the arrows, Tab, the pad), the focused node asked as the pointer
  would be (a position of "the focused item"); `drag.drop` (Return, the pad's South) drops,
  Escape (East) cancels (Godot's accessibility drag). One drag at a time, within one root.
- **Positions.** A drag keeps the pointer in two spaces: its root's (what hit-tests and places
  the preview) and, where the host gives one, the screen's; each node is asked in its own local
  space (scrolled and transformed). Deltas that move something are taken in the space that does
  not move with it: foundation's docking found that a floated window following the drag moves the
  frame its window-local positions are measured in, so it measured in screen space, routed the
  drag past the window under the pointer when that window was the one being dragged, and took the
  shell's button state as the authority when a release was lost at a window's edge (Wayland,
  which also gives no global position). E7 stays within one root; the screen position is kept so
  that dragging between windows, a follow-up that needs the shell to say which window the pointer
  is over, adds a router rather than a rewrite.

Rulings (user, 2026-10-17): the design as written; OS files through the same path (each window's
root takes the drops that land in it, no app code); the keyboard and the pad in E7; dragging
between windows a follow-up, minding local and global positions (above).

Tests: the threshold, by mouse and by finger; the walk up and a node owning its press; acceptance
per move, per modifier and per scroll, with reasons; effects and cursors; the preview's offset;
cancel by each way; the source's outcome; auto-scroll and opening on hover; item view insertion
places and fallbacks, a list's move and a tree's reparent with a refusal; tabs between strips;
an OS drop through the path; a keyboard drag; markup and script drags.

Order: the core (start, payload, acceptance, preview, ending), then auto-scroll and opening on
hover, item views, tabs between strips, OS files, the keyboard and the pad, then markup and
scripts; each a layer with its tests and a sandbox section.

As built on `vg` (2026-10-17): the core (cf3c82295): a 6 px threshold (SetDragThreshold) and
the walk up from the pressed node (DragRequest), ended by a node owning its press (OwnsPressDrag:
sliders, scroll bars and a scroller's bars, text fields, a window's handles, a table header's
resize edge, a tab in a reorderable strip); the press cancelled; a finger's drag armed by a long
press over a source instead of a context menu; a payload of a type, a Variant value, a label and
the effects allowed; DropQuery on every move, modifier change and RequeryDrag, a refusal with a
reason ending the walk; `:drop-target` and `:drag-source`; the cursor by the effect (new `copy`,
`alias`, `no-drop`, the shell showing the nearest it has); the preview (the source's, keeping its
grab point, else a DragPreview with an effect badge) and the hint in a pointer-passing layer of
the top layer; cancel by Escape, a right press or the source leaving; the source told through
OnDragEnd. Auto-scroll (24 px band, up to 900 px/s) and opening on a 0.6 s rest: a tree node, an
expander, a tab (4fba803b6). Item views drag their selection and take drops before, on or after
a row through model capabilities (ModelDrop, CanDrop, Drop, RemoveIds), a list moving its rows
with their ids, a tree reparenting and refusing a node inside itself (4b73d8795). Tabs torn out
of a strip past 32 px drop on strips of their `group`, between tab views with their pages
(9196e1bec). OS files through the same path, InjectFileDrop and the bridge's DispatchFileDrops,
a GUI handler for the rest (561699e33). `drag.pick` (Ctrl+Shift+D, the pad's North) and a drop at
the focus (6338a6a01). Markup's `drag-type`, `drag-value`, `drag-label`, `drag`, `drop-types`,
`drop-reason`, and the script events with a `dragquery` (2e7b761ad).
Left: one drag at a time within one root (dragging between windows is the follow-up the
positions note above describes); a script answers a query by taking it (`dragquery`), its reason
from `drop-reason`, not by setting the effect; the drag scroll band and the hover delay are
dispatcher settings, not style properties; an OS file shows nothing while it is dragged over
(the shell reports only the landing); the pad's d-pad moves the focus during a keyboard drag, so
inside a list it leaves the list where the keys would move its row; a tree takes a drop as a
reset of its rows; a copy of a tree's own nodes is not taken.

### E8 Tooling and performance

Facts (read and measured 2026-10-17):
- The host skips a frame when `SceneRoot::NeedsFrame()` says nothing changed, so an idle GUI draws
  nothing. A drawn frame rebuilds all of its VG geometry: the child loop in `Node::Draw` draws every
  child, with no culling against the clip, so a long scrolled page tessellates what is off screen.
- Timed in GUISandbox with `--always-redraw` in RelWithDebInfo (`build/clang-reldbg`; Debug runs
  far slower on Vulkan, user 2026-10-17, so its numbers say nothing), with a temporary patch: the
  Controls page, the heaviest, costs 0.15 ms styling, 0.03 ms layout, 0.28 ms building VG
  geometry (9.9k vertices, 166 commands) and 0.13 ms in the renderer's Prepare. The sandbox's
  pages are small: none of them is a worst case.
- The same run found the idle GUI was not idle: every page drew every frame, and 13 nodes
  restyled on every frame. An indeterminate progress bar on a hidden page kept asking for
  frames (fixed in d692b1e9e: a node not shown marks itself alone, and NeedsFrame counts actions
  on shown nodes only), and the style pass wrote markup's `items` to a list box at every restyle,
  which rebuilt its rows and so restyled it again (fixed in c5608a58e: a reflected property is
  not written the value it holds). Now a still page draws 2 frames of 840 and restyles nothing.
- Tooling: the F9 layout overlay (bounds, padding, margin, baselines), and GUISandbox's `--keys`
  (keys, pad buttons, and pointer steps at coordinates), `--sweep` (hover, click and right-click
  every widget, a smoke test), `--frame-stats` and the framework's `--screenshot`. A matched rule
  knows its sheet's origin and order but not its file or line. No inspector, no counters,
  nothing reaches `foundation.profiler`.

Survey (2026-10-17):
- **Inspectors** are built from the framework's own widgets and run in process: RmlUi as documents
  in a host context over the inspected one, eepp as a floating window in the same scene, gpui as a
  side pane in the same window, aui as a second native window. Godot alone inspects a separate
  game process from the editor, and eepp and LunarSong add loopback servers (newline-delimited
  JSON, opaque handles, selector queries, input, screenshots) that agents drive. All show a tree
  and a selected node's properties, read through reflection or the style registry, never a list.
  RmlUi alone shows the rule and `file:line` that set each property, with inherited values from
  the ancestors, and toggles `:hover`, `:active`, `:focus` and `:checked`. aui prints the
  applicable rules by selector and shows `measure()` under several constraints. gpui shows the
  source line that built an element, picks deeper in the hit stack with the wheel, and edits a
  style live. Box models are drawn as translucent content, padding, border and margin fills
  (RmlUi, aui, foundation.ui).
- **Overlays**: outline every node (RmlUi; eepp colours each by its pointer), the hovered and
  focused nodes (eepp), redraw flashes (Godot's canvas items, aui's redraw requests, eepp's dirty
  windows), and foundation.ui's focus path and numbered tab order with lines to the next stop,
  which no other framework has. Timing: aui's per-frame flame chart, LunarSong's per-phase
  breakdown (cascade, layout, hit test, geometry), Godot's monitors in the editor.
- **Redraw**: no framework presents only a damaged region. Each redraws its whole surface when
  anything is dirty and skips the frame otherwise (eepp, gpui, foundation.ui, Godot's low
  processor mode); what they save is CPU work, by keeping each node's geometry or commands and
  replaying the clean ones (Godot's canvas item commands, RmlUi's compiled geometry, gpui's
  `cached()` views replaying the last frame's paint range, LunarSong's persistent slots).
  Persistent offscreen subtrees are rare and explicit (eepp's framebuffer windows, aui's
  render-to-texture scroll contents with partial updates). Culling is per node against the clip;
  RmlUi and LunarSong grow the box by its ink (shadows, blur at 3 sigma, outlines), foundation.ui
  and probably gpui do not and can drop a visible shadow. Animations mostly keep asking for frames
  while clipped or scrolled away; only frameworks that animate from the draw (aui,
  foundation.ui's `RequestNextFrame`) stop, because a culled node never asks.
- **Tests**: a test owns the loop and steps it (aui's `frame()` after every action, Godot's
  `process(0)`, gpui's seeded executor and virtual clock). Finders are semantic: aui's composable
  `By::text`, `By::name`, `By::type` with `.perform(click()).check(text(...))`, visibility-aware;
  eepp's `querySelector`; LunarSong's `labelTextUnderId`; Zero's recorded path of name, type and
  sibling index with an offset inside the widget. None dumps the tree on a failure; aui saves a
  screenshot with the matched views outlined. Image tests: RmlUi exact, zed per channel within 2 on
  99% of pixels, eepp CIEDE2000 within 2.3 with a bad-pixel budget, each writing the actual and a
  diff image; approval by a flag (zed's `UPDATE_BASELINE=1`) or by repointing a capture folder
  (RmlUi); eepp writes a missing reference silently, which approves anything. LunarSong compares
  hashes between its cached and correctness modes instead of against stored images. The engine's
  VG probe tests read pixels back on real devices and skip without one.

Design (approved 2026-10-17, with the rulings below), in layers:

- **E8a Counters and a stress page.** `SceneRoot::GetFrameStats()`: per phase the time and the
  count (nodes restyled, containers laid out, nodes drawn and culled, vertices and commands), and
  why the frame was needed (invalidation, layout, an animation and on which node, a binding, a
  held direction), so the two bugs above show at once instead of through a patch. The phases open
  `PROFILE_SCOPE`s, so the engine's profiler shows them. A Stress page in GUISandbox (user,
  2026-10-17) is the worst case every later layer is measured on, its load chosen by a control on
  the page and by `--stress <n>`: thousands of nodes in a long scrolled column; rows of text in
  several faces and sizes, wrapped and rich; every box effect at once (rounded borders, outer and
  inset shadows, gradients, backdrop and filter blur, blend modes, group opacity); hundreds of
  transitions and keyframe animations running together; a list and a table of 100k rows; deep
  nesting (a few hundred levels); a sheet of many rules with descendant and sibling selectors,
  and a toggle that changes a class at the top so the whole tree restyles. The page prints E8a's
  counters each second (`--frame-stats`), and its numbers, taken in RelWithDebInfo, go in this
  section before E8b starts.
- **E8b Culling and quiet animations.** A node whose ink box misses the current clip skips its
  subtree. The ink box is its bounds grown by its outer shadows (offset, spread and 3 sigma of
  blur), its filter blur and its focus ring, mapped through its transform, the way RmlUi and
  LunarSong do it. An animation on a node that was culled or hidden in the last drawn frame stops
  asking for frames unless it animates where the node is (transform, position, size, margin,
  inset), which could bring it into view; it is caught up when the node shows. A differential test
  (LunarSong's) draws pages with culling on and off and requires the same pixels.
- **E8c The debug overlay grows**, one set of flags (the layout overlay's struct becomes
  `DebugOverlay`): the focus path, the tab order with numbered stops and lines (foundation.ui's),
  the navigation candidates of the last directional move with the one chosen and why (the lab's
  own; nobody else has navigation to show), the node under the pointer, and a redraw flash on the
  nodes invalidated since the last frame (Godot's and aui's), which would have shown the desk
  session's repaint bugs at a glance.
- **E8d What the inspector needs from the core.** The CSS parser records each rule's sheet name
  and line, and a node can say for each property where its value came from: a rule (sheet, line,
  selector), inheritance (from which ancestor), markup's inline style, code (a pinned setter), or
  its own value. A node can force pseudo-classes (`:hover`, `:active`, `:focus`,
  `:focus-visible`, `:checked`, `:disabled`) for the inspector, which the style pass reads as
  set.
- **E8e The inspector.** A library over the core's public API (`experimental.gui.inspector`),
  built from the lab's widgets in a root of its own on the same context, shown by the host as a
  pane beside the inspected root (Ctrl+Shift+I in GUISandbox). The tree, live as children change;
  pick by hover and click, the wheel walking deeper through what is under the pointer (gpui's).
  For the selected node: its type, tag, id, classes and states, with the pseudo-class toggles;
  the box model drawn; each property with its value and its origin from E8d, grouped by rule
  with sheet and line (RmlUi's), inherited ones under their ancestor; its reflected properties
  (G1's table), edited live through the path markup uses; its bindings, commands and event
  listeners; `Measure` under a few constraints (aui's). A frames tab draws E8a's counters over
  the last few seconds. Its queries (the tree, describe a node, a selector query) are a C++ API
  the panes use, so an MCP tool can serve the same answers when the lab is hosted (eepp's
  server and LunarSong's tools show agents want them; hosting is not a concern yet, ruling 5).
- **E8f Test helpers.** `experimental.gui.testing`, for tests: a tester over a root owns a fixed
  clock and steps a frame (update, style, layout) after every action. Finders by CSS selector
  (the lab's own selector engine, eepp's `querySelector`) and by text, composable with `within`;
  hidden nodes are not found unless asked; a finder that expects one node and finds none or
  several fails, listing what it found and dumping the tree around it. Actions: click, right
  click, double click, hover, drag to a node or point, type, press a key chord, hold a direction
  for a time, a pad button, an OS file drop. GUISandbox's `--keys` takes selector targets
  (`click:#ok`, `click:text=Show`) besides coordinates, so a script survives a layout change
  (Zero's and LunarSong's point).
- **E8g Image tests.** Sandbox pages at a fixed size, scale 1, a fixed clock and a chosen frame,
  rendered and read back through the RHI (the screenshot path), so on every backend the machine
  offers and not Vulkan's alone (user, 2026-10-17); skipped without a device. A picture matches
  its reference when every channel is within 8 on all but 0.1% of pixels (eepp's budget, zed's
  ratio); a failure writes the picture and a diff image into the build tree. References live in
  the repository under `Data/Tests/GUI/Images/`, one per picture shared by the backends; a
  backend that cannot match it within the tolerance gets its own, named for it, only when it
  must. A missing reference fails. They are approved only by
  hand: `GUISandbox --approve-images` writes the current pictures as references after the user
  has looked at them, so a changed picture fails until approved again (the user's rule). The
  E8b differential test runs in the same harness without references.

Not taken for now: a remote protocol for the inspector before hosting.

Rulings (user, 2026-10-17): the inspector is a pane in the inspected window; image tests run
through the RHI on every backend, with references in the repository; damage rectangles and a
cache of each node's geometry are decided on the Stress page's numbers (E8a), not before. Both
would need more than the lab: partial presentation needs the host to keep the last frame (swap
chains do not), and geometry replay needs VG to record and replay a range of its batch under a
new transform and clip. No framework surveyed presents part of a window.

Stress numbers (E8a, RelWithDebInfo, clang, 2026-10-17; the window shows about one twentieth of
the page at load 10):

| Load | Nodes drawn | First frame: style | Layout | Draw | Animated frame: draw | Vertices |
|---|---|---|---|---|---|---|
| 1 | 2,484 | (under a frame) | | | 3.0 ms | 100,719 |
| 5 | 11,780 | 1,335 ms (12,511 restyled) | 55 ms | 52 ms | 15.4 ms | 489,029 |
| 10 | 22,898 | 3,021 ms (23,629 restyled) | 102 ms | 103 ms | 30.2 ms | 975,169 |

- Building the rows grows faster than their count (400 rows 2.8 ms, 2,000 rows 27.5 ms, 4,000
  rows 84 ms); the 100k-row models take about 40 ms at every load.
- The page found a fault at once: layout doubled with each level of nesting (a one-entry measure
  cache; a flex item is measured twice), so 500 levels never laid out. Fixed: four entries.
- Drawing costs what the whole tree holds, not what shows: nothing is culled, so a frame with one
  animation draws a million vertices at load 10. Culling (E8b) is the first win.
- Styling costs about 110 to 130 µs a node against the page's 1,000 extra rules, the largest
  cost: one class at the top restyles the tree in 3 s at load 10. Every rule is tried against
  every node; an index of the rules by their rightmost compound (its id, classes and tag), as
  browsers and RmlUi keep, would try only the few that can match.
- Damage rectangles would help only frames where little changes, and the culled draw is not yet
  measured; geometry caching is measured again after E8b.

Rulings on the numbers (user, 2026-10-17): no damage rectangles for now; a rule index joins E8
beside culling, since styling was the largest cost.

As built after the numbers (2026-10-17):

- **Style matching.** A sheet keys its rules by their subject compound (id, else first class,
  else tag, else one list for every element), and the cascade breaks a tie by source order. A
  selector carries hashes of the names its subject's ancestors must have, and the restyle walk
  keeps a counting Bloom filter of the names above it (WebKit's SelectorFilter), seeded with the
  ancestors of where it starts. A child keeps its index and a widget its position among its
  widget siblings until its parent's children change, so `+`, `~` and `:nth-child` cost one pass
  over a column, not one each. The full restyle at load 10 with 1,000 rules: 3.0 s, then 0.62 s.
  What is left of a restyle is mostly applying the cascade (about 14 µs a node). Measured apart
  (2026-10-17), that was the first restyle baking each font size's whole atlas as a label first
  asked for it (Roboto's extended Latin, a size at a time: about 100 ms on the stress page, once).
  A second full restyle at load 5 applies in 45 ms (3.6 µs a node; the layout properties the
  largest part, 1.4 µs). Should the first one matter, the fonts could rasterize glyphs as text
  first needs them instead of a size's whole set, or a host could bake its sizes at load.
- **E8b.** `Node::GetInkBounds` (virtual: outer shadows at three sigma; a label adds its laid out
  text) and `GetSubtreeInk` (kept until an invalidation reaches the node, which every change below
  it does); a subtree whose ink misses VG's scissor, with a 32 unit margin for text effects an
  ancestor sets and overhang, is not drawn. `SceneRoot::DrawFrame` clips the drawing (only the
  drawing: navigation still reaches past) to the window. An animated frame at load 10: 30.2 ms
  and 975k vertices, then 0.74 ms and 7.7k. `Action::CanMoveTarget` (a fade, a colour or opacity
  style animation cannot); such an action on a target not drawn in the last frame asks for no
  frames and its writes mark the target alone. The differential test compares the triangles that
  land inside their scissor (layers by those that show) with culling off and on.
- **VG fix found by it.** `TransformRect` took two corners, so a rotated or flipped rect could get
  a negative size: culling dropped turning boxes, and a rect clip inside a flip clipped all.
- **Removed.** `KeyframeAction` (opacity and a background colour, linear, writing past the
  cascade, unused): CSS keyframes cover it.
- **Left.** (Building rows grew faster than their count, each new child marking its siblings for
  a restyle: now the style pass restyles them once, when a rule reads siblings.) the stress page keeps drawing while its turning boxes are out of view (a transform
  animation could bring a node into view; a rotation about the centre could be bounded).
- **E8c as built.** `DebugOverlay` (its own partition) replaces the layout overlay's flags: the
  layout half as before (F9), and an input half the root draws in its own space (F8,
  `--input-overlay`): the focus path named; every tab stop numbered and joined (the dispatcher's
  `TabStops`, which Tab uses); the last directional move's `NavTrace` (each candidate's bounds,
  name, score and verdict: chosen, beaten, behind, clipped; overrides and wraps); the node under
  the pointer; a red flash fading over half a second on each node that invalidated itself (a frame
  reason of its own, "overlay"). Its first picture found Tab and the arrows reaching widgets under
  a node out of the layout; fixed.
- **E8d as built.** A sheet is named when parsed; a rule keeps its selector as written and its
  line (comments are blanked keeping their line breaks). `Node::SetForcedStyleStates` (the style
  state bits; forced disabled is not enabled, forced focus-visible is focus).
  `StyleManager::ExplainStyle` and `ExplainProperty` over `StyleSheet::Trace`: per longhand every
  declaration in precedence, the winner first, and code's pins, inheritance (which ancestor) and
  running animations.
- **E8e as built.** `GUI.Inspector` (`experimental.gui.inspector`): queries (Tree, Describe,
  Query, SetProperty as markup sets it) and the Inspector, a root of its own on the context: the
  tree as an indented list rebuilt when the tree's shape hash changes, the details (identity,
  states with force toggles, box and measure, styles by rule with sheet and line, reflected
  properties edited live, commands, listeners, bindings), a selector field, picking with the wheel
  walking out and in, the box model drawn over the inspected root, and a frames graph.
  GUISandbox hosts it as a pane (Ctrl+Shift+I, `--inspector`, `--inspect <selector>`) with an input
  surface of its own, its boundary dragged to resize it. Its toggles found a layout fault: a
  single-line flex container measured under a limit took the limit as its height; fixed. The MCP
  tools over its queries are a phase of the 0.2 backlog (user, 2026-10-17), once the lab has a
  place in the foundation or the engine.
- **E8f as built.** `GUI.Testing` (`experimental.gui.testing`): Finder (selector, text, text
  containing; Within; IncludingHidden; Parse for "text=" and "text~="), Tester (a frame of update,
  style and layout after each action on a fixed clock; click, right and double click, hover, drag,
  type, chords, held directions, pad buttons, file drops; each aims at a node's middle and fails
  when it is covered; One fails on none or several with the tree), and `CompareImages`.
  GUISandbox's `--keys` pointer steps take finders.
- **E8g as built.** `GUISandbox --image-tests` (`--vulkan`, `--webgpu`): each page but Stress, 40
  frames on a fixed clock at 960x720 and scale 1, drawn offscreen and read back through the
  screenshot capture, compared with `Data/Tests/GUI/Images/<page>.png` (or `<page>.<backend>.png`):
  every channel within 8 on all but 0.1% of the pixels; a miss writes a diff; exit 1 on a miss, 77
  without a device. `--approve-images` (or `--approve-images backend`) writes the references, once
  the user has looked. Deterministic: two Vulkan runs give the same bytes, and WebGPU on the same
  GPU the same pixels. The user approved the references, and ctest runs `GUI.Images.vulkan` and
  `GUI.Images.webgpu` (skipped, 77, without a device). The culling differential runs in GUI.Tests
  on VG's geometry rather than here.

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

What the engine has (surveyed 2026-10-17): no translation anywhere (Foundation, Engine, Editor);
no right-to-left in foundation.ui or the lab; no accessibility. Foundation.Fonts resolves bidi
levels (UAX#9 to I2, SheenBidi behind `IBidiResolver`, installed by Engine.UI and the editor but
not by the lab) and reorders a line's runs (`ReorderLine`, L1 and L2); `ShapeRequest` takes a
direction and a BCP 47 language. The lab's text layout shapes word pieces left to right with no
base direction, `text-align: start` is left, and the caret and its moves are logical and left to
right. The context already carries shared services (`SetFontService`, `SetResourceProvider`); the
clipboard is per root.

Survey (the clones, read 2026-10-17):
- **Translation.** Godot alone has a runtime design: the source string is the key (gettext, with
  a context and plurals), a widget keeps its text and the translated copy apart, `auto_translate_
  mode` (inherit, always, disabled) is resolved up the tree and cached, a language change walks
  the tree and re-resolves, catalogs are PO or CSV, and pseudolocalization (accents, expansion,
  brackets, fake bidi) makes untranslated text show. RmlUi passes every text node through one host
  hook once at load (and translates a binding after interpolation, which breaks keys); eepp's
  `@string/key` with an inline default harvests missing keys; ezEngine chains translators and can
  show every string's key; aui, zed and LunarSong have none.
- **Right to left.** Godot flips every control's x after layout ("left" means start; mirrored
  theme icons); eepp mirrors inside each layout on logical axes and keeps logical and physical
  declarations apart until layout (`margin-inline-start` resolved per edge, so a direction change
  reloads no style); gpui-ce resolves direction in a pass before layout and hands taffy `Rtl`.
  Text: Godot runs paragraph, bidi runs, script, style span, shape, wrap, then reorders each line
  (eepp reorders before wrapping, a known fault); gpui-ce's caret is a logical index with an
  affinity, arrows move visually, delete works on logical clusters. Icons mirror only on opt-in.
- **Accessibility.** Godot (AccessKit through an abstract server: persistent ids, a set of dirty
  nodes flushed once a frame, focus recomputed at each flush, the whole window queued on
  activation, actions bound per node and run on the main thread), gpui (AccessKit, the whole tree
  rebuilt each frame, gated on an atomic latched per frame; gpui-ce adds a forced mode for tests),
  eepp (its own backends: roles from the widget's type with overrides and `aria-*` in markup, a
  name from labelled-by, label, own text, then tooltip, leaf roles hiding parts, an audit of
  unnamed controls, polite and assertive announcements). Godot keeps text to speech a separate
  service.

Design (approved 2026-10-17, with the rulings below), in layers:

- **E9a Translation, over foundation.i18n.** The engine's own layer (`Specs/i18n.md`, built
  2026-10-17) holds the texts: string tables with dotted keys, plural entries and each locale's
  plural rule and direction, a `Localization` its owner holds. The lab takes the host's
  `Localization` on its context (a translator interface, a thin adapter, as the font service is
  set). A reflected string property marked `translatable` (a property attribute, so a library
  widget's joins with no list: a label's and a button's text, a tooltip, a placeholder) keeps its
  source and shows its resolution: markup writes a key as `text="@menu.play"`, and text without
  the `@` stays as written. `auto-translate` (inherit, always, off; the root always), resolved up
  the tree and cached, Godot's. A key takes a count and arguments from sibling attributes named
  for its property (`text="@files.count" text-count="{{n}}" text-args="{{player}}"`, each a value or
  a binding), formatted with `FormatPlural` and `Format` before anything fills (not after, which
  breaks keys); `@@` writes a literal @. The `Localization`'s `OnChanged` re-resolves every
  translated property in the GUI's roots, and a node joining a tree resolves on entry.
  `:lang(xx)` matches a node's language (the `lang` attribute, inherited, else the context's).
  Pseudolocalization is the `Localization`'s, told to leave the lab's rich-text tags and bindings
  untouched. The inspector shows a text's key beside its resolution.
- **E9b Direction and the layouts.** `direction: ltr | rtl` (inherited; at the root the
  language's, from the `Localization`'s `IsRightToLeft`, which its tables say), resolved and cached; a
  change lays out again, no restyle. Each layout mirrors on logical axes (eepp's and taffy's way,
  not Godot's flip after layout): a row runs from inline-start, a grid's columns from inline-
  start, `start` and `end` in justify, align and text-align follow the direction, dock takes
  `start` and `end` beside its physical sides. Logical `margin-inline-*`, `padding-inline-*` and
  `inset-inline-*` stay apart from the physical ones until layout. A scroller's bar goes to its
  inline-end; its offsets stay physical. `mirror-on-rtl` flips a node's drawing (a submenu arrow,
  a tree's expander), opt-in. Widgets whose Left and Right keys move along their own axis (a
  slider, a tab bar, a menu bar) follow the direction; Tab order stays the document's.
- **E9c Bidirectional text.** The lab's text layout gets a paragraph direction (the node's, or the
  first strong character for `auto`), splits bidi runs through Foundation's resolver (GUISandbox
  installs SheenBidi as Engine.UI does), shapes each run in its direction, wraps, then reorders
  each line (Godot's order; not before wrapping). The caret becomes a byte offset with an
  affinity: arrows move visually, backspace and delete logically; a selection is a list of
  rectangles; at a direction boundary the caret draws at its affinity's side.
- **E9d The accessibility tree and the bridge.** AccessKit's model, owned in C++: `AccessNode`
  (a stable id from the element table, a role, a name, description and value, numeric value, min,
  max and step, bounds in root space, states, children, labelled-by, described-by and controls, and
  the actions it supports), `AccessTreeUpdate` (the nodes that changed, the root on the first,
  the focus) and `AccessActionRequest` (focus, activate, set value, increment, decrement, scroll
  into view, expand, collapse; a string or a number). `IAccessibilityBridge` on the context says
  whether anything listens and takes updates; requests it sends are queued and run on the GUI's
  thread through the paths input uses (the dispatcher's focus, Activate, a value's setter, scroll
  into view), and one a node does not advertise is refused. The tree is built only while the
  bridge listens (or forced, for tests and the inspector): the whole root on activation, then the
  nodes marked since (an invalidation, a state, the children changing), flushed once a frame with
  the focus. A widget type declares its role as a type attribute and which reflected properties
  give its name and value as property attributes; markup overrides with `role`, `aria-label`,
  `aria-labelledby`, `aria-describedby` and `aria-hidden`. A name is the labelled-by node's, else
  the label, else its text, else its tooltip; a button's label part is no node of its own (leaf
  roles hide their parts). `GUIContext::Announce(text, polite | assertive)` for narration. The
  inspector gets an Accessibility section (role, name, actions) and an audit (focusable controls
  without a name, relations to no node).
- **E9e The sandbox's page.** A Language and direction page: a language picker over string tables
  in `Data/Assets/ui/sandbox/i18n` (English, pseudolocalized English, and a right-to-left one), a direction
  toggle, mixed-direction text and a text field to edit it, and `--narrate`, a bridge that prints
  each focus change and announcement as a screen reader would speak it (the console stands in
  for text to speech). Image tests gain the page in a right-to-left language.

Rulings (user, 2026-10-17): translation stands on the engine's own foundation.i18n, revised and
built first, with named keys (its dotted ids), not source strings; a table holds its texts,
plural entries and each locale's plural rule and direction, and other formats (CSV) are the
pipeline layer's to convert. Styling by language is `:lang()`, per subtree, not a media feature.
In mixed-direction text the arrows move the caret visually, delete and backspace logically.

- **E9a as built.** `GUIContext::SetLocalization` (a foundation.i18n `Localization`, the host's);
  its `OnChanged` re-resolves every translated property in the context's roots and restyles them.
  A reflected property marked `translatable` (a text, a tooltip, a placeholder) takes `@key` in
  markup through `Node::BindTranslation`, with `<prop>-count` and `<prop>-args` (values or
  bindings); `@@` is a literal @, and a property not so marked takes @ literally. A node resolves
  when it joins a GUI and keeps its key for the inspector. `auto-translate` (inherit, always, off)
  and `lang` inherit down the tree; `:lang()` matches by prefix and counts in specificity.
- **E9b as built.** `Node::SetLayoutDirection` (inherit, ltr, rtl; CSS's `direction`, a slot of
  its own) and `IsRightToLeft`: the first ancestor that says, else the GUI root's Localization's
  language. Unlike the plan, a change restyles the subtree as well as laying it out, since
  text-align's start and end and the logical sides resolve by it in the style pass (the direction
  is applied first there). Flex, grid and stack lay out as left to right with each child's left
  and right margins swapped, then mirror x within the box (or the content, when wider): the same
  answer as running on logical axes, in one place. `margin-inline-*`, `padding-inline-*` and
  `inset-inline-*` (and their two-value shorthands) resolve to physical sides by the node's own
  direction when applied; beside a physical property for the same side, the logical one wins.
  Dock takes `start` and `end` by its container's direction. The vertical scroll bar goes to the
  inline end. A horizontal slider runs from the right; tab strips and menu bars take Left and
  Right along the way they run. `mirror-on-rtl` flips a node's drawing about its centre, through
  the style transform. A right-to-left horizontal scroller still starts at offset 0 (its left);
  starting at its inline start waits for a use.
- **E9c as built.** The formatter (`LayOutText`) resolves each paragraph's levels through
  `BidiResolverFactory` (its base the Text's direction, which a widget takes from its node),
  splits a piece where the level changes, wraps in logical order, then places each line's pieces
  in visual order (L1 by piece, L2 through `ReorderLine`), each shaped through `Shape` in the way
  it reads. Text with nothing right to left in a left-to-right paragraph takes the old path, so
  the image tests are unchanged. `TextHAlign::Start` and `End` resolve per line and are the
  defaults; text-align's start and end map to them. A caret has an affinity (upstream: after the
  text before it); `OffsetAt` reports which edge it found. Left and Right walk a line's caret places
  sorted by x through `ITextCaretPlaces`, which every text field provides; word moves, deletes, Home
  and End stay logical, and a selection collapses to its logical start or end. Ranges draw as the
  boxes their glyphs cover. GUISandbox installs foundation.fonts.bidi; not HarfBuzz yet, so Arabic
  draws unjoined (the stb shaper), which E9e's page needs settled: installing it moves Latin
  glyphs by their kerning and the image references with them. `direction: auto` (a paragraph's
  first strong character, for text the user types) waits for that page too.
- **E9d as built.** `:accessibility` holds the model (AccessNode, AccessTreeUpdate,
  AccessActionRequest, IAccessibilityBridge, roles, states, actions); `AccessibilityImpl.cpp` builds
  it. An id is the node's element handle (slot and generation), so a request resolves back. A role
  is the type's `accessRole` attribute (a base's when its own has none) or markup's `role`; name,
  value, range, description, expanded and secret are reflected properties marked `access`; checked,
  mixed, selected and editable are the selector states; `accessActivate` on a type advertises
  Activate. Markup's `role`, `aria-label`, `aria-labelledby`, `aria-describedby`, `aria-controls`
  and `aria-hidden` are reflected UIWidget properties. A node is marked by `Invalidate` and by a
  change of its children; the walk sends each marked node and the included node above it. Bounds
  are in the window's pixels. Item views expose the rows they have made, not every item, which
  waits for virtualized rows to have a model of their own. The inspector shows a node's
  accessibility and an audit (a focusable control with no name, an aria relation naming no widget).
- **E9e as built.** The Language and direction page reads `i18n/sandbox.strings.xml` (a
  StringTable in English and Arabic through the XML serializer; `strings.py` beside it writes the
  table from its texts). Its buttons switch the language (English, English pseudolocalized, Arabic)
  and the page's own direction; a count from the GUI's data takes Arabic's plural forms. The
  sandbox shapes with HarfBuzz and bakes DejaVu Sans Mono over U+0020 to U+06FF for Arabic (Roboto
  has none; a font fallback list in the lab's text would let Roboto keep the Latin); Roboto's bake
  takes Latin Extended-A, which pseudolocalization's accents were narrowed to. `--lang` and
  `--narrate` (each focus change and announcement printed as a screen reader speaks it). The image
  tests add `language.ar`; every reference changed with HarfBuzz and the sidebar's entry, and the
  user approved the new set (2026-10-17).

Not in E9: the platform adapters (UI Automation, NSAccessibility, AT-SPI) and real text to speech,
which are host work with hosting; vertical text; locale-aware number and date formats.

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

Ruling (user, 2026-10-17, after E9): not before a testing campaign shows the lab as stable as
foundation.ui. The campaign, the decision and every gap the phases left are in
`Plans/backlog-0.2.md` ("The GUI lab after E9").

## Open questions

None left from the draft; new ones go here as the phases meet them.
