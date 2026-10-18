# The GUI lab's second evolution: the gaps the field still shows

> STATUS: APPROVED 2026-10-18 (user: "approved"; theming discussed the same day, see E13).
> Follows `gui-lab-evolution.md` (E1 to E9, built on `vg`). Read CONVENTIONS.md first.

## Why

The 2026-10-17 assessments measured the lab again, after E1 to E9, against foundation.ui and the
frameworks beside the engine (Godot, eepp, RmlUi, GPUI, AUI, Zero, and the UIs of traktor,
LunarSong, Lumix, ezEngine and doriax). They are kept locally (`Code/Experimental/GUI/Assessments/`,
`README.md` the index). Feature for feature the lab's core now stands where foundation.ui's does:
of 35 core areas it is ahead in 21, at parity in 7, and behind in 4 (theme assets, keyboard
breadth, the animation API, hosting). Every foundation.ui control has a counterpart.

What the comparisons still name, across the field, ranked by how many show it and what a game's
screens and the editor would hit first:

| Gap | Shown by |
|---|---|
| Engine hosting (a scene component, reference-resolution scaling, world-space canvases, input from the game) | ezEngine, LunarSong, Lumix, doriax, traktor, foundation.ui |
| Font fallback, and font properties in CSS (`font-weight`, `font-style`, `@font-face`) | Godot, eepp, RmlUi, GPUI, AUI |
| Platform accessibility adapters | Godot, GPUI, eepp |
| Theme assets: SVG icons and drawables, nine-patch and atlas skins from CSS, palettes, `@import` | foundation.ui, eepp, RmlUi, traktor |
| Retained paint: reuse what did not change across frames | GPUI, Godot, RmlUi, eepp |
| `calc()` (E2 designed it) | RmlUi, eepp, Godot |
| Keyboard breadth: punctuation and keypad keys, tab index, Tab passing through, Alt mnemonics, capturing the next key, menu keys through the keymap | foundation.ui, GPUI, ezEngine |
| The animation API: eased tweens of any property, groups, springs | Zero, GPUI, foundation.ui |
| Editor breadth: property grid, docking with tear-off, command palette, colour picker, code editor | Zero, traktor, Godot |
| Test depth: the tester used by the core's own tests, screenshots on failure | AUI, LunarSong |
| Multi-touch, observable geometry, a keyed repeat in any container | AUI |

