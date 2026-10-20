# GUI lab evolution, round three: the satellite libraries

Round two (`gui-lab-evolution-2.md`, E10 to E17) left the lab's core with the text, keyboard,
styling, motion and rendering a game's screens need; what it still lacks is around the core, not in
it. foundation.ui has a ring of satellite libraries (a host, a viewport, docking, a toolkit, a
gamekit, cooked resources and their pipeline, the engine's hosting) and the lab has four of its own
(Shell, VFS, the inspector, the testing helpers) and a host written by hand into its sandbox. This
round gives the lab its satellites, sliced where foundation.ui's slicing was wrong, and builds the
first three: hosting, the viewport, then docking.

## Rulings (user, 2026-10-20)

1. **The libraries now, except the engine's.** Every satellite below exists as a library from the
   start, empty until its phase; the engine's GUI hosting (a scene component, the script facade)
   waits, as round two's ruling 1 has it.
2. **Hosting, not runtime.** foundation.ui's `UI.Runtime` is a host: windows, roots, renderers,
   input. "Runtime" also means something else in this engine (`Foundation/Runtime`). The lab's is
   `GUI.Hosting`, and the picture baker (`GUI.Bake`, round two's last work) is part of it: it needs
   the device and the renderers a host owns, and every real host wants it.
3. **Docking, not application, and all of docking.** foundation.ui splits docking in two: the dock
   manager, splits, zones and floating panels in its toolkit, the floating panels as OS windows in
   `UI.Application`. The lab's `GUI.Docking` is the whole feature, on top of hosting for its OS
   windows; the toolkit knows nothing of windows or docking.
4. **A satellite's theme is a sheet.** foundation.ui needs `IThemeExtension` (C++ injecting rules
   by type, the toolkit's importing every control). A lab theme is a sheet of tokens, so a
   satellite ships a CSS file of its own written against them (`var(--dock-tab-background,
   var(--control))`), following dark, light and contrast with no code; a theme restyles it by
   setting its tokens. An application such as the editor has one sheet over all of it, setting
   tokens and overriding where it wants, over the satellites' default sheets, not a fork of them.
5. **The order:** hosting and the viewport, then docking, whose design comes from a survey of the
   reference UIs first. The sandbox moves onto hosting, and shows the viewport and docking when
   each is ready.
6. **One splitter, in the core.** Docking's splits and every other split (a scene editor's three
   panes, a tool's, a game's) are one `Splitter` widget in `experimental.gui`, not a dock splitter
   beside a toolkit one: what a splitter must get right (minimums from content, weights that survive
   a pane coming or going, keyboard, accessibility, right to left) is fixed once. foundation.ui has
   two (`DockSplit`, the toolkit's `SplitView`), and they drifted. It is in the core, not the
   toolkit, so docking needs no toolkit and a game needs no editor widgets to split a window; what
   docking adds (weights from its layout, drop zones, previews) is the DockSpace's, around it.

## The libraries

| Library | Module | What it is | foundation.ui's | State |
|---|---|---|---|---|
| GUI | `experimental.gui` | The core: nodes, layout, styling, text, input, widgets, scripting | UI | Rounds one and two |
| GUI.Shell | `experimental.gui.shell` | The platform input bridge | UI.Shell | Built |
| GUI.VFS | `experimental.gui.vfs` | A resource provider on the VFS | UI.VFS | Built |
| GUI.Inspector | `experimental.gui.inspector` | The inspector (a tool) | none | Built |
| GUI.Testing | `experimental.gui.testing` | Test helpers, image comparison (a tool) | none | Built |
| GUI.Hosting | `experimental.gui.hosting` | The host on `foundation.graphics`: one GUI, a root per window, each window's VG context, renderer, targets and input; the frame; offscreen pictures; the picture baker | UI.Runtime | Built (H1 to H3) |
| GUI.Viewport | `experimental.gui.viewport` | A widget showing 3D (or any GPU) content drawn into a target of its own, with input gated to it | UI.Viewport | Built (V1, V2) |
| GUI.Docking | `experimental.gui.docking` | Docking: splits, tab groups, drop zones, floating panels in the window and as OS windows, layouts saved and restored; its default sheet | UI.Toolkit's docking and UI.Application | Designed (D0 to D7) |
| GUI.Toolkit | `experimental.gui.toolkit` | Editor widgets: the property grid, colour pickers, curves, node graphs, tool and status bars | UI.Toolkit less docking | Empty (round two's ruling 4) |
| GUI.Gamekit | `experimental.gui.gamekit` | A game's screens: a screen stack, input modes, transitions, prompts, toasts | UI.Gamekit | Empty |
| GUI.Resources | `experimental.gui.resources` | Cooked GUI documents and themes, framework free | UI.Resource | Empty |
| GUI.Pipeline | `experimental.gui.pipeline` | Cooking and validating them (tools only) | UI.Pipeline | Empty |
| (the engine's) | | A scene's GUI, the script facade | Engine.UI, Engine.UI.Script | Not this round (ruling 1) |

## The phases

### H1 The host

`GuiHost` draws the lab on the runtime's multi-window graphics host (`foundation.graphics`), as
foundation.ui's `UIHost` does: constructed with the allocator, the graphics device, the shell, the
font service and the data mount; it compiles the VG shaders once, owns one `GUIContext` and its
style manager, and attaches a `SceneRoot` per `RenderWindow` with that window's VG context, VG
renderer, pass targets (multisampling and the stencil the fills use) and input surface through the
lab's shell bridge, stashed on the window as its payload. `Update(dt)` routes each window's input
to its root, runs the GUI's clock, styles and lays out; `RenderWindow(frame)` draws a window's root
when it needs a frame (a skipped frame costs nothing), records the picture baker, then the layers
and the pass. The baker joins it, serving every window's renderer (a bake registered with each).

Tests: on the Null backend, a host attaches and detaches windows, a root's frame is drawn only when
it needs one, the baker registers its bakes with every window's renderer, and a detached window's
renderer forgets them.

As built on `vg` (2026-10-20): `GuiHost` and `GuiWindowData` (the window's payload), `VgShaderSet`
and `VgShaderLibrary` (the set resolved from a data mount; a test hands its own), and the baker
moved in from `GUI.Bake`, which is gone, serving several renderers (`AddTarget`, `RemoveTarget`).
Input: the pointer to the window under it or the one a held press began in (the lab has no capture
to ask), keys, text and the pad to the focused window, drops to the window they landed on. The
null shell injects no input, so routing is checked by the sandbox once it runs on the host (H3).

### H2 Pictures offscreen

A root drawn into an image at a given size on the host's device, read back: what the sandbox's
image tests, `--bench`, the sweep and the mirror audit draw with today by hand. Tests: a root drawn
offscreen matches the same root drawn on screen; the bench's stages are timed.

As built on `vg` (2026-10-20): `GuiOffscreen`, a target of a size and format of its own with a
renderer and pass targets of its own (the bakes registered with it, as with a window's), drawn on an
encoder and fence of its own and waited for, the picture read back in the call through
`foundation.rhi.capture`; each draw times the GUI's draw, the renderer's prepare and the GPU. The
host's `StyleAndLayOut` styles and lays out a root as a window's is. The "matches on screen" test
is the sandbox's image tests, which draw this way once the sandbox is on the host (H3).

### H3 The sandbox on hosting

GUISandbox becomes an application on the runtime host (`runtime::RunApplication`), its window, its
GUI and its frame the host's: its pages, the inspector pane, the image tests, the bench, the sweep,
the soak and `--keys` unchanged in what they do and show. Screen: every sandbox page's reference
image unchanged.

As built on `vg` (2026-10-20): GUISandbox is a `runtime::IApplication` run by `RunApplication`,
as foundation.ui's UISandbox is; its window, input and frames are the host's. On the way the host
took panes (several roots in one window, each over a region with its own input, for the inspector's
pane, which is another GUI with its own styles), a pointer filter (the inspector picking, the pane's
boundary, a `--keys` script), an overlay (the inspector's highlights over the page) and a drop
observer. The image tests and the bench draw through `GuiOffscreen` and keep the window out of it
(their fixed clock is the sandbox's own); F12 and `--screenshot` record on the frame's encoder. Every
reference image is unchanged, and an idle page draws one frame in 151 (the runtime host presents
every frame; the GUI's batch is encoded again, not redrawn). The sandbox blends in linear light as
its references were drawn; the host's default is encoded, as foundation.ui's is.

### V1 The viewport

`Viewport`, a widget that shows content drawn into a colour target (and depth) of its own, sized
to the widget in device pixels: an `OnRender` the application fills, recorded before the window's
pass, the target registered with the window's renderer as an external texture, so the widget draws
it as an image. Its input is gated: a shell input surface over the widget's area, so a 3D camera
reads the keyboard and mouse only while the widget is hovered or focused, and a drag that leaves it
keeps going. It follows the window it is in (a widget moved to another window's root re-registers).

As built on `vg` (2026-10-20): `Viewport`, a widget, is hosted content (`IHostedContent`, a new
seam in GUI.Hosting): handed to the host once (`HostIn`), the host binds it to the renderer of the
window its widget is in and binds it again when it moves to another window (foundation.ui's
application had to call `AttachToWindow` itself), sizes its targets to the widget in that window's
device pixels, places its input surface over it (on a router of the host's own, so the GUI keeps
the pointer for hover and focus) and records its drawing on that window's frame before the GUI
samples it; with no `OnRender` it clears, hidden it records nothing. Not carried from
foundation.ui: a game-in-a-panel's keyboard capture and hosted text input, until an embedded GUI
needs them.

### V2 The sandbox's viewport

A Viewport page: a spinning cube a camera flies around, the page's controls over it.

As built on `vg` (2026-10-20): the sandbox's Viewport page, a spinning cube (the samples' shared
`SpinningCube`) a fly camera moves through from the widget's gated input, a note over the scene,
a camera reset and a letterbox at a fixed 16:9. Its image reference shows the widget unbound: the
image tests draw offscreen, where no window binds it.

### D Docking (approved by the user, 2026-10-20)

The survey (2026-10-20) read Godot's docks, the Zero editor, ezEngine with Qt-ADS, Dear ImGui's
dock nodes through Lumix, traktor, zed and gpui, LunarSong, eepp, and foundation.ui's own port.
Findings that decide the design:

- foundation.ui's port makes the live widgets the layout tree, so nothing can be tested or previewed
  without them; its floating windows hold one panel each, cannot be docked into, and are not saved;
  panels a saved layout does not place float in a cascade; its drop zones are hard-coded squares
  drawn outside the styled tree; one 50 px minimum for every split (a squeezed console asserted);
  and its OS-window floats chase the cursor every frame, which Wayland punishes.
- The best of the others: a layout as plain data with tested operations (LunarSong, ImGui), n-ary
  splits whose weights survive a removal (zed resets them to equal, a fault), every floating window a
  whole dock tree (ADS, LunarSong), a central document area that stays when empty (ImGui), panels
  known by a registered id never by title (Godot and ez break on renames), a panel that registers late
  filling its saved place (zed, ImGui), a drop preview that is the real result (ImGui's future node,
  where zed and our port draw fixed fractions), the strongest keyboard (zed's directional moves) and a
  no-drag route (Godot's "Move to" map). traktor's docking, untested, rotted into dead code.

The design:

1. **A layout is data.** `DockLayout`: surfaces (the main window's and each floating one's, with its
   bounds, screen and scale), a flat table of nodes by stable id (`Split{Axis, Children, Weights}`,
   `Stack{Panels, Active, Flags}`, one stack may be the central one), and each hidden panel's last
   place. Operations (insert as a tab or beside a stack, move, float, close, show at its last place,
   resize, maximize) are functions on it that refuse with a reason, never fall back silently, and
   normalize after (empty stacks go, single children collapse, same-axis splits merge, weights sum
   to one). Tested headless, fuzzed by random sequences that keep its invariants.
2. **Panels by id.** A registry of panel descriptions (id, a title key, icon, a factory, a close
   policy: hide, rebuild or destroy, a default place, the places it may go, which panels it may tab
   with, whether it may have several instances), registered in code, markup or script. The registry
   owns a panel's content, so moving it between windows or hiding it never destroys it.
3. **Widgets from the data.** A `DockSpace` widget shows one surface: splits become the core's
   `Splitter` (ruling 6; n children, minimums from each child's min-content so nothing is squeezed past what it can
   draw, keyboard resizing, a separator's accessibility role, right to left), stacks a `TabView` whose
   strip's group is `dock`, reconciled by node id without rebuilding what did not change.
4. **Drag to dock.** A tab torn from a strip carries the panel; a stack's own grip carries the stack.
   Over a strip it is a tab at an index; over a stack, its middle or an edge band (`--dock-edge`, a
   sheet's); over the surface's edges, a split of the whole; anywhere else, a float at the pointer.
   The preview is the layout the drop would make, laid out; refusals show their reason as the lab's
   drops do. The zones and the preview are parts of the styled tree, sized and coloured by tokens.
   The keyboard drag (`drag.pick`) works on them; `dock.*` commands move and focus by direction, float,
   re-dock, maximize and reset; a "Move to" menu shows a map of the places.
5. **Floating through the host.** Docking never opens an OS window itself: it asks the host (a
   surface host: open, close, move a surface; which surface is under the screen pointer). GuiHost
   opens a window with a root in the same GUI; a virtual host puts a floating surface in a lab
   `Window` in the main root, for the web and one-window platforms. A drag across windows is routed
   by the GUI to the root under the pointer; no window chases the pointer: the drag shows a preview,
   and the window is made or moved at the drop. On Wayland, where a client can neither place its
   windows nor know the global pointer, floats are decorated windows and docking between them goes
   by the keyboard and the "Move to" menu.
6. **Serializable data.** A layout is reflected data that the engine's serializer reads and writes,
   with a version and its migrations; docking adds a restore that checks it first (a bad layout is
   refused with its reason and the current one stays), panels it names that are not registered kept
   as placeholders that fill when they are, registered panels it does not name put at their default
   place. Docking reads and writes no files: where a layout is kept (the editor's, per project and
   user), when it is saved and where named presets live are the application's. Hosting knows nothing
   of it either; as the surface host it only answers where each floating window is and what screens
   there are, so a layout taken carries the floats' bounds and a layout restored clamps them back
   onto the screens there are.

The phases, each a layer with its tests: D0 the Splitter; D1 the layout and its operations; D2
saving and restoring; D3 the DockSpace, the registry and the panels' lifetimes; D4 drag to dock
within one window, the keyboard and the "Move to" menu; D5 floating surfaces in the window; D6
floating as OS windows through the host, and drags between windows; D7 presets and the tools an
agent drives it with. The sandbox's Docking page grows with them.

D0 as built on `vg` (2026-10-20): `Splitter` and `SplitterHandle` in the core. Panes by weight,
each held at its minimum (its sheet's, else its content's) and clipped when the minimums pass the
room; a handle between each two kept in step as panes come and go, read as a separator with where
it stands, moved by drag and by the arrow keys (Shift, Home, End), a double click giving the given
shares back; a resize changes the two panes beside the handle alone, a pane that goes gives its
share to its neighbours; vertical and right to left; `axis` and `weights` in markup and sheets; the
theme's `splitter-handle` quiet until hovered, dragged or focused. On the way: a node measured with
no room (min-content) lost a width its style gave it, because exactness was read from a minimum
equal to the maximum; the constraints now say it (2bb68576f).

D1 as built on `vg` (2026-10-20): `DockLayout` in GUI.Docking, plain data. Panels tab into a
stack, dock beside one (joining its split when the axis matches, else wrapping it) or along a whole
surface's edge, move, float into a surface of their own (docked into in turn), close keeping their
place and show again there, beside a neighbour from then when the stack is gone, else in the central
stack; a split's shares are set, a stack maximized, its flags (central, locked, no split, no tabs)
refuse what they forbid. Every operation refuses with its reason and changes nothing, or does what
it says and normalizes. Tested headless, with five seeded runs of 4000 random operations each
checked against the layout's invariants after every step.

Not in scope: the editor moving off foundation.ui's docking. It needs the toolkit too, which is not
written, and the lab's libraries proven stable first (the user, 2026-10-20).
