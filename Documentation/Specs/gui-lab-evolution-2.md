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

As built on `vg` (2026-10-18): GUI.Fuzz.Tests (45b95a01f) runs five targets in ctest (the CSS
parser and cascade, the markup loader with layout and drawing, the keymap's bindings, the string
table reader, input streams on every sandbox page), seeded from the sandbox's own files and
`Data/Tests/GUI/Fuzz/`, with `GUI_FUZZ_RUNS` mutations a seed (40 by default: six seconds); with
`OPTION_GUI_LIBFUZZER` in a tree built with `-fsanitize=fuzzer-no-link,address` and
`BUILDSYSTEM_OUTPUT_SUFFIX=-Fuzz`, each is a libFuzzer executable. Two minutes of each found one
crash, a string table read short of its locales' cells (fixed on master, 66aeb480a; kept as a seed);
an ASAN run of 300 mutations a seed found nothing more. The tester leaves, scrolls, taps, swipes and
hands each failure to a capture (ebd4bfafa), and tests drive every input it has. The sweep walks each
page by keys and the pad too, fails on a warning or an error, and runs as `GUI.Sweep.vulkan` and
`.webgpu` (388 steps, clean on both); `--soak <minutes>` writes frame time percentiles, live
allocations and resident memory to a CSV every ten seconds (ccd11ce18). The real screen: the
showcase's Play now leads to a HUD and a pause menu (8eab45e50). The faults the inventory's section
18 listed were already pinned by the bug round of 2026-10-17.
Left: the input fuzzer's streams are long (74 runs in two minutes under libFuzzer); a failure
capture is a seam only, no host saves a screenshot yet; the soak has not run in RelWithDebInfo.

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

As built on `vg` (2026-10-18): `font-family` takes a list; the first family the font service has is
the font and the rest, up to four, are the faces a character it lacks falls back to; a piece is
shaped with the whole list and its glyphs come out as one run per face (59724859a). `font-weight`
and `font-style` inherit and choose a family's face in CSS's order, the style first, among faces
named as font files name them ("Roboto Bold", "Roboto Light Italic"), synthesizing bold and italic
only where no face has them; a run's `<b>` is bolder (579755b0a). `@font-face` names a face and its
file, and the style manager asks a loader the host gives for each, once per sheet (e9bb4733f).
`direction: auto` runs each paragraph as its first strong character reads, one with none as its
node does, its boxes as its parent's (43d86c028). In a right-to-left paragraph a word to the left is
the next one and Left collapses a selection to its logical end; deletes, Home and End keep to the
text's order (9f39cfa19). `overflow-wrap: break-word` breaks a word wider than its line between
graphemes (64a5919da); the style slots passed 64 on the way, so the pins are two words (5d65fefe6).
A rich text's link shows the pointer hand (b75a7ba94). A password field's text input is secret, so
the platform's IME keeps out of it: a text input kind on the shell, on master too (3673074e5,
master b2e1b288f). HarfBuzz: the lab shapes through `ITextShaper` and names no shaper, its hosts
install HarfBuzz before loading fonts, and now GUI.Tests does too; that showed a piece measured by
its characters but drawn shaped, now measured as shaped, and a caret that skipped the inside of a
ligature, now stopping at each grapheme in one (131667e6d). The Language page draws in Roboto with
DejaVu behind it, and the sandbox loads Roboto's bold through `@font-face` (0d8a2f3f9, references
approved, 6fe3e3726). On the way: the gcc lane had not built five of the lab's implementation units
(a4ce90633), and the web build had stopped configuring (b3a0a9545, master 155c735c6).
Left: a bold or italic run's fallback faces are the families' regular ones; fallback faces come only
from the list (no generic families mapped to bundled fonts); a font with no shaper draws its
fallback stretches in the run's own face.

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