The inventory adds what the reading found thin (its section 19), and the 0.2 backlog ("The GUI lab
after E9") what each phase left. This document is the design for the next round of them.

## What this round is for

The user ruled after E9 (2026-10-17): before anyone thinks about the lab replacing foundation.ui,
a testing campaign has to show it as stable as foundation.ui. The campaign runs at the end of this
round (ruling 6), on what it builds: E10 builds the tools the campaign runs on, and the phases
after it close gaps a real screen would meet, so the campaign tests the lab a game would use
rather than the one it has today. It does not start the toolkit or the gamekit, and it does not decide the replacement.

## Principles

The first evolution's hold: one mechanism per concern, chosen by surveying the references rather
than porting one of them (no frankengui); the core holds what foundation.ui's core holds, editor
widgets go to a future toolkit and game screens to a future gamekit; CSS-shaped where CSS has a
name and a behaviour; every property reflected and reachable from sheets, markup and both script
languages; every behaviour with a test and the sandbox showing it; no compatibility shims. Two
more:

- **Measured, not assumed.** A performance phase states its budget and measures it in
  RelWithDebInfo (Debug on Vulkan is too slow to time), before and after.
- **A real screen per phase.** Each phase ends with something a game would build (a menu, a HUD,
  a settings screen, a rebinding screen) in the sandbox, not only a feature page, so the campaign
  has screens to test.

## Rulings so far (user, 2026-10-18)

1. Hosting is not in this round (the first evolution's ruling 5 holds).
2. Theming (E13) and the font work (E11): yes.
3. Platform accessibility adapters: not yet.
4. No toolkit yet.
5. Every phase is in: E10 and E12 to E17 beside E11 and E13.
6. The testing campaign runs at the end, after the phases (E10's tools are built first, and used
   then).
7. Popups that open a window of their own wait for hosting. Not every platform has real windows, so
   they will need virtual windows too, as foundation.ui has (a window drawn inside the root's
   surface where the platform gives only one).
8. Line breaking by Unicode's rules (UAX #14, libunibreak or our own) is deferred: E11 keeps breaking
   at spaces, and adds only the break anywhere a word too long for its box needs.
9. Skins belong in themes. The token set is to be as large as useful control over styling needs
   (the user did not fix a size): E13's layered tokens are the proposal.

The first evolution's rulings (2026-10-16) still apply where nothing here changes them.

## The phases

Each phase is a layer with its tests and a sandbox page or screen. The order: the campaign's tools
first, then text (every screen has it, and Arabic or CJK fails without a fallback), then the
keyboard (a game's rebinding screen), then styling breadth and theme assets (what a themed game
looks like), then motion, then rendering cost, then what scripts and overlays still cannot do,
then direction and accessibility's leftovers.

### E10 The campaign's tools: fuzzing, sweeps, soak and test depth

Facts: 812 test cases, of which only GUI.Testing's 6 drive the GUI through the tester; the core's
tests inject coordinates by hand. The tester has no right click, hover, pad presses or file drops.
Two image tests (`GUI.Images.vulkan`, `.webgpu`) over 14 references. GUISandbox has `--sweep`,
`--screenshot` and `--scroll`. The markup, CSS and keymap parsers and the dispatcher have never been
fed malformed or random input. No soak run has been made. The inventory's section 18 lists faults no
test pins.

Design:

- **Fuzz harnesses** for the CSS parser, the markup loader, the keymap's chord parser, the string
  table reader and the event dispatcher (a random stream of pointer, key, text, touch and drag
  input against a sandbox page). Each is a test executable that runs a fixed corpus and a seeded
  random run in ctest (so every lane runs it) and, under clang, a libFuzzer entry for long runs
  (`-fsanitize=fuzzer,address`). A crash it finds becomes a corpus file and a regular test.
- **The tester in the core's tests.** The tester gains right click, hover and leave, wheel, pad
  buttons and sticks, touch, and file drops; new behaviour tests are written through it, and the
  tests that hand-inject coordinates move to it as their areas are touched. Its failures print the
  node tree around the target and, with a device, save a screenshot beside the test's scratch data
  (AUI's and LunarSong's testers do both).
- **Sweeps in ctest.** `--sweep` over every sandbox page on Vulkan and WebGPU becomes a ctest entry
  (skipped without a device, code 77, as the image tests are): every page loads, lays out, draws a
  few frames and is navigated by keys and pad without a warning or an error in the log.
- **Soak.** `GUISandbox --soak <minutes>` cycles every page with scripted input, writing frame time
  percentiles and live allocation counts (the allocator's) to a CSV; a growing count or a frame
  time drifting up is a finding. Run in RelWithDebInfo.
- **Pins.** Every fault section 18 lists gets the test that would have caught it.
- **A real screen.** A game's title, pause menu and HUD built on the lab in the sandbox (the
  backlog's "a real screen or two"), copied from a sample's look (Sky Hopper's or Masquerade's), as
  the campaign's first target.

Tests: the harnesses themselves; the tester's new inputs; a sweep entry; the soak's CSV written and
parsed.

### E11 Text: fallback fonts, font properties, direction from content

Facts: a run has one face; the Language page draws everything in DejaVu Sans Mono because Roboto
has no Arabic. CSS has `font-family` and `font-size`; no `font-weight`, `font-style` or `@font-face`.
`direction` is `ltr` or `rtl` or inherited; nothing takes it from the content. Word moves, deletes,
Home and End are visual in mixed-direction text, and a selection collapses to its visual end.
Segmentation is partial (no Hangul jamo or Indic conjuncts); lines break only at spaces
(`Text/Text.cppm:321`), so CJK and long words overflow. HarfBuzz is the sandbox's shaper, not the lab's default.

Design (the references: Godot's font fallback chains and `TextServer`, RmlUi's font effects and
`@font-face`, GPUI's font fallback through its text system, eepp's `FontFamily`):

- **A font stack per run**: `font-family` takes a list (`"Roboto", "Noto Sans Arabic", sans-serif`),
  resolved to faces by the font service; shaping splits a run where a face lacks a character and
  takes the next face that has it (the F3 shaper's request already carries a font list). A generic
  family at the end maps to the engine's bundled fonts.
- **`@font-face`** in sheets: a family name, a source (a VFS path, through the resource provider),
  a weight and a style; `font-weight` (100 to 900, `normal`, `bold`) and `font-style` (`normal`,
  `italic`) choose among a family's faces, the nearest weight as CSS chooses it.
- **`direction: auto`** (and `unicode-bidi: plaintext` on text fields): the paragraph's direction
  from its first strong character, for typed text and translated strings alike.
- **Logical editing in mixed text**: word moves, deletes, Home and End by the text's logical order,
  a selection collapsing to its logical end (the caret's visual moves from E9c stay for the arrows).
- **A word too long for its box breaks** where nothing else fits (CSS's `overflow-wrap:
  break-word`), so a long name or path no longer runs out of its box. Line breaking by Unicode's
  rules (CJK, punctuation) and fuller segmentation are deferred (ruling 8).
- **HarfBuzz as the lab's default shaper**, the basic shaper the fallback when HarfBuzz is absent.
- Small ones the backlog lists: a password field taking no composition; a link showing the pointer
  cursor.

Tests: fallback across faces in one run; weight and style selection; `@font-face` loading through a
fake provider; auto direction; logical word moves in mixed text; long-word breaking; the
Language page in its real fonts (an image reference the user approves).

### E12 The keyboard's breadth

Facts: the lab's key codes stop at the editing and navigation keys, the letters, the digits and F1
to F12 (`Core/Event.cppm:204`); punctuation and keypad keys arrive only as text. Tab order is tree
order; there is no tab index, and Tab cannot pass through to a widget that wants it. A menu's
arrows, Return and Escape are widget code, not keymap commands. Every radio and every tab is a tab
stop. There is no way to capture the next key before the keymap and navigation see it, and no Alt
mnemonics. Type-ahead reads keys, not text input.

Design (foundation.ui's `TabIndex`, GPUI's key contexts and action dispatch, ezEngine's input
sets, Godot's `shortcut_context` and `accessibility` focus groups, WinUI's access keys as a
reference for mnemonics):

- **The full key set**: punctuation, the keypad (with its own codes, so a game binds Numpad 5
  apart from 5), media and browser keys left out; the shell bridge maps every SDL key it gets.
- **`tab-index`** as CSS and a reflected property: a positive index ordered first, 0 in tree order,
  -1 focusable but skipped by Tab (HTML's rule).
- **Focus groups**: a container with `focus-group` (radio groups, tab bars, toolbars) is one tab stop;
  arrows move inside it (a roving tab index), and it remembers the last focused member.
- **`tab-captures`** on a widget (a code editor, a terminal): Tab and Shift+Tab reach it as keys
  instead of moving focus; Ctrl+Tab still moves.
- **Menus through the keymap**: a menu's moves, activation and closing become commands in a `menu`
  key context, so a game remaps them like any binding.
- **Capturing the next key**: `CaptureNextKey(callback)` on the dispatcher takes the next key or pad
  button before the keymap, navigation and focus see it (a rebinding screen); Escape cancels unless
  the caller asks for it too.
- **Mnemonics**: a label's `&` marks its access key (`&File`), shown underlined while Alt is held
  (or always, as a style), and Alt plus the key activates it in its window or menu.
- **Type-ahead from text input**, so it follows the keyboard layout and IME.

Tests: keypad and punctuation codes through the bridge; tab index order; a focus group's arrows and
memory; Tab captured; a menu rebound; capture and cancel; mnemonics; type-ahead in a non-Latin
layout. Screen: a rebinding screen.

### E13 Styling breadth and theme assets

Facts: no `calc()`; colours without `hsl()` or `currentColor`; radial gradients of one shape; borders
drawn solid whatever their style; no `background-size` or `background-position`; media queries
without `orientation` or `not`; `@import` skipped; images are referenced by name, SVG icons are not
CSS images, and a nine-slice drawable exists (`Drawing/NineSliceDrawable.cppm`) with no CSS form.
The built-in themes write their colours directly, not through a palette of custom properties (E3's
design). Parts take no motion and no text properties; `inherit` on a non-inherited property takes
the parent's cascaded value, not its live one.

Design (RmlUi's decorators and `@spritesheet`, eepp's CSS drawables, foundation.ui's SVG drawables
and atlas, CSS itself):

- **`calc()`** over lengths, percentages and numbers, resolved where percentages are (E2's design).
- **Colours**: `hsl()`, `hsla()`, `currentColor`, `color-mix()`.
- **Images as CSS values**: `url()` naming a VFS path or an `@icon`; SVG drawn at the size it fills
  and its device scale, cached by size (foundation.ui's SVG drawable); `background-size`
  (`cover`, `contain`, lengths) and `background-position`.
- **`border-image`** for nine-slice skins (slices, widths, `stretch`, `repeat`, `round`), and
  `@spritesheet` atlases (RmlUi's) so a theme names a region of one texture.
- **Border styles**: dashed and dotted, drawn by VG (a new dash pattern in its stroker), shared with
  foundation.ui.
- **Media**: `orientation`, `not`, `prefers-reduced-motion` and `prefers-contrast` from the host.
- **`@import`** through the resource provider.
- **Themes** (discussed with the user 2026-10-18; Material's design tokens and Godot's per-control
  theme items are the references). A theme is a CSS file in the data at the user-agent origin; the
  built-in dark and light themes stay embedded as the fallback, and a game ships its own as an
  asset, hot-reloaded as sheets are. Its tokens are custom properties on the root, live (a game
  retints at runtime, a subtree overrides them, `var()` reaches drawables), in three layers:
  1. **Base tokens**, about fifteen colours (accent and the text on it; surfaces base, raised and
     sunken; text, dim text, border, focus ring, selection; danger, success, warning) and the
     metrics (three corner radii, a spacing scale, font sizes small, body, title and heading,
     border and focus-ring widths, motion's fast and slow durations and its easing). A theme that
     sets only these restyles everything consistently.
  2. **Derived states**: hover, pressed and disabled colours mixed from the base by default
     (`color-mix()`); a theme that wants its own sets them.
  3. **Widget tokens**: every part of every widget reads a token of its own falling back to the
     base (`button { background: var(--button-background, var(--accent)) }`), so a theme changes one
     control alone. The list is generated from the built-in sheet, documented, and shown by the
     inspector beside the part that reads it.

  Skins are tokens too (ruling 9): a widget token may hold a drawable (`border-image(...)`, an SVG,
  a gradient) as well as a colour, and a theme brings its fonts with `@font-face`, so a game-styled
  theme overrides widget tokens with its art through the same mechanism. Light, dark and
  high-contrast are `@media (prefers-color-scheme)` and `(prefers-contrast)` blocks in one theme,
  chosen by the host's flags. The lab does not read foundation.ui's `.sss` themes.
- **Parts take motion and text properties**; `inherit` reads the parent's live value.

Tests: each value form parsed and drawn; an SVG at two scales; a nine-slice skin; dashed borders;
media queries from host flags; an import chain with a cycle; a theme retinted by its palette. Screen:
a settings screen in a skinned theme.

### E14 Motion as an API

Facts: sheets have transitions and keyframes (E3); code and scripts have eepp's actions
(`Scene/Actions.cppm`: move, fade, scale, delay, sequence, tick), which interpolate linearly and
cover four properties. Nothing tweens an arbitrary reflected property, runs a group in parallel,
or follows a spring.

Design (Zero's action system and its eased tweens of any property, GPUI's animations and springs,
foundation.ui's `Storyboard`, Godot's `Tween`):

- **Tweens of any reflected property**: `node.Animate(property, to, duration, easing)` over the same
  interpolation the sheets use (`StyleInterpolation`), so a colour, a length, a transform or a
  shadow tweens from code exactly as from a sheet. CSS timing functions and the named eases.
- **Groups**: sequences and parallel groups, delays, repeats, ping-pong, and completion callbacks;
  a tween of a property replaces the running one of the same property, as foundation.ui's channels do.
- **Springs**: a damped spring toward a target (stiffness, damping, mass), retargetable mid-flight
  without a jolt, for drags that settle and menus that overshoot.
- **One clock**: tweens, transitions and keyframes advance on the root's clock, which a game's time
  scale may leave alone (menus animate while the game is paused), and stop with their node.
- The eepp actions fold into it, their call sites moved (no shims).
- Scripts reach all of it.

Tests: every value kind tweened; groups and callbacks; replacement per property; springs settling
and retargeting; the paused-game clock; scripts in both languages.

### E15 Retained rendering

Facts: a drawn frame rebuilds every visible node's geometry; culling skips what is off screen and
quiet frames skip drawing, but a frame that draws redoes it all. Hit testing walks the tree with no
spatial index.

Design (GPUI's retained element frames, RmlUi's geometry caching per element, Godot's canvas items,
eepp's frame buffers per node):

- **Recorded draw lists per node**: a node's own drawing is recorded once into a VG command list and
  replayed while nothing it reads changed; a change invalidates that node's list alone. Layers
  (VG's offscreen layers) can be kept across frames for a subtree marked `will-change`.
- **Damage**: the region that changed, so a host that can present a part of a frame (or a game that
  redraws its HUD over the scene) does less.
- **A hit-test index** over the frame's visible boxes, rebuilt with the draw lists.
- **The budget**, measured on the stress page and a long list in RelWithDebInfo: a frame where one
  label changes costs a fraction of a full redraw, stated in the phase's notes with the numbers.

Tests: replay identical to a fresh draw (the image tests); invalidation reaching exactly the
changed nodes; the index agreeing with a tree walk. Measurements in the notes.

### E16 What scripts and overlays still cannot do

Facts: scripts cannot remove a listener, or build menus, windows, popups, models, actions or
localized strings; a shrunk menu clips instead of scrolling; tooltips follow the pointer only and
are never interactive; a popup never leaves its root (no OS windows); modality is per root.

Design:

- **Scripts reach the rest**: listener tokens with removal; menus, windows and popups built from
  scripts; models (a script array as a list model); the localization's keys; actions and tweens
  (E14). Each through reflection, in both languages.
- **Menus scroll** when they do not fit, with the arrows and wheel; **keyboard tooltips** (shown for
  the focused control after a delay) and **interactive tooltips** (a delay before closing, so the
  pointer can enter them).
- **Popups in windows of their own** (menus and tooltips that leave their window, for the editor)
  wait for hosting (ruling 7), with virtual windows where a platform has only one surface.

Tests: script-built menus, windows, models and removal in both languages; scrolling menus; focus
and interactive tooltips.

### E17 Direction, accessibility and touch, the leftovers

Facts: several widgets and popup placement do not follow the direction; a right-to-left horizontal
scroller starts at its left; item views expose only the rows they made to the accessibility tree;
touch is one pointer (no pinch or rotate).

Design:

- **Direction everywhere**: every widget and popup placement checked under `rtl` (a sweep of the
  sandbox in Arabic, as the Language page does), fixes as they come; scrollers start at their inline
  start.
- **Virtual accessibility nodes** for item views: every item has a node (from the model), the made
  rows give theirs bounds, so a screen reader can count and reach items not on screen.
- **Multi-touch**: pinch, rotate and two-finger pan as gestures on the event route (with E1b's touch
  model), a touch slop scaled by the display.

Tests: the Arabic sweep; item counts in the accessibility tree; gestures from scripted fingers.

## Not taken in this round

- **Engine hosting** (a scene component, reference-resolution scaling, world-space canvases): not in
  this round (ruling 1).
- **Platform accessibility adapters and text to speech**: not yet (ruling 3); host work, after the
  lab has a host.
- **Editor breadth** (property grid, docking with tear-off, command palette, colour picker, code
  editor): the toolkit's, which does not start yet (ruling 4).
- **Game screens** (screen stacks, prompts, toasts): the gamekit's, after the decision. E10's real
  screen is built from the core's widgets in the sandbox, not as a gamekit.
- **The inspector as MCP tools**: its own phase, once the lab is hosted (0.2 backlog).

## The end state

When E10 to E17 are done, the campaign has its tools and runs on a lab with the text, keyboard,
styling, motion and rendering a game's screens need; the gaps the field shows are hosting, the
platform adapters and the editor's breadth, each the next stage's rather than the core's. The
decision the 0.2 backlog keeps (replace foundation.ui's core, or move pieces across) is still made
after the campaign, not by this round.

## Open questions

None left from the draft; new ones go here as the phases meet them.
