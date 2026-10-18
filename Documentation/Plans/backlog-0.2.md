# 0.2 backlog - open items for after the 0.1 release

> Work decided for 0.2, not to be looked at for 0.1. Started 2026-10-10 with items moved from
> `weekly_backlog.md`; `backlog-0.1.md` holds the release's work. Pull an item out of here into
> the current weekly when it is started; delete it here when done.

## A material / shader node graph (user 2026-10-12; moved from 0.1)

The tier-2 half of the 0.1 custom-shader item: visual nodes (texture sample, math, blend, ... to a
surface output) that GENERATE a surface shader, fed through the same cook (variants, entry
points, bytecode packs), on the NodeGraph UI and the AnimationGraphPage pattern. It builds on
0.1's surface shaders (`Specs/custom-shaders.md`): a graph is a surface function written for you.

## An orthographic view that sees behind its eye: revisit (user 2026-10-13; after 0.1)

For 0.1 an orthographic editor view draws from one view height behind its eye (`EditorCamera::
DrawPullBack`), so a view tipped down shows the ground below the eye, with the shadow reach
lengthened to match (`ViewCamera::pullBack`, `ShadowReach`); what stands further behind stays
clipped. The user was unsure between that and a view volume that starts behind the eye (a
negative near plane, which games' orthographic cameras could use too), and asked to revisit it.
A negative near meets every pass that takes a view depth to be positive: the cluster slices'
`Log(far / near)`, the cascade splits' logarithmic start (`Max(nearZ, 0.01f)`), TAA's
disocclusion ratio, the depth linearizations. What other engines do (surveyed 2026-10-13, on
their current code):
- ezEngine: a negative near. Its editor's axis-aligned orthographic views draw -1000..+1000 about
  the eye (`ViewRenderSettings.cpp`), and focusing puts the eye at the selection's centre. The
  cost shows: clustered lights have a todo for orthographic and directional shadows are off in
  those views; its depth linearization is perspective-only. Its directional-shadow cameras use
  both: the render camera pulled back with near 0, the culling camera with a negative near.
- Traktor: a negative near. Every orthographic view is -far..+far about the eye (`WorldRender
  View::setOrthogonal`, the near ignored), editor axis views only. Its cluster slices take
  `log(far / near)` of that negative near and its cascades start at a positive near, with no
  orthographic branch in either.
- Doriax: a negative near by default (game cameras -10..10; its 2D editor view -10000..10000),
  shadows forced to one cascade fitted to the whole volume; no 3D orthographic editor view.
- LunarSong, Zero: a positive near; the eye stands a distance back from a pivot (Zero's
  LookDistance, LunarSong's orbit distance doubling as the view height), so the pull-back is a
  side effect of placing the eye, not a fix. LunarSong's near extraction turns a near of 0 or
  less into 0.1, and Zero clamps a camera's near to 0.01.
- Lumix: our old behaviour, near 0.1 from the perspective eye, tilted views allowed, nothing
  compensating.
None makes its renderer correct behind the eye: those with a negative near switch off or leave
broken what assumes a positive one. Doing it properly here means each of those passes, on every
backend, with a test of each; the payoff is games' orthographic cameras seeing behind themselves.

## The profiler: a live connection to a running player (user 2026-10-13; after 0.1)

0.1's profiler (`Specs/profiler.md`) profiles an exported game through capture files (a key or
`--profile-capture` in the player, opened in Perfetto). Add a live view: the editor connects to
a running player (a desktop player, the Steam Deck, a phone) over the network and its Profiler
panel shows that player's frames as it shows the editor's, as Traktor's target connection and
ezInspector do. The capture store is UI-free already; this is a transport (the engine's reliable
UDP), a player-side server armed by a flag, and the panel choosing its source.

## The GUI lab after E9: stability first, then the decision (user 2026-10-17)

The lab's evolution (`Specs/gui-lab-evolution.md`) is built through E9. Before anyone thinks about
it replacing foundation.ui, it has to be shown as stable as foundation.ui is (the user,
2026-10-17: "a lot of testing"). In order:

1. **Stability.** The user's visual pass over every sandbox page (most are unchecked: Text,
   Layout, Styling, Effects, Motion, Data binding, Navigation, Language and direction, the game
   menu, and the desk fixes' re-checks); the full lanes (gcc, clang-shared, wasm, ASAN) green on
   the lab and kept so; `--sweep` over every page on both backends; the markup and CSS parsers
   and the input dispatcher fed malformed and random input; long soak runs watching memory and
   frame time (in RelWithDebInfo); and a real screen or two built on it (a game's menus and HUD)
   to find what the sandbox does not. Bugs found go in as they come.
2. **The decision.** Only then: whether the lab replaces foundation.ui's core (the editor and the
   samples moving over) or pieces move across one at a time (the spec's "end state").
3. **Then** the toolkit and gamekit on whichever core that leaves.

Gaps the phases left, by area (each phase's own notes say more):

- **Text.** A font fallback list in the lab's text (the Language page draws everything in DejaVu
  Sans Mono because Roboto has no Arabic); `direction: auto` / `unicode-bidi: plaintext` for typed
  text; word moves, deletes and Home and End logical in mixed-direction text, and a selection
  collapsing to its logical end; a right-to-left line's clamp ellipsis on its right; full UAX #29
  (Hangul jamo, Indic conjuncts) and words from the shaper; a password taking no composition; a
  link with no pointer cursor; HarfBuzz as the lab's default shaper outside the sandbox.
- **Direction.** A right-to-left horizontal scroller starts at its left, not its inline start.
- **Accessibility.** Item views expose only the rows they have made, not every item (rows need
  a model of their own); the platform adapters (UI Automation, NSAccessibility, AT-SPI) and real
  text to speech are host work.
- **Layout.** A container's baseline is its content box's bottom, not its first child's; grid and
  stack place `baseline` as start; text is not snapped to the pixel grid; `z-index` is parsed but
  orders neither drawing nor hit testing; no `calc()`; a node scrolling on both axes gives `fr`
  columns their content's width.
- **Styling.** Parts take no motion and no text properties; `inherit` on a non-inherited property
  takes the parent's cascaded value, not its live one; attribute selectors read reflected
  properties only; a reflected property code set before the sheet first styled the node loses
  to the sheet.
- **Overlays.** No OS popup windows (a popup never leaves its root), modality per root;
  type-ahead by first letter only; a shrunk menu clips rather than scrolls; tooltips for the
  pointer only, never interactive; no Alt mnemonics; scripts cannot build a menu.
- **Controls.** The range is a base class (a progress bar carries a page it hides); every radio
  and every tab is a tab stop; type-ahead reads keys, not text input; a tree resets on every
  model range.
- **Drag and drop.** One drag within one root (none between windows); a script answers a query
  by taking it; the scroll band and hover delay are dispatcher settings, not styles; an OS file
  shows nothing while dragged over; the pad's d-pad moves focus during a keyboard drag inside a
  list; a tree takes a drop as a reset; a copy of a tree's own nodes is not taken.
- **Events and the rest.** Escape closing a popup is the dispatcher's special case; click
  thresholds are device pixels, not scaled; the stress page's turning boxes keep drawing out of
  view (a rotation about the centre could be bounded).

## The GUI lab's inspector as MCP tools: a phase of its own (user 2026-10-17)

The lab's inspector (`Specs/gui-lab-evolution.md`, E8e) answers its questions through a C++ API
over the core (`experimental.gui.inspector`: the tree, a node described with its states, box,
reflected properties, commands and listeners, a selector query, a property set as markup sets
it, and `StyleManager::ExplainStyle` for where each style came from). Serve the same answers as
MCP tools, as eepp's server and LunarSong's tools do, so an agent can read and drive a running
GUI. Not before the lab has a place in the foundation or the engine (the user, 2026-10-17: until
then nothing hosts a GUI an agent would ask about); then a standalone phase: the tools, their
place in `McpGuide.md` and the engine's tool count, and a session that finds a node, explains its
styles and edits it.

## The shader page: the cooked code, and project include files (2026-10-12; after 0.1)

What custom shaders left after 0.1 (`Specs/custom-shaders.md`, phase 8): a toggle on the shader
page showing the cooked SPIR-V or WGSL of a chosen variant (the parity doc's "live disassembly",
from the cook's own output), and a project's own `.hlsli` include files with their dependency
tracking (today `#include` reaches the engine's shader library only).

## Android: a native player and its export (user 2026-10-11)

Games run on phones today through the web export (touch drives the game UI, touch controls with
their guide, 0.1). A native Android player is the 0.2 step: a game shipped as an app, on Vulkan,
without a browser's limits. Moved here from `weekly_backlog.md` ("Android: shell backend +
touch/input + build"). The pieces:

- **Shell.** The desktop shell is SDL3, which runs on Android: its window, lifecycle (pause,
  resume, a lost surface on going to the background), touch (already normalized, with `pointerX/Y`,
  the contract the web and desktop shells keep) and the back button.
- **Rendering.** Vulkan (the RHI's backend); the surface's `currentTransform` drives the present
  transform (`Specs/present-transform.md` section "Native, later": `preTransform = currentTransform`,
  the extent swapped, the content turned), so the compositor does no rotation work.
- **Build and export.** An Android export template (NDK, Gradle, an APK or AAB), the player and its
  sidecars as the desktop and web templates are; an export preset (Android) with its icon, its
  orientation (the spec's `ScreenOrientation`) and signing; content in the APK's assets read through
  the VFS.
- **Saves and files.** The user data directory on Android's app storage.
- **Not in it.** The editor stays desktop; on-device MCP and play-in-editor are out.

Check against a real device (the user's phone), as the web demos were: touch, rotation, pause and
resume, the back button.

## Seeded: mountable content directories (user 2026-08-29)

Origin: reviewing Sedulous's VFS while building the editor's `data://` discovery+mount. Sedulous lets
a project MOUNT MULTIPLE content directories. The ask: allow an editor project to mount additional
content dirs beyond its own Sources/ - so authored content can be packaged (zip a data dir), shipped
to someone else, and they DOWNLOAD + MOUNT it into their editor project (asset packs, shared prefabs,
sample content). Builds on foundation.vfs (schemes/mounts) + the content DB.

Sketch: a project references N mounted content roots (its own + external packs); the content DB and
asset browser union them; GUID stability across packs (a pack's assets keep their guids so references
resolve after mounting). Decide: mount persistence (in project settings), conflict handling (same
guid in two packs), whether external mounts are read-only, and product/cook handling for mounted
sources. Relates to the `data://` discovery work + [[vfs-and-resource-stack]].
