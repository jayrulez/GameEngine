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

## The GUI: what its stability campaign and its phases left (reframed 2026-10-28)

The campaign of 2026-10-17 (stability before the GUI could replace foundation.ui) is past its
decision: the editor and every sample are on the GUI, Engine.UI is deleted and foundation.ui is
in `Code/Deprecated`. Its steps done: the user's pass over every sandbox page, `--sweep` over every
page on both backends and in Arabic (GUISandbox's CMakeLists), the five fuzz targets
(GUI.Fuzz.Tests), real screens (the editor, the six samples), and the toolkit and gamekit. Checked
against the code on 2026-10-28; what is still open:

- **Stability.** The full lanes (gcc, clang-shared, wasm, ASAN) kept green: last all run
  2026-10-24. A soak run in RelWithDebInfo (`--soak` exists, none recorded; resident memory reads 0
  off Linux, backlog-0.1). The input fuzzer's streams are long, and a failure capture saves no
  screenshot.
- **Text.** Full UAX #29 (Hangul jamo, Indic conjuncts; common ranges only, by ruling) and words
  from the shaper (a character-class scan, TextSegmentation.cppm); `unicode-bidi: plaintext` by
  name (the behaviour is `direction: auto`'s); GUI and GUI.Hosting install no shaper of their own,
  so a new host must install HarfBuzz; a bold or italic run falls back to regular faces, no generic
  family maps to a bundled font, and a font with no shaper draws its fallback stretches in the
  run's own face.
- **Accessibility.** The platform adapters (UI Automation, NSAccessibility, AT-SPI) and real speech
  are host work (ruled out of scope for now); a table's cells are no nodes (a row reads as its
  first column); items without nodes are counted, not listed; the narrator speaks a root that is
  off screen.
- **Layout.** A container's baseline is its content box's bottom (only text widgets give one); grid
  and stack place `baseline` as start; text is not snapped to the pixel grid (boxes are); a node
  scrolling on both axes sizes `fr` columns to their content.
- **Keyboard.** Asking for text input over a list would raise an on-screen keyboard on touch;
  Ctrl+Tab from a Tab-capturing widget inside a tab view switches tabs before moving focus.
- **Styling.** Parts take no motion and no text properties (ApplyPartStyle); `inherit` takes the
  parent's resolved style, not its live value; attribute selectors read id, class and reflected
  properties only; a reflected property set in code before the first styling loses to the sheet;
  `@icon` is not read; a background with no `background-size` fills its box rather than its
  natural size; `@import` resolves from the data root, not the importing sheet; the token list is
  not generated into a document, nor shown beside a part in the inspector.
- **Motion.** A `transform: scale()` in a keyframe animation showed no change on a label in the
  engine's GUI (Masquerade's clock, 2026-10-28), where an opacity animation on the same label ran:
  not yet reduced to a test (whether the scale is not animated or not drawn); a sheet property's
  tween shows only after a StyleManager pass; groups do not
  ping-pong; a spring retargeted across shadows, transforms or gradients keeps its speed in
  progress, not the value's units; a node's own scale is not reflected.
- **Overlays.** Modality is per dispatcher, not per root (GuiHost has none); a tooltip's content is
  its text alone. Popups stay inside their root and the menu bar opens on release (no
  press-drag-release), both by ruling (editor-gui.md ruling 8, the 2026-10-19 ruling).
- **Controls.** The range is a base class, so a progress bar carries a page it hides; a tree resets
  on any change to its model, a moved or dropped node included (TreeModel::MoveNode).
- **Drag and drop.** A drag crosses the roots and windows of one GUI, not between GUIs or out to the
  OS; a script accepts a drop by taking it, never choosing the effect or refusing; the scroll band
  and hover delay are dispatcher settings, not styles; an OS file shows nothing while dragged over;
  a copy of a tree's own nodes is not taken; whether the pad's d-pad moves focus in a keyboard drag
  inside a list is untested (StepDirection does not ask the drop again).
- **Rendering.** No host presents part of a frame (damage is unused); a node's drawing is
  tessellated again every frame it draws; the stress page's 3000 animating out-of-view children
  still have their reach read each frame; the GPU-and-wait time growing with load is unexplained.
- **Scripts.** `gui.trCount` ignores a script's format arguments.
- **Direction.** The mirror audit skips docks, point popups, recycled rows and the Stress and Motion
  pages; transforms stay physical; no Arabic speaker has checked the Arabic.
- **Touch.** A third finger is ignored; the second finger has no capture of its own; no trackpad
  rotation.
- **Docking.** A float's whole surface cannot be dragged back into the dock; Shift-drag over a
  float's window asks for no float; a restored layout's floats are not kept on screen; Move to is a
  list of places, not a map.
- **Web.** The engine's GUI has never run in a browser (wasm builds it).

## A CPU rasterizer for VG (user 2026-10-28)

VG already does all but the last step on the CPU: an SVG parses into VG paths (VG.SVG), and fills
and strokes tessellate into triangles that carry their colour and an anti-aliasing coverage
(Tessellation.cppm). A CPU backend would fill a batch's triangles into an RGBA buffer instead of
the GPU. Two sizes (estimates): solid fills, normal blending and the scissor clip, enough for the
logos (a few hundred lines); or a twin of VG.Renderer (textures, distance-field text, radial and
conic gradients, the box shadow, blur, layers, stencil clipping, the blend modes and spaces;
1,500 to 2,500 lines, the GPU renderer and its shaders being about 2,800). The first is the
second's first step.

Its uses: the GUI's reference images and sweeps running on CI with no GPU (they skip with 77
today), against references of their own; the splash, the window icons and export art (a Windows
`.ico`, Steam artwork) rendered from one SVG instead of PNGs at each size; vector thumbnails in a
host with no GPU. The splash took a 2x PNG instead for now (scripts/logo-splash.py).

## The GUI lab's inspector as MCP tools: a phase of its own (user 2026-10-17)

The lab's inspector (`Specs/gui-lab-evolution.md`, E8e) answers its questions through a C++ API
over the core (`foundation.gui.inspector`: the tree, a node described with its states, box,
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

## A sound cue's thumbnail: its variants' waveforms stacked (user 2026-10-26)

A sound cue shows only its type icon in the assets browser and the pickers; no editor ever drew it
a thumbnail (the one audio thumbnail, Editor.Audio's AudioClipThumbnailGenerator, takes clips
alone). A cue's thumbnail would draw each variant's clip as a band of its waveform, one under the
other, so cues tell apart at a glance. The clip generator's pattern: read each variant's clip from
its source (the audio pages' `ReadAuditionClip`), its peaks into a band. Decide: an empty variant's
band, whether the weights show (a band's height or shade), and refreshing when a variant's clip
changes (the thumbnail's recipe hashing the clips' sources).

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