As built on `vg` (2026-10-18): the lab's key codes now cover the keyboard (punctuation, the keypad
with codes of its own, F13 to F24, the lock and system keys, the modifier keys alone), one table
giving each its binding name and display text, and the bridge maps every key the shell reports; a
modifier key pressed alone matches its own binding. `CaptureNextKey` takes the next key or pad
button before anything else, a modifier alone as it is released, dropping the text a taken key
types; Escape cancels unless asked for. Type-ahead reads typed text, matching letters in either
case in Latin, Greek and Cyrillic; lists and combo boxes claim the keys that type and ask for text
input to get them (ca2080584). `tab-index`, `focus-group` (a tab strip is one by default; a radio
is its group's current member) and `tab-captures` (b6f6796e8). A menu's moves are `menu.up`,
`menu.down`, `menu.left` and `menu.right` in the `menu` key context, the arrows by default; a
binding whose command nothing on the focus path handles is now as if unbound. An `&` in a button's
or a menu item's text marks its access key, underlined while Alt is held (or always), Alt and the
key activating it in the open menu or the focus scope (e9e324f38). Scripts get `gui::captureKey`
and `gui::keyText`, and the showcase a Controls page that rebinds four actions, a taken key
swapping (f742c1d26).
Left: asking for text input over a list would raise an on-screen keyboard on a touch platform (the
lab has none yet); access keys do not reach a label's `for` target; Ctrl+Tab from a widget that
captures Tab inside a tab view is the tab view's (it switches tabs) before it moves focus.

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

As built on `vg` (2026-10-18): `calc()` lengths, a sum of units, the percentage resolving where
percentages do (29360ffcb); `hsl()`, `hsla()`, `color-mix()` in sRGB and `currentColor`, the
nearest declared colour, through the cascade (2a0dac384); media query lists, `not`, `orientation`
and the user's preferences, `prefers-color-scheme`, `prefers-contrast` and
`prefers-reduced-motion`, which a host sets on its GUI (308c7961e); `@import` through the
resource provider's new `LoadText`, a cycle stopping (95775b47d); `background-size` and
`background-position` (39a54cd9f); SVG images drawn as paths at the size they fill (`LoadSvg`;
since 2026-10-19 an Image's `src`, a menu command's icon and a rich text's `[icon=]` too, by an
`.svg` path, and an `SvgDrawable` made from SVG markup for an icon set compiled into a binary;
an SVG written inline anywhere a picture's source goes, a `data:image/svg+xml,...` URL, plain,
percent-encoded or base64, which the resource provider parses once and keeps; and an SVG
`border-image`, sliced in its own units, each piece drawn as paths clipped to its place; since
2026-10-20 baked: a host's `IPictureBaker` (the sandbox's `GpuPictureBaker`, `GUI.Bake`) draws the
document once into a GPU texture at the size its corners land in device pixels, which the pieces
then sample as premultiplied quads (VG's `DrawImagePremultiplied`), paths only while a bake is
queued; the sandbox's Pictures page, 40 framed tiles, went from 680,383 vertices and 36.7 ms of
GUI draw a frame to 5,779 and 0.2 ms),
`border-image` nine-slice skins (stretch, repeat, round, fill) and `@spritesheet` atlases written
as `url(atlas.png#xywh=...)` (202d678bb); dashed and dotted borders through VG's dash patterns,
the lab's alone, no Foundation change (8b433b342). The built-in theme is one sheet of tokens on
`:root` (a new pseudo-class) in the three layers, light and more contrast as media blocks, its
values the old themes' to the pixel; a part inherits its widget's style, so tokens reach it
(4dedc5a54). Skin tokens bring a theme's art for buttons, fields, combo boxes, windows and menus;
a combo box's and a list's own face gives way to a sheet's fill (08aeff4fa). Screen: the
sandbox's Themes page, the same settings in the built-in tokens and in an Ember subtree skinned
with art its own script draws (e17b642d5). After the screen's check: dashed and dotted borders
redone per side, each pattern fitted to start and end on a piece and the sides meeting on the
corner's diagonal, square or rounded (4b80fb824); radius tokens beside the skin tokens, so a
focus ring follows rounded art (4f0abfcd0); list rows of every kind show the pointer over them
(e2ff1536f); a window placed in a tree closes from its close button (80771da01).
Left: parts take no transitions or text properties yet, and `inherit` reads the parent's
cascaded value, not a value it is animating through (both want per-frame work of their own);
`@icon` is not read; a background with no `background-size` still fills its box (the lab's old
default, not CSS's natural size); `@import` paths are from the data root, not the importing
sheet; the token list is in the theme's source, not generated into a document, and the inspector
does not show a part's token beside it.

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

Decided (2026-10-18), from the code as it stands: a tween's property is named as a sheet names it
when the property table has it, and composes as CSS's animation origin does, over the cascade and
the sheet's own transitions: the tween writes a value into the node's animated layer and the
style pass applies it, so a sheet's later change to the same property does not undo a tween that
holds. A tween with no `from` moves from the underlying value (what the cascade would show,
Web Animations' implicit keyframe), and holds its end until stopped or replaced (a fill of
`forwards`; `Stop` gives the property back to the sheet). A name the property table does not
have is a reflected property (`value`, `x`, `textColor`), read and written through its accessors,
which pin it as code's, numbers, colours and points interpolating. The named eases are
easings.net's ten families in, out and in-out, Foundation's curves, written `ease-out-back` in
sheets and code alike. Of the eepp actions only `TickAction` has callers outside tests; the others
go, their tests moving to the tweens.

As built on `vg` (2026-10-18): named eases, easings.net's ten families in, out and in-out over
Foundation's curves, in sheets and code (f80dda733). `Node::Animate` returns a `Tween`; a sheet's
property on a node a sheet styles goes into the node's animated layer (`MotionValue`), which the
style pass composes over the cascade and its transitions, applying the cached style again when only
motion changed (`MarkStyleMotionDirty`); a tween holds its end until `StopAnimation`; any other name,
or any property of a node no sheet styles, moves through its reflected property; a tween waits,
repeats, runs back and forth and calls back; `background-color` and `box-shadow` gained CSS's
initial values (7f079d3dc). Groups: `TweenSequence` and `TweenParallel` of tweens, waits, calls and
groups, repeated as a whole, each step starting with the time the last ran past its end
(`Action::GetOverrun`); a tween's start takes its property from any other, inside a group too, and a
replacement starts from the value showing now (10158f57b). Springs: `Tween::Spring`, `Retarget`
and `Node::SpringTo`, the speed carried across a retarget in the value's units where both spans have
a measure (214401c6a). eepp's Move, Fade, Scale, Delay, Runnable and Sequence actions and
`ActionInterpolation` removed, their tests moved to tweens; `TickAction` stays (087307716). Scripts
in both languages: `node.tween` (an expander's `animate` property took the name), `createTween`,
`springTo`, `stopAnimation`, `play`, and the tweens' and groups' chains, `repeats` for repeat, a
word of Luau's (2511db13d). The GUI's clock was already each root's, with its own time scale and
pause, apart from a game's. Screen: a level's end on the Motion page, played by its script, and the
named eases beside CSS's (fd87ea59a).
Left: a tween of a sheet's property needs a style pass to show (a GUI with no StyleManager moves
only reflected properties); a group repeats but does not run backwards (ping-pong is each tween's);
a spring retargeted across shadows, transforms or gradients keeps its speed in progress, not in
the value's units; a node's own scale is not reflected, so only a sheet's `transform` scales.

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

Measured first (2026-10-19, RelWithDebInfo, `GUISandbox --bench <page>`: 960 x 720 offscreen, the
page title's text changed each frame), the draw was not where the time went: the GUI's draw took
0.09 ms on Controls, 0.11 ms on Data views and 0.22 ms on Stress at load 1, against 1.0 to 1.5 ms of
GPU work and waiting, and culling already kept the drawing to what shows (99 to 170 nodes). What
grew with the load was the walk: at load 20 the draw took 1.62 ms, visiting 7984 nodes only to cull
them beside the 170 it drew. Ruling (user, 2026-10-19): aim E15 at the walk, not at recorded draw
lists; keep damage.

As built on `vg` (2026-10-19): `GUISandbox --bench <page> [frames]` prints a frame's cost by stage
(3b3cffea5, bffc9c392, e09c4443b). VG's `GetLocalClipBounds`, the clip in local space (d8f94dcd0).
A node with 32 children or more keeps an index of where each child's drawing reaches, ordered
along the axis they spread on; drawing asks it which children reach the clip and hit testing which
hold the point; `Invalidate` notes the child a change came up through so a check reads those
alone, and the index is built again only when a reach moved; the node's subtree ink reads the
index's bounds, and whether it has stacked children is kept until a z-index or the children change
(c3ba4a460). Damage: each drawn node keeps the device area its subtree covered, tells its root
where it was when it changes, moves, hides or goes, and notes where it is when that is new or moved;
`SceneRoot::GetLastDamage` (993ff9026). The style cache's entries are checked against the element
table, so a destroyed widget's are swept and never pass to one at its address (32aa6381a, a report
from the read of the code that came with Sedulous's port).

Numbers after, the same bench: load 20, 1.62 ms to 0.66 ms, culled 7984 to 5 (the rest is 3000
out-of-view children animating every frame, each reach read again); load 1, 0.22 to 0.20 ms;
Controls and Data views unchanged at 0.09 and 0.11 ms. A frame changing one label damages 3.6% of
the window (the label), where before every drawn frame redrew it all.
Left: no host presents part of a frame yet (the sandbox draws the whole window; the damage is
there for one that can); the GPU-and-wait time grows with the stress load though the geometry is
the same, which is outside the GUI's draw and not yet explained; a node's own drawing is still
tessellated each frame it draws (the ruling's choice).

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

As built on `vg` (2026-10-19): `on` and `onCommand` return a token and `off` removes either, from
inside its own handler too, a node's listeners and handlers holding still while they run
(c9b8fb114). Scripts open windows (`open`, `openModal`, `close`, `isOpen`, `closable`,
`resizable`, a `close` event), menus (`openAt`, `openBelow`, `isOpen`) and popups (`popup`,
`closePopup`) of their own (8f1f0e340, f509160df); a window held by its tree alone outlives its own
close, which had freed it before calling back (c5e6908a4). A script's `ListModel` shows in a view
through `setModel`, and `gui.tr`, `gui.trCount` and `gui.language` read the localization
(a8c7670f9). A menu taller than the window keeps to it and scrolls between two arrows, by the
pointer resting on one, the wheel and the keys (309e4b2ed). The keys moving the focus show the
focused control's tooltip below it, and `tooltip-interactive` gives a tooltip the pointer can
cross into and press in (6d42c7615). Screen: the Menus page's script builds a menu of sixty items
and a window of its own; its buttons show their tooltips from the keys, and one can be entered
(657b9b96a).
Left: popups in windows of their own wait for hosting (ruling 7); a tooltip's content is its
text, so an interactive one has nothing to press yet but itself (a script can open a popup of its
own for richer content); a script's Variant arguments (a format's list) are not taken by
`gui.trCount`, which fills `{0}` with the count alone.

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

Surveyed and planned (2026-10-19), on the lab as it stands and on Godot, eepp, RmlUi, aui, gpui
and foundation.ui:

- **Direction.** The direction model, the layouts and text already follow it; of the widgets only
  Slider, the tab bar's keys, MenuBar and the scroller's bar side read it. Parts placed by hand do
  not (the menu's gutter and submenu arrow, the combo box's arrow, the progress fill, the
  horizontal scrollbar, the number field's buttons, the tree's indent, chevrons and keys, the
  table's columns, the grid's cells, the toggle's knob, the radio keys, the window's grip, the tab
  bar's drag), and popup placement is physical, so menus, combo boxes, context menus and tooltips
  open as in left to right. The plan, one place per concern: popup sides and the start alignment
  logical, resolved in `PlacePopups` against the anchor's direction; a `Node::InlineRect` for every
  part placed by hand, the theme mirroring the directional glyphs (`mirror-on-rtl`); an
  `InlineStep` for the arrow keys; scrollers starting at their inline start (the ruling below); the
  sweep in Arabic as its own test, and a mirror audit laying each page out both ways.
- **Accessibility.** Item views expose the rows they have made, with no item role, in the order the
  rows were made, under ids that pass from item to item as rows recycle; the focus stays on the
  view, so the narrator reads the list's name; finding an item by its id scans the rows. The plan
  (eepp's model, the nearest to the lab's): item nodes from the model under ids that follow the
  item, in item order, roles `listitem`, `treeitem` and `row`; set size, position, level, row and
  column counts and an off-screen state on the node; bounds for made rows only; the focus the
  current item; focus and scroll-into-view actions making the row; an item found by its id in
  constant time.
- **Touch.** The dispatcher follows one finger (a test asserts a second is ignored); every
  threshold is in device pixels (the 0.2 backlog's note); SDL's trackpad pinch and finger cancel
  reach the shell and are dropped, and the sandbox's polled touch follows the first finger. The
  plan (Safari's and aui's model): one recognizer in the dispatcher making one gesture of scale,
  rotation and translation (deltas and totals, about a focal point), as `gesturestart`,
  `gesturechange`, `gestureend` and `gesturecancel` on the event route and in both script
  languages; the slop 8 DIP times the display scale, and every click, tap, pan, drag and fling
  threshold in DIPs with it; the trackpad's pinch into the same events, a finger cancel ending a
  gesture, the trackpad's fingers no longer taken for a touchscreen, the polled path following
  every finger.

Rulings (user, 2026-10-19):

1. A right-to-left scroller uses CSS's signed offset: 0 at the inline start, negative toward the
   left, clamped to [-range, 0]; the thumb, scrolling to the end and the wheel follow the inline
   end.
2. Item nodes for a window, not every item: the made rows and about a page around them, the current
   item and the selected ones, each with its set size and position, so a reader counts every item
   and reaches any of them by the keys or the actions. "Every item has a node" above reads "a
   reader can count and reach every item".
3. Gestures are claimed by `touch-action`, as CSS has it: a node names what the lab may still do
   itself (`auto`, `none`, `pan-x`, `pan-y`, `pinch-zoom`, `manipulation`), the value in force the
   intersection of the node's and its ancestors', and what it leaves out comes to it as gesture
   events from the gesture's start. Nothing looks pressed and then cancels; a node takes gestures
   only by saying so.

Order: E17a, E17c, E17b (the last widens the accessibility model most). Screens: the settings
screen in Arabic; an image viewer pinched, turned and panned; `--narrate` on the Data views page
reading an item's position among them all.

E17a as built on `vg` (2026-10-19): popups place themselves on logical sides (`InlineEnd`,
`InlineStart`) and a dropdown's start is the inline start, resolved in `PlacePopup` against the
anchor's direction (else the popup's own, its owner's); a tooltip takes its provider's (7f7d1317f).
`MirroredX` and `MirroredIn` place a widget's own parts from the inline start: a menu's gutter,
label, shortcut and submenu arrow, a combo box's arrow (51ac69a7f); a progress fill, a horizontal
scrollbar's thumb and its presses, a switch's knob, a closed chevron, a number field's arrows and
the room its text keeps clear of them (a new `EditBounds` of the text editors, not a physical
padding, in code or in the theme) (b1b25815d); a tree row's indent, a window's grip at its bottom
left pulling its left edge, and a tab drag's passing, drop place and mark (24ef0aaf5).
`Node::InlineStep` says which way an arrow goes along the line, for the tree, an exclusive group, a
grid, a submenu (its `menu.left` and `menu.right` read by direction) and the tab strip (2486b1fba).
A scroller across a right-to-left flow keeps CSS's scroll positions, `GetScrollMin` and
`GetScrollMax` bounding every clamp; its end, the wheel on a strip and the thumb follow; a table's
rows, columns, header, sort mark and grips and a grid's cells run from the right (d01a28483).
Content wider than a right-to-left box overflows toward its inline end, the left (b55044554).
`GUISandbox --mirror-audit` (the GUI.Mirror test) lays every page out both ways and checks each
widget's box within its parent; it found the pages' sheets padding and pinning by physical sides
where they meant the inline start, now logical; 748 widgets, none out of place; the sweep runs in
Arabic too, `GUI.Sweep.<backend>.ar` (6696b6a58). The Arabic Language page's reference moved a
pixel with the overflow rule, approved by the user (680026cca). Screen: the game menu, its
settings among it, speaks Arabic: every text a key, the script's through `gui::tr`, its families
falling back to DejaVu (561599c29).
Left: the audit skips docks (physical by design), popups at a point, item views' recycled rows
(the unit tests cover theirs) and the Stress and Motion pages; transforms stay physical, as CSS's
do; the Arabic is unchecked by a speaker; scripts see negative x offsets in right to left, as the
ruling has it. A text field laid its one line out from its left whatever its direction and
`text-align`, which never reached its text: it now places a short line by its alignment against
the line's direction and scrolls a long one from its inline start, and `text-align` from a sheet
reaches it (the user's catch on the Arabic settings screen).

E17c as built on `vg` (2026-10-19): the click, tap, pan, drag and fling thresholds are lengths in
DIPs, scaled by the display (c85e2b44c). The dispatcher follows every finger down (817b04479).
`touch-action` (auto, none, manipulation, or any of pan-x, pan-y and pinch-zoom) is a reflected
property with its css name, as `cursor` and `pointer-events` are, narrowed by every ancestor; a
one-finger move along an axis a node keeps goes on as the pointer instead of panning; two fingers
over a node that leaves pinch-zoom out make one gesture from the second's landing, the first's
press cancelled: `gesturestart`, `gesturechange` (scale, rotation and translation, totals and
deltas, about the fingers' middle), `gestureend`, `gesturecancel`; scripts read them from
`GuiEvent`, and the tester has `Pinch`, `Rotate` and `TwoFingerPan` (0d85399e2). The desktop shell
passes a cancelled finger and a trackpad's pinch, and no longer takes a trackpad's fingers for
touches (1dca6412f); the bridge forwards both, a pinch as the mouse's gesture at the pointer, and
its polled path follows every finger (1b8b58d8f). A gesture's translation is in the GUI's units,
not its node's, which its gesture may turn (7935feb6e). Screen: the Input page's picture viewer,
and two fingers' steps in the sandbox's `--keys` (8245f3c6c).
Left: a third finger is ignored; per-pointer capture for the second finger is not modelled; a
trackpad's rotation and two-finger scroll come as the platform gives them (SDL has no rotation);
the trackpad filter could not be tested without a trackpad device.

E17b as built on `vg` (2026-10-19): an AccessNode says an item's place in its set and the set's
size, a tree item's level, a table's rows and columns and a row's index, and that an item is off
screen; `--narrate` reads them; an id's row in RowIds is a lookup (39cb8a569). An item view's items
are nodes of their own with no widget behind them: the rows made and a page each side, the current
item and the first fifty selected, in item order, under ids that follow the item (a bit, the view's
key, the item's key); roles listitem, treeitem and a new row; the focus is the current item; focus,
activate, scroll-into-view, expand and collapse run through the view; a view sends again only the
items whose nodes changed (677440a84). Screen: `--narrate` on the Data views page reads "Row 3,
listitem, 4 of 500, selected", and End reaches "500 of 500".
Left: a table's cells are not nodes (a row is read by its first column); items without nodes are
counted, not listed, which a pull adapter (UIA, AT-SPI) would list on demand, eepp's way (host work,
ruling 3); the narrator speaks a root not on screen (the game menu's, at start).

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
