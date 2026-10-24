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
| GUI.Docking | `experimental.gui.docking` | Docking: splits, tab groups, drop zones, floating panels in the window and as OS windows, layouts saved and restored; its default sheet | UI.Toolkit's docking and UI.Application | Designed (D0 to D6) |
| GUI.Toolkit | `experimental.gui.toolkit` | Editor widgets: tool and status bars, the property grid, colour, curves, gradients, the timeline, node graphs, the breadcrumb bar | UI.Toolkit less docking and the code editor | Built (T0 to T7) |
| GUI.Code | `experimental.gui.code` | The code editor | UI.Toolkit's CodeEditView | Built (C0 to C8) |
| GUI.Gamekit | `experimental.gui.gamekit` | A game's screens: a screen stack, input modes, transitions, prompts (toasts are the core's, T7) | UI.Gamekit | Built (K0 to K7, 2026-10-23) |
| GUI.Resources | `experimental.gui.resources` | Cooked GUI documents and sheets, framework free | UI.Resource | Built (E2) |
| GUI.Pipeline | `experimental.gui.pipeline` | Cooking and validating them (tools only) | UI.Pipeline | Built (E1, E3) |
| Engine.GUI | `engine.gui` | The engine's GUI: screens, scene canvases, billboards, world panels, the script facade, the player and play-in-editor | Engine.UI, Engine.UI.Script | Designed (E4 to E11; awaiting approval; the user's direction 2026-10-23 lifts ruling 1) |

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
   `Splitter` (ruling 6; n children, minimums from each child's sheet alone, never its content, so
   only the user moves a separator and a panel that does not fit wraps or is clipped; keyboard resizing, a separator's accessibility role, right to left), stacks a `TabView` whose
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
floating as OS windows through the host, and drags between windows. The sandbox's Docking page
grows with them.

Presets and agent tools are the application's, not docking's (the user, 2026-10-20): the library
gives a layout as data to take and adopt, and an application such as the editor keeps its named
presets and offers its own tools (MCP's among them) over that. There is no D7.

D0 as built on `vg` (2026-10-20): `Splitter` and `SplitterHandle` in the core. Panes by weight,
each held at its minimum (its sheet's) and clipped when the minimums pass the room; a handle between each two kept in step as panes come and go, read as a separator with where
it stands, moved by drag and by the arrow keys (Shift, Home, End), a double click giving the given
shares back; a resize changes the two panes beside the handle alone, a pane that goes gives its
share to its neighbours; vertical and right to left; `axis` and `weights` in markup and sheets; the
theme's `splitter-handle` quiet until hovered, dragged or focused. On the way: a node measured with
no room (min-content) lost a width its style gave it, because exactness was read from a minimum
equal to the maximum; the constraints now say it (2bb68576f). Changed after the user tried the Docking page:
a pane's content no longer gives it a minimum (a tab shown with longer text pushed the separator
over; only the user moves one, and a dock stack clips what does not fit), and a dragged handle reads
the pointer in its splitter's space (it added its own place to a screen point, running ahead of
the pointer faster the further it went).

D1 as built on `vg` (2026-10-20): `DockLayout` in GUI.Docking, plain data. Panels tab into a
stack, dock beside one (joining its split when the axis matches, else wrapping it) or along a whole
surface's edge, move, float into a surface of their own (docked into in turn), close keeping their
place and show again there, beside a neighbour from then when the stack is gone, else in the central
stack; a split's shares are set, a stack maximized, its flags (central, locked, no split, no tabs)
refuse what they forbid. Every operation refuses with its reason and changes nothing, or does what
it says and normalizes. Tested headless, with five seeded runs of 4000 random operations each
checked against the layout's invariants after every step.

D2 as built on `vg` (2026-10-20): `DockLayoutDocument`, an `ISerializable` holding a layout (the
layout stays a value, copied for a preview), versioned (a payload of another version is refused);
it reads and writes no file. `DockLayout::Adopt` takes a restored layout only if it holds together,
refused with what does not and the current one unchanged; panels it names that are not registered
stay (placeholders), registered ones it does not name go to their `DockDefault` place, a floating
surface off every screen comes back onto the first. The invariants also refuse ids at or past the
layout's counters (a file edited by hand would make a new node collide).

D3 as built on `vg` (2026-10-20): `DockController` holds the layout and the registered panels
(title, icon, a content factory, a close policy, closable, a default place) and owns each panel's
content once made, so a panel moved or hidden keeps it. Closing follows the policy: Hide keeps the
content and the place, Rebuild lets the content go, Destroy lets both go (`DockLayout::Forget`).
Every change goes through the controller, which tells each `DockSpace` showing it to show it again;
what the user already did in the widgets (a separator dragged, a tab chosen or dragged within its
strip, a tab's close) is noted back. `DockSpace` shows a surface as core widgets: splits as
`Splitter`s and stacks as `TabView`s (group `dock`, reorderable), kept by node id as the layout
changes; a panel's content is made when its tab is first shown; a panel not registered shows a
placeholder that fills when it registers; a maximized stack is shown alone, the widgets of the
rest kept for its restore. On the way, in the core: a `TabView` reports a page moved; a tab bar no
longer selects its first tab while a selection asked for waits for its tab (it built content that
was not to be shown); a node destroyed leaves the children held elsewhere pointing at no parent
(a panel's content, kept by the controller, reached its freed page when it moved).

D4, the pointer, as built on `vg` (2026-10-20): a tab torn from a stack's strip carries its panel.
Over a strip it goes in at a tab's place, over a stack's middle into it, over a stack's edge band
(the sheet's `edge-band`, the theme's `--dock-edge`, a third of the stack at most) beside it, over
the surface's band (`surface-band`, `--dock-surface-edge`) along the whole edge, which wins over a
strip lying there. Each drop is the layout's move tried on a copy: refused, the layout's reason shows
and nothing moves; accepted, a `dock-preview` panel stands where the panel would be (the split
shares of the layout it would make; the separators' room is left out). The strip's drops reach the
layout through two hooks the core's tab bar gained: a tab offered may be refused or described, and a
dropped one taken over.

D4, the keyboard, as built on `vg` (2026-10-20): a tab of a strip in a group is picked up by
`drag.pick` (the core's tabs) and dropped where the focus is, on a strip's tab at its place or into a
stack (the bands are the pointer's aim; the keyboard has none). `DefineDockingCommands` defines
`dock.focus-*` and `dock.move-*` by direction (into the nearest stack that way, else along the
surface's edge, the focus going with the panel; left and right as the screen runs), `dock.maximize`
(and back), `dock.close` and `dock.move-to`, with keys. "Move to" is a menu, from a tab's context
menu or the command: into each other stack, beside one on each side, along the surface's edges,
each place the layout refuses shown disabled; a list of the places, not yet a map of them. A stack's
own grip came after D5 (below).

D4, the drag's feel, after the user tried it (2026-10-20): the drag showed only the payload's
label beside the pointer, and the torn tab sat in its strip as if nothing were dragged. Surveyed:
foundation.ui shows a tab chip and takes the tab out of its strip at once; Godot leaves it and shows
its icon and title; LunarSong dims it and shows a mock panel; Zero and Dear ImGui tear the panel out
into a floating window. Now a torn tab's ghost is the tab (its title, its size, held where it was
grabbed), and it dims in place meanwhile, or its strip closes up, a neighbour's panel shown
(`torn-tab`, the theme's `--dock-torn-tab`; dim the default, the user's choice after trying both); the layout still moves nothing until the drop, so
Escape shows the stack as it was (foundation.ui floats the panel instead). A stack's only tab, or a
tab picked up from the keyboard, dims either way. The floating-window feel waits for D5.
Then the drag's start: a tab first slid along its strip and tore out only 32 pixels past the
strip's top or bottom (never past its ends), 35 to 55 pixels from the press by where on the tab
it was, so the ghost and the gap came late and abruptly. In a strip of a group a tab is now a drag
past the dispatcher's threshold, any way, as foundation.ui's; a reorder within its strip is a drop
there at the mark (strips of no group keep the live slide), and the tear distance is gone.

D5 as built on `vg` (2026-10-20): floating surfaces in one window. The layout docks a floating
surface whole: its panels as a stack's tabs, or its tree beside a stack or along an edge, its splits
kept. `DockArea` is the virtual surface host: the main surface's DockSpace fills it, and each
floating surface is a `DockFloat` over it (a Window holding a DockSpace of its own), made, placed,
titled and ended with the layout; a window moved or resized writes its bounds back once let go, kept
where its title bar can be reached, and closing it closes its panels as their policies say. How a
float is asked for and what a float's title does were the user's choices: a tab dragged with Shift
held floats (Dear ImGui's rule: the dock targets pass the drop on, the area shows the window at the
pointer), as do "Move to"'s Float and `dock.float` (Ctrl+Alt+F); a float dragged by its title bar
follows the pointer itself and docks whole where it is let go over a target (Dear ImGui's and
Qt-ADS's), the preview the room its content would take; with Shift it only moves, and Escape puts it
back. On the way, in the core: a drag source may follow the pointer itself with no preview, a window
reports a move or resize the user made and its title bar can start a drag instead, and a tab bar's
owner may pass a tab offered on. The controller's pass over its views takes views made and ended as
it goes.

The dock keys across surfaces (2026-10-20). A float lies over the main surface, not beside it, so an
arrow alone cannot reach most floats; no surveyed UI crosses overlapping windows with arrows alone
(Visual Studio's Alt+F6, Windows' F6 and Ctrl+F6, Dear ImGui's Ctrl+Tab and i3's `focus
mode_toggle` cycle instead). The user's choice: `dock.next-surface` and `dock.previous-surface`
(Ctrl+Alt+\` and with Shift) take the focus around the surfaces (the main one, then the floats as
they were made) to the stack last left in each, bringing a float to the front; the arrows stay
within a surface, and past a float's edge go on to what lies just beyond it there (the topmost
other float, else the main surface), the move keys carrying the panel into that stack, before the
surface's own edge. "Move to" lists the other surfaces' stacks, named for where they are. A DockArea
is a DockSpace's `IDockSurroundings`. On the way, in the core: a menu item outlives an action that
ends its menu, and a window's title bar is the mirror of itself right to left.

Docking is the pointer's (the user, 2026-10-20): a drag, with modifiers (Shift to float, Shift on a
float's title to only move it). The `dock.*` commands stay defined but no key is bound to them, as
Ctrl+Alt and an arrow is GNOME's and Ctrl+Alt is AltGr on many layouts; the keyboard's way in is a
focused tab's context menu (Shift+F10, the Menu key), which is Move to and reaches every place, and
`drag.pick` (Ctrl+Shift+D). An application binds the commands where it wants keys.

Closing is the pointer's too: a closable panel's tab shows its close part when chosen or hovered (as
Visual Studio's and Godot's docks do), a middle click closes it, and the tab's context menu ends with
Close, disabled for a panel that cannot be closed. Unbinding `dock.close` had left a docked panel no
way to close.

A stack's grip (the user's choice, after Dear ImGui's, 2026-10-20): a dock strip's end always shows
its list button, which clicked lists the tabs and dragged carries the whole stack, to the same places
a tab goes (into a stack over its strip or its middle, beside one, along an edge; with Shift, a float
of the stack as it is), the stack dimmed meanwhile; past it a cross closes every closable panel of the
stack. Both are parts a sheet styles (`tablist`, `closeall`, the docking sheet's
`--dock-strip-button` tokens), and they need no room past the tabs, so a full strip keeps its grip.
In the layout a stack moves whole: its panels into another stack, the stack itself beside one, along
an edge or floated, the central stack's panels going while it stays. In the core, a tab strip's list
button may always show (`list-display`), and a `close-all` part asks to close every closable tab,
by place, as an owner may make its tabs again after each close.

D6, the plan (2026-10-20). What is there: GuiHost runs several OS windows, each with its roots, in
one GUI; the shell makes windows at a screen place, moves and sizes them, reads the global pointer
and says which window system it is. What is not: the screens (no display list, bounds or scale), a
window raised, a host that opens a secondary window or hears its close, a drag that leaves its root,
and the surface host. The layers, each with its tests:

1. The shell: the screens (bounds, usable bounds, scale, name) and a window raised.
2. Hosting: a secondary window opened and closed for a root, its moves, resizes and close requests
   reported, and the root under a screen point.
3. The core: a drag started in one root goes on over another root of the same GUI, its image and
   the drop's preview in the window the pointer is in; let go over no window, it ends "outside".
4. Docking's surface host: open, move, close and place a surface, and the screens. The floats in
   the window become the virtual host (the web, one-window platforms); an OS host gives each float a
   window and a root holding its dock space. A float's bounds are in screen units on its screen. A
   tab or stack let go outside every window floats there; with Shift, at the pointer, as now. No
   window follows the pointer: a preview shows, and the window is made or moved at the drop.
5. The sandbox: the Docking page switches between floats in the window and floats as OS windows.

The user's ruling (2026-10-20): an OS float has the platform's own frame on every system. Wayland
needs it anyway (a client neither places its windows nor sees the global pointer), so one way of
working, not two; the system moves, resizes and snaps the window; and a borderless window with the
lab's title bar would have to be moved by the lab, which this design rules out. The cost: a float is
not docked back by its OS title bar, but by its stack grips (a float of one stack is one grip) or Move
to; on Wayland docking between windows is Move to.

D6 as built on `vg` (2026-10-20). The shell lists the screens, raises a window, reports a moved one
(SDL never turned its event into the shell's), retitles a window, and says what its windows can do
(more than one; placed where the application puts them), the last four on `master`. The host opens
a window for a root through the application's opener, tells any number of listeners each window's
events by its root, and says which root is under a screen point, where its windows hold their
places. A drag goes on over another root of one GUI (the GUIContext's IRootPlaces, the host's), its
preview moving into that root's top layer, and one let go over no root ends outside with the screen
point; a tab strip hands its owner how a torn tab's drag ended. A DockArea takes an IDockWindows:
each float a dock space in a window of its own, its bounds on the screen, retitled and placed as the
layout has it, the user's moves and closes heard back; a tab or stack let go outside every window
floats there, Shift and Move to's Float place it on the screen, the surface cycle raises its window,
and a window that will not open takes the area back to floats in it. DockHostWindows is that on
GuiHost, and GUI.Docking links GUI.Hosting for it (the user agreed, 2026-10-20: docking is the whole
feature). The sandbox's Docking page switches between floats in the window and in OS windows. Not
yet: a float's whole surface docked back by a drag (its stack grips and Move to do it), a drag over
a float's window asking for a float with Shift, and screens clamping a restored layout's floats.

Not in scope: the editor moving off foundation.ui's docking. It needs the toolkit too, which is not
written, and the lab's libraries proven stable first (the user, 2026-10-20).

### T The toolkit (approved by the user, 2026-10-21)

What the editor uses of foundation.ui's toolkit (81 of its files, counted 2026-10-21): the property
grid and its editors (33 files), SplitView (19, the core's Splitter now), the draggable tree (9), the
code editor (8), tool bars built from the editor's action registry (5), the timeline (5), floating
panels (4), the curve canvas (3), a toast host, the gradient editor, the breadcrumb bar and the
status bar; the node graph only in the animation state machine. Its standalone colour pickers, HDR
picker and vector fields the editor does not use. The lab's core already has the item views (tree,
table, list), number fields, sliders, menus, tabs, expanders, windows, an undo stack, commands and a
keymap.

The survey (2026-10-21) read ezEngine, Godot, the Zero editor, Lumix, traktor, doriax, LunarSong and
zed for property grids, colour, curves and gradients, node graphs, and tool and status bars.
Findings that decide the design:

- **An edit is a gesture.** Begin, preview, then restore the original and commit once as one undo
  entry, or cancel (Zero, LunarSong, whose gesture cancels itself if dropped). Godot merges edits
  made within 800 ms of each other and Lumix every repeat edit of a property, even separate ones.
- **Editors chosen in layers**: the property's hint, then its type, then a fallback that says why
  nothing fits (ezEngine, Zero), not one switch over every type.
- **The selection behind one accessor**, so one object and several share code, with mixed values
  recorded per component (Zero, ezEngine). Neither foundation.ui nor the editor has mixed values.
- **Defaults from a stack of providers** (the type's default, then a prefab's), giving revert and a
  marker on a changed value (ezEngine).
- **State outside the rows**: fold state by property path, search over an index, virtualised rows
  (zed); the wheel scrolls and never changes a value (ezEngine).
- **Colour**: HDR as an intensity in exposure stops beside the colour (Godot, ezEngine, traktor),
  one undoable commit when a picker closes, after restoring the original (Godot). foundation.ui's
  HDR picker runs HSV on linear values. The engine's own rule (`Color`): an authored colour is
  sRGB, decoded to linear where it reaches the GPU, so the picker edits sRGB values; linear is a
  display, not the storage.
- **Curves**: the editor draws with the runtime's own evaluator (LunarSong); this engine has three
  copies of one cubic Hermite (Core's `Curve`, the particles' curves, foundation.ui's CurveCanvas).
  Several curves on one canvas, box select, framing, constrained drags, tangent modes (ezEngine); a
  gradient's interpolation space chosen (Godot), alpha stops apart from colour stops.
- **A node graph is a view over a model** with stable ids, its edits requests its owner applies
  (Godot, ezEngine); foundation.ui's copies the graph into the widget and maps indices. A connection
  asked for returns its cardinality and whether it replaces one, shown on pins while dragging
  (ezEngine); connections culled to the view (Godot, traktor); frames and comments.
- **One command registry** feeds menus, tool bars and shortcuts (ezEngine, Lumix, Zero); a tool
  bar's overflow is designed (zed shrinks and clips its left side); a status bar is registered items
  in left and right zones with timed messages; a tooltip shows the shortcut and why a command is
  disabled.

#### Rulings (user, 2026-10-21)

1. **The reflected source is the toolkit's.** The property grid reads a property source; the
   toolkit has a source over core reflection and its attributes, and the application supplies the
   writes and the undo (as ezEngine and Zero do). Today the editor's inspector maps reflection to
   rows in about 3,000 lines of its own.
2. **One curve, one gradient, in Core.** The runtime's types are the editors' model; the toolkit
   has no curve or gradient model of its own. On `master`, before T5:
   - `foundation.core` `:curve` gains `EvaluateCurve(Span<const CurveKey>, f32 time)`, the one
     Hermite, which `Curve::Evaluate` calls. `CurveKey` gains a tangent mode (auto, smooth,
     broken, flat) the editors keep, and `:curve` the function that sets tangents by their modes,
     so an editor and an importer agree. PropertyAnimation's resource reads and writes the mode
     (its version bumped).
   - A new `:gradient` partition: colour stops (time, sRGB `Color` without alpha), alpha stops
     (time, alpha), an interpolation (constant, sRGB values as today, linear light, OKLab), and
     `EvaluateGradient` over spans of each.
   - Particles keep their fixed eight keys (value types, reflected as bounded arrays), but of
     Core's: `ParticleCurveFloat` holds `CurveKey`s and evaluates with `EvaluateCurve`, so it gains
     constant and linear keys; `ParticleCurveColor` becomes
     a fixed gradient evaluated by `EvaluateGradient`. `HermiteFloat` goes.
   - `ParticleCurveFloat2` goes too. Its one user, size over lifetime, holds `size` (a
     `ParticleCurveFloat` setting both axes) and `separateAxes`, with which X follows `size` and
     Y a second curve, `sizeY`, of keys of its own. Every use in the samples (nine) sets X equal
     to Y at every key, so each becomes `size` alone; a uniform size is one curve to edit and one
     search per particle, and nothing needs linked key times (foundation.ui's CurveCanvas has
     `LinkedTime` for this type only).
   - The particle format's version is bumped; the samples' effects and the three authoring scripts
     that write them (PaperKid's `fx.py`, Lamplight's `effects.py`, Snowline's `particles.py`) are
     migrated, and the editor's particle page follows.
   - Not folded in: Animation's sampler (glTF cubic splines over vectors and quaternions, with
     glTF's tangent convention, imported rather than authored) and `Foundation/Spline` (paths in
     space). foundation.ui's CurveCanvas and GradientEditor keep their copies until the editor
     leaves foundation.ui.
3. **The code editor is a satellite of its own**, `GUI.Code`: about 4,000 lines, used by eight of the
   editor's files, and independent of the rest. Its plan comes later.
4. **Several objects and mixed values from T1.** The editor edits only its primary selection today;
   the protocol carries several from the start, since adding them later reworks it.
5. **The core covers the generic trees and floats**, verified before T1: the core's TreeView with
   drag and drop for the toolkit's draggable tree, the core's Window and docking's floats for its
   floating panels. A tree specific to one use (a scene's hierarchy, an asset browser) is the
   editor's, its own widget on the core's TreeView, not a toolkit widget.

#### Phases

Each a layer with its tests, the sandbox's Toolkit page growing with them.

- **T0 Bars.** A tool bar from command ids: buttons, toggles, split and drop-down buttons, groups,
  an overflow menu, tooltips with the shortcut and the reason a command is disabled. A status bar:
  items registered in left and right zones, hideable, with timed messages.
- **T1 The grid.** The property source protocol (rows by path, each object's value, mixed state,
  read only, visibility), categories, folding by path, search, the label split, virtualised rows,
  the edit gesture; the editors chosen in layers; bool, numbers with drag to scrub, text, enum and
  flags.

  T1's design (approved by the user, 2026-10-21), in GUI.Toolkit's `:property_source`,
  `:property_editor` (an editor's base and the registry), `:property_editors` and `:property_grid`:
  - **A row is a property of one value**: a bool, an integer, a float, a string, an enum or flags,
    the core's `Variant` (an enum and flags are integers, their names in the property's
    description). A compound value (a vector in T2, a nested object) is a property with children
    by path (`position.x`), so mixed values are per component without a value type of parts.
  - **The source** (`IPropertySource`): the object count; the properties under a path (name,
    display name, category, kind, hint, range and step, enum items, read only, visible, each read
    afresh, so `visibleWhen` is the source's); a value by path and object; `BeginEdit`,
    `PreviewEdit` (every object, live), `CommitEdit` (the originals restored, then one undo
    entry), `CancelEdit` (the originals restored). The undo is the source's; the grid has none,
    and does not merge edits by time. A client hears that the properties or the values changed.
  - **The grid is an `ItemView`**: categories and properties flattened into virtualised rows, a
    row type for a category and one per editor kind, their editors pooled. Folding is a set of
    paths (a category's `#Category`); search filters by display name and path, keeping a match's
    ancestors and opening them while it searches, the stored folding untouched; one label split
    for the grid, dragged at any row's boundary, mirrored right to left; the wheel scrolls and
    never changes a value.
  - **Editors in layers**: a registry by the property's hint, then its kind, then a row that says
    no editor fits; an application registers its own.
  - **T1's editors**: bool (a check box, mixed its indeterminate state), integer and float (the
    core's NumberField, scrubbed), text (one line, committed on Return or leaving it), enum (a
    ComboBox), flags (a drop-down of check rows, a flag that differs partly checked). Mixed shows
    the placeholder "Mixed".
  - Core first: NumberField's mixed state, Escape cancelling a scrub, its wheel steps optional;
    ComboBox's none (-1) and placeholder.
- **T2 Compound editors.** Vectors with mixed values per component; containers (add, remove,
  reorder); nested objects; default providers with revert and the changed marker; copying and
  pasting values.

  T2's design (approved by the user, 2026-10-21; the editor's inspector surveyed first: Float2, Float3
  and Float4 editors by type, lists that add a default element and move it, nested structs not shown,
  one "Revert to Prefab" per component, whole components copied, edits of a field merged across drags):
  - **Float2, Float3, Float4 on one row**: a group hinted with Core's type name (`Float2`, `Float3`,
    `Float4`) takes an editor of a number field per child, each its child's path (mixed per component,
    each scrub its own gesture), labelled with the children's display names. An editor registered as
    covering its children gives its row no chevron and no rows under it; a search matching a child
    shows the row. A Quaternion is a source's `Float3` group of Euler degrees, converted by the source.
  - **Lists**: `PropertyKind::List`, its elements the children `items.0`, `items.1`, named by the
    source; its info says what it allows (add, remove, move), its most elements (a `BoundedArray`),
    and the types an add chooses from (a polymorphic array). A change is one call, `EditList(path,
    edit)` (insert, remove, move; the element's type), one undo entry. Its row shows the count, mixed
    when the objects' differ, and an Add button (a menu of types when there are several); an element
    has a grip to drag it to another place and a menu (insert above, remove, move up, move down).
    Over several objects the elements shown are the fewest any has, and each change is made to each.
  - **Nested objects** are groups, to any depth. A polymorphic single field is not in T2 (reflection
    has none outside arrays).
  - **Defaults**: `GetDefault(path, object)` (empty: none) and `Revert(path)`, each object's own
    default back for the path and what is under it, one undo entry, the source naming it ("Revert to
    prefab"); the providers (the type's default, a prefab's) are the source's. A row is marked
    changed (an accent bar at its start, its name bold) when any object differs from its default; a
    group or a list when anything under it does.
  - **Copy and paste** go through the source as the edits do: `CopyValue(path)` writes the path and
    what is under it with the engine's serialization (`XmlSerializer`: the clipboard holds text),
    `PasteValue(path, text)` reads it back as one undo entry, or refuses with a reason the grid
    passes to the application (its status bar shows it). The grid has the row's menu and Ctrl+C and
    Ctrl+V on the focused row, and no format.
- **T3 The reflected source.** Core reflection and its attributes (display name, range, enum,
  `visibleWhen`) as a property source; the sandbox edits a reflected object on the lab's undo stack.

  T3's design (approved by the user, 2026-10-21), GUI.Toolkit's `:reflected_source`:
  - **Over reflected instances** of one type (several entities' components are several objects).
    bool, every integer width, `f32`, `f64` and `String` are value rows; an enum is choices from its
    enumerators, or flags with a `flags` attribute (its value read and written as an integer through
    the property's address); `ReadOnly` is a read-only row.
  - `Float2`, `Float3`, `Float4` are groups hinted with their names; a `Quaternion` is a `Float3`
    group of Euler degrees the source converts; `Nested` structs are groups, to any depth.
  - `Array<T>` and `BoundedArray` are lists (reflection's `emplaceDefault`, `removeAt`,
    `moveElement`; a bounded array's capacity its most); `Array<RefPtr<Base>>` adds by type
    (`createElement`, `canCreateElement`), the types' `displayName` its menu.
  - **The attributes the inspector reads**: `displayName` (else the name made readable),
    `description`, `range` (min, max, step), `visibleWhen` (`"prop"` or `"prop=1,2"`, evaluated by
    the source), a type's `displayName`; and `flags`.
  - **Any other type** is a row hinted with its type's name, for an application's editor (`Color`
    until T4, `Ref<T>`, `EntityRef`), else the row saying no editor fits.
  - **Defaults**: the type's, from an instance its creator makes; an application stacks its own
    providers over it (a prefab's).
  - **Writes** go through `SetProperty`, or the address for what a `Variant` cannot carry; each
    commit, list change, revert and paste is told to the application as one record, before and
    after, and the undo is the application's (ruling 1): the sandbox keeps a history of them.
  - **Copy and paste** use a serializer the application hands the source (the sandbox's
    `XmlSerializer`), so GUI.Toolkit gains no dependency.
- **T4 Colour.** An inline picker and a swatch editor on the engine's sRGB colour; HDR intensity in
  stops where a property has one; alpha; hex.

  T4's design (approved by the user, 2026-10-21):
  - **The core's** `ColorPicker` (a saturation and value square, a hue strip, an alpha strip, a hex
    field, number fields) and `ColorSwatch` (a colour over a checkerboard, mixed as such) edit the
    sRGB values an authored `Color` holds, in HSV; linear is never the storage. A perceptual model
    may come later.
  - **A colour is a value**: `PropertyKind::Color`, the `Variant`'s Color, one gesture; mixed is the
    whole colour. Its editor is a swatch and a hex field; the swatch opens the picker as a popup,
    a drag previews on every object, closing it puts the originals back and commits once (Godot's
    rule), Escape cancels.
  - **An HDR colour** (a reflected `Float4` with an `hdrColor` attribute: sRGB rgb, a linear
    intensity in w) is a group of `color` and `intensity`, the intensity shown in exposure stops
    (log2: 0 stops is x1), on one row: a swatch and a stops field, each its own gesture.
  - **Alpha** shows where the colour has it: not an HDR colour's, nor a Color's with `noAlpha`.
- **T5 Curves, gradients, the timeline.** On ruling 2's Core types, after that `master` work.

  T5's design (approved by the user, 2026-10-21; foundation.ui's toolkit `Timeline` surveyed first: only
  the PropertyAnimation editor uses it, the profiler has its own; its keys are (lane, index), no id;
  its `CurveCanvas` keeps its own key, tangent modes and interpolation):
  - **One time axis**: a `TimeAxis` holds pixels per second, the scroll and an optional frame rate;
    the ruler, the curve view and the dopesheet read it and keep no copy. The ruler's step is the
    smallest of {1, 2, 5} times a power of ten whose labels do not collide; labels are seconds, or
    frames with a frame rate.
  - **The curve view**, `CurveView` over an `ICurveSource`: its curves (name, colour, value range, most
    keys, locked) and each one's keys as Core's `CurveKey`s. Edits go as the property source's do
    (begin, preview, commit, cancel): moving keys, their tangents, a tangent mode, an interpolation,
    inserting and removing; each gesture one record for the application's undo. It draws with
    `EvaluateCurve` and applies `ApplyTangentModes` while previewing, so auto and smooth keys follow.
    Several curves on one canvas; a click or a box selects; Shift locks a drag to one axis; a double
    click on a curve adds a key; a key's menu sets its interpolation (constant, linear, cubic) and
    tangent mode (auto, smooth, flat, broken); a selected cubic key shows its handles, and dragging
    an auto or a flat key's handle makes it smooth (a handle that did nothing would be no handle); the wheel zooms about the pointer (Ctrl only time, Shift
    only value), a middle drag pans; framing fits every curve or the selection. Keys linked in time
    across curves are not kept (the old canvas's `LinkedTime` served only the particles' two-axis
    size, which is gone).
  - **The gradient view**, `GradientView`, edits one Core gradient as a value: a bar of
    `EvaluateGradient` over a checkerboard, alpha stops above it and colour stops below; a click adds
    a stop of what is there (an alpha stop on the upper half, a colour stop on the lower), a drag
    moves one and a drag off the bar removes it (never a kind's last, as Unity keeps one); a double click on
    a colour stop opens T4's picker as a popup, an alpha stop has a number field; the interpolation
    is a drop-down.
  - **In the property grid**, `CurvePropertyEditor` and `GradientPropertyEditor`: a preview on the row,
    the full view as a popup (closing it commits, Escape cancels). The application registers the
    types they take and where their parts are (`ParticleCurveFloat`: `keys`; `ParticleCurveColor`:
    `colors`, `alphas`, `interpolation`), so the toolkit names no runtime type and reflection gains no
    attribute. Over several objects a curve or a gradient is one value, mixed when they differ, an
    edit setting it on each (as a colour). It travels as one (the user, 2026-10-22): the `Variant`
    carries Core's keys (`Curve`) or a `ColorGradient` (`Gradient`), as it carries a `Color`, of
    `PropertyKind::Curve` and `::Gradient`; the reflected source takes the registrations
    (`RegisterCurveType`, `RegisterGradientType`: a type's name and its parts' properties, its most
    the lists' capacity, which Core's reflection says) and reads and writes the parts itself.
  - **The dopesheet**, `Dopesheet` over an `ITimelineSource`: rows folded by path (component, property,
    channel), a parent row the sum of the keys under it; keys addressed by (row, index), an edit
    returning the moved keys' new addresses so the selection survives the source's sorting. A click
    or a box selects, a drag moves keys in time (on frames with a frame rate), Delete removes, a double
    click inserts a key (its value the source's, the sampled one as a rule); the playhead scrubs on
    the ruler. A curve view can stand under it on the same axis. Its names are at the inline start
    and time runs left to right in either direction, as a ruler's numbers do.
  - **The sandbox**: a lamp's flicker curve and a sky gradient in the grid, and a short clip whose
    tracks animate the lamps, scrubbed in a dopesheet and a curve view, the grid following.
- **T6 The node graph.** A view over a graph source.

  T6's design (approved by the user, 2026-10-22; foundation.ui's `NodeGraphCanvas` surveyed first: it
  copies the graph into the widget and addresses it by index, a removal shifting the rest, so its
  caller mirrors parallel arrays through `OnConnectionDeleting`; its one user, the editor's animation
  graph, is a state machine of node-to-node transitions, parallel edges and an Any State):
  - **A source with stable ids**, `IGraphSource`: nodes (title, subtitle, position, header colour,
    movable, deletable, their input and output pins), pins (name, direction, a type's colour, one link
    or many), links (two pins, or two nodes) and frames. Edits are requests it applies, as the
    property and curve sources' are: moving nodes a gesture (begun, previewed, committed once or
    cancelled); connecting, disconnecting, deleting, creating a node and a frame's edits one change
    each; copying its text and pasting it back. The undo is the application's. `GraphSet` is a source
    in memory, for tests and the sandbox.
  - **A link asked for first** (ezEngine): while one is dragged the view asks the source of each pin
    it passes, which answers allowed or refused (and why) and which link it would replace; pins that
    fit are lit, the rest dimmed, a link about to be replaced marked.
  - **Two kinds of link**: a pin's (dataflow), a curve from an output to an input; a node's (a state
    machine's), a straight arrowed edge between two nodes' borders, parallel edges apart (A to B and B
    to A both seen), a node to itself, begun from its edge or its menu.
  - **Nodes are widgets, links drawn**: a node's header, title and a row per pin are widgets the sheet
    styles and right to left mirrors, and its body may hold an application's widgets (a pin's
    default); only the nodes in view are made, their widgets reused as the view moves; links are drawn
    by the view, culled to it (Godot, traktor).
  - **The view, mouse first**: a click or a box selects (Shift or Ctrl adds or takes away); a drag
    moves nodes (on the grid where it snaps); a drag from a pin links it, and dropped on nothing opens
    the menu of nodes with a pin that fits, the new node linked; a link's end dragged off its input
    rewires it or lets it go; a double click on a node is the application's (a subgraph opened); the
    wheel zooms about the pointer within limits, a middle drag pans; the menu over nothing frames every
    node or the selection and adds a node by category, with a search; the application highlights nodes
    and links (a state machine's active state); read only, it shows and does not edit. Its keys:
    Delete, Escape, and Ctrl+C and Ctrl+V as the property grid's; duplicating is the menu's.
  - **Frames**: a titled, coloured rectangle behind nodes, the nodes inside moving with it, resized
    from its edges; a frame with a body is a comment.
  - **The sandbox**: a small dataflow graph and a lamp's state machine (Off, On, Flicker and their
    transitions), each a `GraphSet` with its Undo.
- **T7 The rest.** A breadcrumb bar; toasts, unless the gamekit's serve.

  T7's design (approved by the user, 2026-10-22, after a survey of breadcrumb bars and toasts in
  foundation.ui, Godot, zed, doriax, Lumix, Zero, ezEngine, eepp, gpui and LunarSong). foundation.ui's
  `BreadcrumbBar` takes strings, navigates on press (the current segment too), has no overflow (its
  segments run past a narrow bar) and leaves its one user, the editor's asset view, mirroring a
  table of groups by index. Its toasts are one class written twice, `UI.Toolkit`'s and a copy in
  `UI.Gamekit` ("identical behaviour"), only for a module rule; no reference has a game's toasts
  apart from an editor's.

  **The breadcrumb bar**, GUI.Toolkit's `breadcrumbbar`:
  - **Segments carry their target**: each item a text, an optional icon, a tooltip (the full path)
    and an id the application gets back on navigation (doriax pairs a name with its path), so no
    table beside it. A helper splits a `/` path, a drive or root kept as one segment.
  - **A click navigates on release** (as the lab's popups open); the last segment is the current
    one, shown so and inert.
  - **A separator is a chevron**: given a children callback, it opens a menu of what lies in the
    segment before it, and a child chosen is navigated to (Explorer's; none of the references has
    it); without one, a mark.
  - **Overflow**: the current segment and as many before it as fit, filled from the end; the rest
    fold into a "…" button whose menu lists them (doriax), the root among them (user's answer 4).
  - **Drops**: the segments but the current take drops of the application's types through the
    core's drag and drop (doriax).
  - **The path editable** (user's answer 3): opt-in, a click on the bar's empty end turns it into a
    field holding the path; Return gives the text to the application, Escape cancels. No default
    shortcut (mouse first).
  - **Keys and access**: one tab stop; Left and Right move along it, Return navigates, Down opens a
    chevron's menu; the current segment is marked current to a screen reader.
  - **Right to left**: its segments run right to left and its chevrons mirror (a path reads as text,
    unlike the graph's canvas).
  - **History is the owner's**: back, forward and up belong to the view that holds the bar, as in
    every reference.

  **Toasts, in the core** (user's answer 1), `experimental.gui`'s `ToastHost`, beside its tooltips and
  popups, so a game and the editor share one host, with no copy and no Gamekit for an editor:
  - **A host per window** the application places: its cards alone take input, and it never takes
    focus.
  - **A request**: a message and an optional title, or a widget for its content (Godot's and zed's
    take any; the user's point, 2026-10-22) with the host's card round it (severity, close, actions,
    pause, timeout, key); `Bare` drops the card, for a game's own (an achievement's banner), keeping
    the lifetime and stacking. A severity (info, success, warning, error; colour and icon the
    sheet's), up to two actions (callbacks or command ids), a duration and a key. A message may be
    the core's rich text. A widget's request carries the text to announce, else its accessible name.
  - **Posted from any thread** (user's answer 2): a post from another thread waits in the host's
    queue until its frame (Godot, zed, Zero); a widget cannot cross threads, so such a post passes
    a function that makes it, called on the GUI's thread.
  - **Lifetime**: by severity unless the request says (info and success 5 s, warning 8 s, an error
    until closed, as the editor's notices are), each default the host's to change; hovering or
    focusing a toast pauses them all; each has a close button; one leaves after the event that
    closed it, never inside it.
  - **Repeats and limits**: a post with the key of one shown replaces it (its widget too) and counts
    it ("×12"), which also updates a progress toast (Godot, zed); four shown at once, the rest
    queued until there is room.
  - **Placement and motion**: stacked at the bottom end corner (the bottom left, right to left), the
    sheet's to move; in and out by the core's transitions, a request's classes choosing which (the
    theme's slide up, `pop`, `still`, a game's own), a reduced-motion preference fading only (the
    user, 2026-10-22: the caller chooses, the sheet says what each choice is).
  - **Announced** through the core's announcements, an error interrupting, the rest waiting.
  - **History is the application's**: the host reports each toast posted and dismissed, for a log
    or a bell that shows them again (Godot's); it keeps none.
  - **From scripts** (the user, 2026-10-22): a request is a value with no callbacks in it, so a
    script fills one in as code does (`ToastRequest r; r.title = ...; r.addAction(id, text,
    command)`) and posts it (`ToastHost.nearest(node).post(r)`, or `postContent(r, widget)`); an
    action's button sends `activate` with the action's id, as a menu item does. Code passes its
    callbacks beside the request (`Post(request, onAction)`, `PostFromAnyThread(request,
    makeContent, onAction)`).

  Layers, each a commit with its tests: **T7a** the bar (segments, clicks, overflow, keys, right to
  left, access); **T7b** its chevron menus, drops and editable path; **T7c** the toast host in the
  core; **T7d** the sandbox: breadcrumbs over an invented asset tree on the Toolkit page, and buttons
  posting each kind of toast (a repeat, one with an action, a widget, one that stays).

### C The code editor (approved by the user, 2026-10-22)

`GUI.Code` (`experimental.gui.code`), after T7 as the user asked. The survey (2026-10-22) read
foundation.ui's code editor and its users, zed, eepp, Godot, ZeroEngine, LumixEngine, traktor and
doriax.

foundation.ui's (`UI.Toolkit/Code/`, about 4,900 lines) keeps a line array and a sound incremental
lexer cache, and serves four of the editor's pages: scripts (AngelScript and Luau: compile
diagnostics, breakpoints, the execution line while debugging, hover values, completion over the
script API), shaders (HLSL with the engine's properties block), the game UI document (markup, with
completion from the markup registry) and its theme (the stylesheet, plain text: it has no lexer). It
has one cursor, diagnostics by whole line (no squiggles), fixed-pitch drawing (tabs and wide
characters misalign), every visible token drawn each frame, and no folding, auto-close, IME or
accessibility.

Findings that decide the design:

- **Every editor keeps an array of lines** with tokens cached per line, state carried from line to
  line, and draws only the rows in view; none but zed (whose rope serves collaboration) keeps a gap
  buffer or a rope.
- **zed**: positions that survive edits (anchors) hold selections, diagnostics and folds; an undo
  transaction keeps the selections before and after it; one map from document lines to visual rows
  for folds and wrapping; a language's configuration is data; a key context on the editor keeps the
  keymap declarative.
- **eepp**: a line's tokens are cached by its hash and the state it starts in, so re-tokenizing
  stops where a line ends as it did before, and runs only to the last row in view.
- **Godot, Zero**: a language service the host implements (completion, validation, lookup) keeps
  the widget language-neutral.
- **Pitfalls**: Godot's cache invalidates to the end of the file; eepp draws squiggles as `~`
  glyphs; an undo that forgets the cursors breaks multi-cursor undo; folding and wrapping kept as
  two maps drift.

#### Rulings (user, 2026-10-22)

1. **A language's definition is a value** (keywords and types, token rules, brackets and their
   auto-close pairs, comment tokens, indent rules), built in code or read from a file through the
   engine's XML serialization. The library ships none: its owners do (the script modules
   AngelScript's and Luau's, the sandbox its own), as foundation.ui's rule had it.
2. **No tree-sitter**: a dependency with no AngelScript grammar, where a state-carrying tokenizer
   serves these languages (zed's own cheaper path).
3. **Plain search first** (case, whole word); a regular-expression engine is a dependency decided
   on its own.
4. **No minimap.**
5. **VS Code's keys by default** (Ctrl+D, Alt+Up and Down, Ctrl+/, Ctrl+F, H and G, F3, Alt+click),
   bound in the keymap as commands, every one rebindable.

#### The design

- **The document**: an array of lines (text, hash, token cache); positions (line, byte column) on
  character boundaries; anchors moved through each edit, biased left or right, holding selections,
  diagnostics, markers and search hits; undo as transactions of (range, old text, new text) with
  the selections before and after, edits within 300 ms merged, a compound edit one step.
- **Drawn with the lab's text**, not fixed pitch: each line in view shaped by the core's text runs
  and cached until its text or its highlighting changes; tabs to tab stops in layout; hit testing by
  the shaped positions. Code runs left to right in every language (as the graph's canvas), a
  string's Arabic still shaped. Only rows in view are laid out; a very long line is cut for layout.
- **Multi-cursor from the start**: the selections sorted and disjoint, with one being dragged; a
  double click selects a word, a triple a line, Alt+click adds a cursor, Alt+drag selects a
  column; commands to add the next occurrence and a cursor above or below.
- **Syntax**: a per-line tokenizer carrying state, cached by (hash, entry state), run lazily to the
  end of the view; colours from the sheet by parts (`codeeditor::keyword` and the rest), so themes
  restyle code.
- **Editing**: auto-indent, auto-close and matching brackets, comment toggle, indent and outdent of
  a selection, smart Home, moving and duplicating lines.
- **The gutter**: line numbers and the host's markers per line (breakpoint, execution line,
  bookmark, kinds of its own), drawn by the sheet; a click in the marker column is the host's.
- **Diagnostics**: anchored ranges with a severity and a message, set by the host: a wavy path
  under the range (the worst severity where they overlap), an icon in the gutter, a tooltip on
  hover; anchored, they follow edits while the host recompiles.
- **A language service** the host implements: completion and hover (signature help later). The
  completion list is a core popup that does not take the focus, filtered fuzzily, with a key
  context of its own so Up, Down and Return go to it.
- **Find, replace and go to line**: a bar of the core's widgets; matches highlighted; replacing all
  one undo step.
- **Folding and soft wrap**: one map from document lines to visual rows, which the caret and hit
  testing share; fold ranges from brackets, indentation, `#region`, or the host; whitespace shown
  where asked.
- **IME** composition as the core's fields; to a screen reader a multiline text input whose value
  is the current line, a moved caret announced. **Read only** for logs and generated code.

#### Phases

Each a layer with its tests.

- **C0 The document.** Lines, positions, anchors, edits, undo with selections.
- **C1 The view.** Shaped lines in view, multi-cursor carets and selections, the mouse, editing
  keys through the keymap, scrolling, the clipboard, IME, the line numbers.
- **C2 Syntax.** The language definition, the incremental tokenizer, the sheet's colours;
  definitions for AngelScript, Luau, HLSL with properties, XML markup and the stylesheet language.
- **C3 Editing.** Auto-indent, brackets, comments, indentation, multi-cursor commands, line moves.
- **C4 Find, replace, go to line.**
- **C5 Markers and diagnostics.** The gutter's markers, squiggles, icons, hover.
- **C6 The language service.** Completion and hover.
- **C7 Folding, soft wrap, whitespace.**
- **C8 The sandbox's Code page**: its own AngelScript with the engine's compiler's diagnostics as
  it is typed, breakpoint markers, and a shader.

### K The gamekit (approved by the user, 2026-10-23)

`GUI.Gamekit` (`experimental.gui.gamekit`): a game's screens. The survey (2026-10-23) read
foundation.ui's `UI.Gamekit` (about 800 lines) and engine.ui's script facade over it, Godot,
RmlUi, ezEngine, traktor, doriax, eepp, LunarSong, LumixEngine and ZeroEngine (the last two have
nothing for a game's screens), and what the lab's core already has.

foundation.ui's keeps a `ScreenStack` of `UIScreen`s (push, pop, replace, clear; Overlay, Modal or
Opaque; a transition kind in markup, played in code), a `ButtonPrompt`, `MenuList`, `Bar`,
`Ticker` and toasts. Its pitfalls: an exiting screen stays clickable until its animation ends, so a
second Back pops the screen under it; a pop during a push transition fades a view still fading in;
an Opaque screen sliding out reveals a lower one only when the slide ends; Replace and Clear drop
their saved focus without restoring it (the focus stack's depth leaks); Back is a script call, not
bound to Escape or the pad, and a screen cannot refuse it; markup sets a transition's kind but not
its time or easing; a prompt shows an action's first binding whatever the device.

Findings that decide the design:

- **RmlUi**: each element remembers its focused child and the context keeps a history of
  documents, so closing one refocuses the leaf of the one under it, with no saved tokens to leak;
  Back is each document's own (`onescape`); transitions are CSS, with `transitionend` and
  `animationend`.
- **eepp**: a closing window stays alive through its fade, guarded against closing twice;
  **traktor**: the old stage fades out before the next enters, and a stage says whether it keeps
  updating meanwhile.
- **ezEngine**: the world clock pauses while the menu is open and goes back to what it was; the
  Escape that opened the menu cannot close it in the same frame.
- **LunarSong, doriax**: a full-screen document hides overlays by an override that is removed, not
  undone; screens covered get a release so nothing stays pressed; a transition's time step is
  capped so a hitch does not skip it.
- **Input modes and prompts**: no reference tracks the device in use or draws prompts by device.
  Godot shows the focus ring on any navigation action and hides it on a pointer press, and counts a
  stick only past its dead zone on a fresh press; every reference that names a binding names the
  first one, whatever the device (foundation.ui's `ButtonPrompt` too); ezEngine broadcasts a change
  when a binding changes.
- **The lab's core already has** focus scopes with remembered focus and `nav-default`, pad
  navigation (the stick with hysteresis), `:focus-visible`, key contexts and `pad-*` keys, commands
  that stop at a modal, modal popups that save and restore focus, tweens, CSS transitions and
  keyframes, `prefers-reduced-motion`, toasts and data binding. It lacks a screen stack, a way for a
  node to play its exit and then go (no `animationend` or `transitionend`), one Back across layers
  (Escape is the dispatcher's special case for popups, run before the keymap), an input kind that
  tells the pad from the keyboard (a C++-only bit, no change event), a hidden cursor, and a tween
  of a data model value. The sandbox's game menu builds every one of these by hand in its script
  and its host.

The sample games use foundation.ui's gamekit through `<screen>` roots alone (46 documents in all
six games: 30 modal, 16 overlay; 29 fade and 2 scale transitions; `default-focus`) and the facade
(`ui::push` 54 times, `ui::pop` 19, `ui::clear` 23, `ui::find*` and `Screen.find*` about 400).
None uses `MenuList`, `Bar`, `Ticker`, `ButtonPrompt` or the gamekit's toasts.

#### Rulings (user, 2026-10-23)

1. **The input kind lives in `foundation.input`**, so a game without the GUI (a prompt in the 3D
   world) has it too; the GUI reads it through its host.
2. **Glyphs come from the sheet**: a prompt shows its binding as an attribute and a game's sheet
   draws the glyph; no glyph table in code.
3. **The stack is an element**, any number per root.
4. **Pausing is reported, not done**: the stack says the game is paused; the game stops its clock.
5. **`Bar` and `Ticker` stay out**: the core's data tween and a sandbox pattern instead.
6. **The gamekit may depend on `foundation.input`** (the InputMap is Foundation's, not the
   engine's).

#### The design

- **What goes in the core**, because it is not a game's alone:
  - *Leaving*: `Node::Leave()` puts a node in a leaving state (`:leaving`, the sheet's exit
    transition or animation runs), takes it out of hit testing and focus, and removes it when its
    transitions and animations end (at the next update when there are none; reduced motion is the sheet's to answer, as a theme's `prefers-reduced-motion` rule does, so a toast may still fade; an infinite or paused animation is not waited for);
    `transitionend` and `animationend` events.
  - *Back*: a `back` command bound to Escape and the pad's east button in the keymap; an open popup
    answers it by closing, and the dispatcher's Escape special case goes.
  - *The input kind* (ruling 1): `foundation.input` keeps it, pointer, keyboard or pad, from the
    events it sees (the pad only on a press or a stick past its dead zone), with a change
    notification, for any consumer; the GUI core holds the kind its host gives it (GUI.Shell's
    bridge passes the tracker's on), with a change event, script access (`gui.inputKind`) and a CSS
    hook (a media feature, `@media (input: pad)`), and without a host keeps today's pointer or
    keyboard switch; the focus ring follows it as Godot's does; a pad hides the cursor (`cursor:
    none` added).
  - *A data value tweened*: a tween route for a data model value, so a score counts up with no
    widget to drive it (the reason `Ticker` existed).
- **The screen stack** (the gamekit): a `screens` element, placed where a game's screens go (any
  number per root), holding `screen` elements. Push takes a document (loaded fresh each time) or a
  node; pop, replace and clear; a screen says whether it covers what is under it (hides it),
  blocks its input, and pauses the game (the stack reports `IsPaused`; the game decides what pausing
  means, as a library must). A screen remembers its focus (the core's remembered focus), so a pop
  refocuses the screen now on top, with no tokens; on push, `nav-default` or the first focusable.
  A screen under a covering one gets a pointer release, so nothing stays pressed.
- **Transitions** are the sheet's: a screen pushed is `:entering` until its enter transition ends,
  one popped `:leaving` (the core's Leave); the stack takes a popped screen off at once (it no
  longer counts for Back, focus or input) and keeps it drawing until it has left; a pop during an
  enter cancels the enter and leaves from where it is; screens under a covering one show again
  when it starts leaving, not when it has gone.
- **Back on the stack**: the top screen gets `back` first (a script's handler may take it, or the
  screen may say `back="none"`); else the stack pops, never below its last screen; the Back that
  pushed a screen cannot pop it in the same frame.
- **Lifecycle** as events a script listens to with `on`: `screenenter`, `screenshown` (enter
  ended), `screenleave`, `screenhidden` (covered) and `screenrevealed`.
- **Input modes**: the stack answers whether the game should have the input (no blocking screen
  on top, and the pointer not over a widget that takes it: the core's `IsPointerOver`).
- **Button prompts**: a `prompt` element names an action (and its set); it reads the game's
  InputMap (`foundation.input`, which the gamekit may depend on; ruling of 2026-10-23) after the
  player's rebinding, honouring set priority and enabled sets, takes the binding for the input
  kind in use (else any, else nothing), labels it with `foundation.input`'s `BindingNames`, and
  updates when the input kind or the bindings change. It shows its binding to the sheet as an
  attribute (`binding="pad-south"`, `kind="pad"`), so a game's sheet draws its own glyphs
  (`prompt[binding="pad-south"] { background-image: url(...) }`) and the text label is the
  fallback: a satellite's theme is a sheet (ruling 4), and no glyph table is code.
- **Not in the gamekit**: toasts (the core's, T7); `MenuList` (markup, navigation and commands
  make one); `Bar` (its drain is one tween; a lagging damage trail is two bars and a tween, shown
  in the sandbox); `Ticker` (the core's data tween); presets and a game's policy (the library is
  not an application); more than one player (one input kind, one stack's input) until a game
  needs it; the engine's hosting (ruling 1).
- **The script facade** keeps the call shapes the sample games use where they fit (`push`, `pop`,
  `replace`, `clear`, `back`, `top`, a screen's `find*`), so moving them is a change of documents
  and themes more than of scripts.

#### Moving the sample games (when the engine moves onto the lab)

Each game's `.sml` screens and `.sss` theme become the lab's markup and CSS by a migration script,
then are looked at against the old pictures (the images are the user's to approve); `<screen
mode="modal">` becomes a screen that blocks, `mode="overlay"` one that does not, `transition` the
sheet's enter and leave rules; the scripts move to the facade above; each game is played through
its screens in the editor and in its exported player.

#### Phases

Each a layer with its tests.

- **K0 Leaving.** `Node::Leave`, `:leaving`, `transitionend` and `animationend`. Built (vg, 2026-10-23).
- **K1 Back.** The `back` command in the keymap; popups answer it; the Escape special case goes. Built (vg, 2026-10-23): a modal dialog answers it as its close button does.
- **K2 The input kind.** K2a in `foundation.input` (on master, as Foundation work): the tracker,
  its rules and its change notification. K2b in the lab: the core's kind given by the host and the
  bridge passing it on, the change event, `gui.inputKind`, the media feature, the focus ring's
  rules, `cursor: none` and the hidden cursor on a pad. Built (K2a on master, K2b on vg,
  2026-10-23): the bridge passes the tracker's changes, and the dispatcher still follows what it
  is given (a press, a key, a pad button), so a host without a tracker, or a test, drives it.
- **K3 A data value tweened.** Built (vg, 2026-10-23): `GUIContext::AnimateData` and `gui.tween`, in the value's own type.
- **K4 The screen stack.** Push, pop, replace, clear; covering, blocking, pausing; focus per
  screen; the release under a cover; Back; the lifecycle events; the facade. Built (vg,
  2026-10-23): `<Screens>` and `<Screen covers blocks pauses back>`, the screens under a blocking
  one inert (the core's), a press on a screen's empty area keeping the focus so `back` reaches it,
  `pausechange` beside the five lifecycle events (named events, the core's), and the stack's
  script surface (`push` a document, `pushNode`, `replace`, `pop`, `clear`, `back`, `top`); the
  engine's `ui::` facade waits for the move of the sample games.
- **K5 Transitions.** Entering and leaving through the sheet; a pop during an enter; covered
  screens shown as the cover leaves; the capped time step. Built (vg, 2026-10-23): the core's
  `Node::Enter` (`:entering` while the sheet's entrance plays, a keyframe animation or the values
  it starts from as `@starting-style`, then the node's own style, then `entered`), which the
  stack's push uses (`screenshown` follows it), and `GUIContext::SetMaxClockStep`, off by default.
- **K6 Button prompts.** The InputMap read by input kind after rebinding, the label, the sheet's
  glyphs, live updates. Built (vg, 2026-10-23): `<Prompt action set>` reading the host's
  `InputPrompts` (its ActionRuntime, installed on the GUI through the core's new service slot, told
  of a rebinding by `NotifyBindingsChanged`); a pad's binding while a pad plays, a key or mouse
  button under the keyboard or the pointer, else the action's first; `binding` is the label in
  lower case joined by dashes (`pad-south`).
- **K7 The sandbox's game menu on the gamekit.** The showcase's hand-built stack, Back, fades,
  pause and host plumbing replaced by the gamekit; a HUD under a pause screen; prompts that follow
  the device. Built (vg, 2026-10-23): the frame (backdrop, title, `<Screens>`) and a document per
  screen in Data/Assets/ui/showcase/screens/, each pushed fresh and wired as it is pushed;
  entrances and exits in the sheet; Back the stack's, the HUD's own opening the pause screen;
  the game stops while the stack reports a pause; the score counts up through a data tween in
  whole numbers (`whole`, as a script's numbers arrive as fractions); the HUD's pause prompt
  follows the device. Building it showed a screen with nothing focusable must take the focus
  itself, or Back never reaches it.

### E The GUI's assets and the engine's GUI (design 2026-10-23; the user's go: E1 to E3, then on)

The user's direction (2026-10-23): the GUI's resources and pipeline, then `engine.gui`, the
engine's integration, with full parity with Engine.UI. This takes up what round three's ruling 1
and the 0.2 backlog deferred (the engine's hosting, the decision to replace foundation.ui).

#### Rulings (user, 2026-10-23)

1. **They coexist for a while, then the GUI takes over.** The engine's sandbox and the native
   samples (`Code/Samples`) move onto `engine.gui`; the sample games follow. Nothing is removed
   (foundation.ui, Engine.UI, Engine.UI.Script, the editor's game UI pages, their pipeline) until
   the work is blocked and the user says how to proceed. (The user first said they would not
   coexist, then, the same day, that they must for a while.)
2. **Sources are `.gml` (markup) and `.css` (sheets).** A distinct markup extension, so the
   importer never takes an unrelated XML file; plain CSS for sheets.
3. **References are asset guids.** A sheet's `url("{guid}")`, a document pushed by its guid, an
   image's source: as every other asset reference in the engine, so a rename or a move breaks
   nothing.
4. **Full parity with Engine.UI**, designed now and built in phases.
5. **The plain script names are the GUI's.** foundation.ui reflects nothing to scripts; only
   Engine.UI.Script's handles clashed with the lab's (Label, Button, Slider, ProgressBar, RichText,
   Image, Screen). They took a `UI` prefix on master (`UILabel`, `UIScreen`, ..., 4f9511667, which
   the mirror engine ports), so one process binds both while the games move.
6. **The order:** E1 to E3 (the loaders' reports, the resources, the pipeline), then the user
   hears before `Engine.GUI` starts.

#### What foundation.ui's chain does (the survey)

- **Resources** (`UI.Resource`): a document and a theme, each cooked as validated text the runtime
  parses again (`UIDocumentSource{markup}`, `UIThemeSource{stylesheet, icons}`); a theme embeds
  the SVG of each `@icon`. Its module, `kUIResourceModule` ("ui"), belongs to the UI domain with
  the vector images' and the fonts'.
- **Pipeline** (`UI.Pipeline`): linked-source assets (the text in `Sources/`, as a script's), each
  builder validating by parsing (a document through the gamekit's markup, a theme through the
  stylesheet loader, its icons read with `ReadVectorImage`) and failing the cook on an error; one
  importer for `.sml` and `.sss`; creators for File > New and MCP's `asset_create`. Registered in
  `Pipeline.Registration` (the importer and builder counts are tripwires, as are the composition
  test's resource module count and its factory count).
- **The engine** (`Engine.UI`, 4200 lines): one subsystem for the application, a screen tier (a
  root with the gamekit's stack and an overlay layer) and a scene tier (a root per scene), a tier
  per game run in the editor; components `ui.Canvas` (a document and theme, an order, a scaler,
  over the screen or into a render texture), `ui.Billboard` and `ui.WorldPanel` (a render-texture
  quad, picked by a ray); documents rebuilt when their cooked product changes (hot reload); images
  by guid through the engine's textures; input from the engine's input subsystem (one active root a
  frame, the action layer told what the GUI took); drawn as a scene overlay and a screen overlay.
- **Its script surface** (`Engine.UI.Script`): `ui::push(guid)`, `pop`, `replace`, `clear`,
  `back`, `find*`, a screen's `find*`; the application installs it per script context.
- **The player and the editor**: project settings for the default theme, the fonts and the loading
  document; the player's boot splash; the editor's run screens per Game tab; asset pages for a
  document (with a live preview) and a theme.
- **The lab today**: `GUI.Resources` and `GUI.Pipeline` are empty; `GuiHost` owns its windows and
  their backbuffer (it cannot draw into a pass the engine owns); `GUI.Shell`'s bridge takes shell
  events; the gamekit pushes a document by path through the GUI's resource provider; the lab is off
  in a web build.

#### The design

- **`GUI.Resources`** (`experimental.gui.resources`, framework free: Core, Resource, Content): a
  `GuiDocument` (its markup) and a `GuiStyleSheet` (its CSS), each cooked as validated text
  (`GuiDocumentSource`, `GuiStyleSheetSource`), and the module `kGuiResourceModule` ("gui"). No
  icons embedded: a sheet reads an SVG by guid as it reads any image, through the vector images'
  own resource (VG.Resource), so an icon is cooked once however many sheets use it.
- **`GUI.Pipeline`** (`experimental.gui.pipeline`, tools only): a `GuiDocumentAsset` (`.gml`) and a
  `GuiStyleSheetAsset` (`.css`), linked-source assets; builders that validate by parsing (the
  markup through the lab's loader with every element type the tools register, the core's,
  the gamekit's, the toolkit's and the code editor's; the sheet through the CSS parser) and fail
  the cook on an error, warnings for what was dropped; what a document or sheet reads by guid
  (images, vector images, fonts, sheets it imports, documents) recorded as the cook's dependencies;
  one importer for both extensions; creators for both (File > New, `asset_create`); registered in
  `Pipeline.Registration`, its counts bumped. The core's markup loader and CSS parser learn to
  report what they drop (the cook's warnings).
- **References by guid**: the lab's `IResourceProvider` already takes a string; the engine's
  provider answers `{guid}` from its resources (a document's or sheet's text, an image from a
  texture, an SVG from a vector image), so markup, sheets and the gamekit (`PushDocument("{guid}")`)
  need no change, and a path still works in the sandbox.
- **`Engine.GUI`** (`engine.gui`, built when the lab is): the engine's GUI, at parity with
  Engine.UI:
  - *A subsystem* for the application: one `GUIContext`, its style manager, the project's default
    sheet and fonts, the engine's resource provider; a screen tier (a root holding the gamekit's
    stack and an overlay layer) and a scene tier (a root per scene); a tier per game run in the
    editor's Game tabs; documents and sheets rebuilt when their cooked products change (hot
    reload).
  - *Drawing into the engine's frame*: `GUI.Hosting` gains a way to draw a root into a pass the
    caller owns (the screen and scene overlays) besides its window host, and its offscreen picture
    for render textures.
  - *Input from the engine's input subsystem*: its active source's events through `GUI.Shell`'s
    bridge, the input kind from the engine's tracker (K2), one active root a frame as Engine.UI
    picks it, the action layer told what the GUI took.
  - *Components*: `gui.Canvas` (a document and sheets, an order, a scaler, over the screen or into
    a render texture), `gui.Billboard`, `gui.WorldPanel` (a render-texture quad, picked by a ray).
  - *The script surface*: the lab's `gui` facade and the gamekit's stack, installed per script
    context with the screen tier's (or a run's) root; `gui.screens()` the screen tier's stack,
    pushing by guid; a screen's `find`.
  - *The player and play-in-editor*: project settings for the default sheet, the fonts and the
    loading document, the boot splash, the run screens.
  - *Composition*: its domain (resources `gui`, `vg`, `fonts`) beside the UI domain while both live
    (ruling 1); the composition tests' counts follow.
- **The editor**: asset pages for a `.gml` document (a code editor and a live preview, drawn by the
  lab offscreen into the editor's view) and a `.css` sheet; the inspector naming the new asset
  types; the MCP guide updated (the generic asset tools serve them).
- **The web**: the engine's GUI ships in a web game, so the lab, its hosting and the engine's GUI
  build for wasm (WebGPU), which they do not today.

#### Open questions (for the user)

- **Where a theme's tokens come from**: answered (the user, 2026-10-23): a project setting naming a
  list of sheets, applied in order over the lab's built-in sheet; one sheet is a list of one, and a
  game layers its own over a shared one.

#### Phases

Each a layer with its tests.

- **E1 The loaders report.** The markup loader's and CSS parser's warnings (what they dropped). Built (vg,
  2026-10-23): `GuiDiagnostic` (a level, a line, a message); the loader's and parser's optional lists
  (else the log); `CheckStyleSheetNames` against the style table and the registered types' CSS names.
- **E2 `GUI.Resources`.** The two resources, their factories, the module. Built (vg, 2026-10-23):
  `GuiDocument` and `GuiStyleSheet` (cooked as `GuiDocumentSource`, `GuiStyleSheetSource`),
  `kGuiResourceModule` ("gui"), free of the GUI framework.
- **E3 `GUI.Pipeline`.** The assets, builders, dependencies, importer, creators; registration. Built
  (vg, 2026-10-23): guids found in the text are the product's runtime references (a guid naming no
  asset a warning); registered in Pipeline.Registration only when the lab is built (two builders,
  an importer and two creators more then).
- **E4 Drawing into a caller's pass.** `GUI.Hosting`'s overlay path. Built (vg, 2026-10-23):
  `GuiPassRenderer` on a device the caller gives (no window, no shell): a root drawn into an open
  pass at a region of its target (`GuiPassView`: format, depth-stencil, samples, encoded, frame); a
  renderer per kind of pass, rewound once a frame; a baker of its own, recorded once a frame; images
  registered by key told to every renderer, those made later too. The root's owner styles and lays
  it out first.
- **E5 `Engine.GUI`'s subsystem.** The context, the tiers, the resource provider by guid, hot
  reload, the overlays; composition. Built (vg, 2026-10-23): `GuiResourceProvider` (a document's
  markup, a sheet's CSS, a vector image's SVG, a texture's image by guid, each bound only as the
  cooked type its instance has; `Poll` follows re-cooks); `GuiSubsystem` (one context and style
  manager, the built-in theme under the game's sheets as one author sheet `@import`ing each guid in
  order, the built-in font; the screen tier a root holding the gamekit's stack under an overlay
  layer, the scene tier a root per scene; both drawn through `GuiPassRenderer` above Engine.UI's
  overlays, the bakes recorded in the screen tier's prepare step; a re-cooked sheet or document
  re-applies the sheets); `GuiDomain` (resources `gui`, `vg`, `fonts`) in the composition when the
  lab is built. Pushed screens are not rebuilt on a re-cook (the canvases are, with E7); the screen
  tier lays out at the target's size (the game's resolution and orientation come with E9).
- **E6 Input.** The engine's input into the GUI, the active root, the action layer told. Built (vg,
  2026-10-23): the input subsystem's active source through GUI.Shell's bridge (a bridge per root;
  the bridge's poll now takes any devices, `PumpDevices`, an `InputSurface`'s among them), one root
  a frame (an occupied overlay layer modal; else the GUI under the pointer, the screen tier's
  first; else the screen tier while it holds a screen; else the first scene root with content),
  a press kept with its root until let go, the scenes input may reach as the input subsystem's
  binding says. The action layer's pointer and keyboard claims are added to Engine.UI's in the
  BeginFrame lane and cleared at EndFrame, after the frame's actions read them. The engine has no
  input kind tracker of its own: the GUI keeps one, fed from the same source. The pad's confirm on
  release and the touch guide are Engine.UI's and not carried over; the lab's own pad handling
  serves.
- **E7 Components.** Canvas, billboard, world panel. In three layers:
  - **E7a The canvas.** Built (vg, 2026-10-23): `gui.Canvas` (a document, a sheet of its own as a
    subtree sheet over the game's, an order, visible, interactive (inert when not), a scaler:
    constant pixels or a reference resolution fitted and centred) in a `canvas` host in its scene's
    root; rebuilt when its document's product changes, its host swept when it goes, the hosts kept
    in order. One sheet per canvas: a sheet imports others. The document's root is the canvas's
    area (the host arranges it), so it never takes the pointer unless it paints.
  - **E7b Billboards.** A document over an entity, projected through the view's camera. Built (vg,
    2026-10-23): `gui.Billboard` (a document, a sheet of its own, an offset in world or the
    entity's space, fixed or distance scale within limits, visible) in a `billboards` layer under
    the scene's canvases; placed as each view draws (`PlaceBillboard`, pure: hung by its bottom
    centre on the projected point, hidden behind the camera keeping its layout, scaled toward that
    point), from its last layout's size (a new one settles the next frame). A scene's GUI draws, and
    takes the keys, only while it holds a canvas or a billboard.
  - **E7c Render textures and world panels.** A canvas drawn into a texture, a world panel's quad,
    the pointer by a ray, the bakes recorded before the scene. Built (vg, 2026-10-23): a canvas's
    `renderMode` (ScreenOverlay or RenderTexture, a texture's size) and `gui.WorldPanel` (a size in
    metres, a density, interactive, visible), each in a root of its own with its input;
    `RenderTextures` (the host's, before the scene, once a frame) records the bakes, then draws each
    into an sRGB texture (multisampled with a stencil when the device takes it), a canvas's
    entity's sprite and decal shown it, a panel's sprite driven (entity-oriented, after the tone
    mapping, a transparent two-pixel edge); textures made again at a new size and let go with their
    canvas or panel (the GPU waited for). The pointer reaches the nearest interactive panel by a ray
    through its scene's camera (through the view's centre when the mouse is captured), after the
    screen's and the scenes' GUI; a press stays with it. A panel takes the mouse, not fingers. The
    ray math is Engine.UI's, kept in Engine.GUI while both live. No host calls `RenderTextures`
    yet (E9). The canvas kept data version 1: no scene had saved one.
- **E8 The script surface.** The facade per context, pushing by guid. Built (vg, 2026-10-23): the GUI
  domain's script facade registers the lab's surface (`gui`, the widgets, the gamekit's stack) and
  each component's `.of(entity)` with `root()`, the tree as a node; `gui` and the three component
  names join the behaviour prelude (the facade name count +4 with the lab built).
  `GuiSubsystem::ExposeToScript` gives a script context the screen tier's root: `gui::root()`, its
  stack `ScreenStack::of(gui::root().find("screens"))` (the stack's id), which pushes a document by
  `"{guid}"` through the provider, so no `gui.screens()` was needed. No host installs it yet (E9).
- **E9 The player and play-in-editor.** Settings, splash, run screens. Rulings (the user,
  2026-10-23): every game constructs the GUI when the lab is built; it shares Engine.UI's fonts (the
  project's `defaultUiFontId` and `uiFontIds`); the manifest gains the ordered sheet list and a GUI
  loading document. In layers:
  - **E9a The hosts.** Built (vg, 2026-10-23): DefaultApplication adds `GuiSubsystem` (`GUI()`),
    gives it the device, the resource manager (each place it gives Engine.UI its own) and each
    script context's surface, and calls `RenderTextures` before the scene beside Engine.UI's
    textures; the editor's scene and game pages call it too; the player points the GUI's text
    input at its window.
  - **E9b The project's settings.** The sheet list and the shared fonts applied by the player and the
    editor; the settings page and MCP's project settings. Built (vg, 2026-10-23): the manifest's
    appended `guiStyleSheetIds` (an asset list of `GuiStyleSheetAsset`, in the UI tab; the dialog,
    `project_info` and `project_settings_set` take it from its reflection, the dist copy and the
    asset walks reach it); `GuiSubsystem::SetFonts` (the project's default font and others, a
    resource font service, else the built-in font; the pass renderer and the baker draw with what
    measured); the player and the editor's project defaults give the GUI the UI's fonts and the
    sheets. The project-settings asset-list count follows (2).
  - **E9c Run screens.** A screen tier per run in the editor's Game tabs, the game's resolution and
    orientation. Built (vg, 2026-10-23): a screen tier is a root, its stack, its input and the
    game's screen it lays out for; the shared one holds the overlay layer. `SetScreenResolution`
    and `SetScreenOrientation` (the shared tier's, or a run's) place it as Engine.UI's
    `PlaceScreen` does: laid out at the resolution over its display scale, drawn into the fitted
    rectangle, turned through the pass view's new transform; its pointer, in render space, is taken
    to its pixels (a crop's offset off, at its scale). With `SetRunScreens` (the editor) each run's
    tier is made on first use (`ScreensFor`, `ScreenRootFor`), drawn under the shared tier for its
    tab (`SetRenderRun`), given the input of the scene it is bound to, and gone with
    `EndRunScreens`; the `gui` binding finds the calling script's run's root (the lab's binding
    gained `resolve`). DefaultApplication passes on the game's resolution and orientation; the
    editor turns run screens on; a Game tab sizes, draws and ends its run's tier. Fingers on a
    fitted tier come as the mouse SDL copies them to.
  - **E9d The GUI's loading document.** The boot splash through the GUI when the project names one.
    Built (vg, 2026-10-23): the manifest's appended `guiLoadingDocumentId` (a `GuiDocumentAsset`,
    Startup); `GuiSubsystem::PushOverlay` and `RemoveOverlay` (a document built, or a tree, on the
    overlay layer, modal while it shows); the player shows its boot splash there when the project
    names one, its load progress the GUI's data value `bootProgress` (0 to 1, which the document
    binds), else the UI's splash as before. The project settings' asset count follows (8).
- **E10 The editor's pages.** Document and sheet pages, previews, the inspector, the MCP guide.
  Built (vg, 2026-10-23): `Editor.GUI` (with the lab): `GuiAssetEditorPage` for a `.gml` document
  (its markup, XML-lexed, beside a live preview) and a `.css` sheet (its CSS beside preview markup
  of its own, kept on `GuiStyleSheetAsset` as editor-only `previewMarkup`, "Preview Document..."
  copying a document's in); the cook's diagnostics mark the lines, a document that does not build
  keeps the last good preview; Save writes the linked source and asks for the validating cook. The
  preview is the runtime's `GuiSubsystem::CreatePreview` (the GUI's own context, fonts, theme and
  the game's sheets; never in a tier, no input; a sheet over that tree alone) drawn by
  `RenderPreview` into the page's target. The inspector already names the assets ("GUI Document",
  "GUI Style Sheet", E3) and the components ("GUI Canvas", ..., E7); the MCP guide says how the
  generic asset tools make and import them. The pages are foundation.ui's, as the editor is, until
  its own move; CSS is plain text until the toolkit has a CSS lexer. Running the pages in the
  editor (over MCP, `editor_screenshot`) showed a document's text missing: the style applier gives
  text a font only where a family is declared, and nothing declared one in the engine's GUI (the
  sandbox's sheets and code set fonts themselves). The GUI's user agent sheet now declares the
  font service's default family at the body size on every tree's top elements (`:root`), which a
  game's sheets override; with it the pages show their text. Seen there too, for later: a lab
  Button with no width fills its line (the showcase gives every button one). After it (the user's
  choice, 2026-10-23): the XML parser keeps each element's and attribute's line (on master), and
  the markup loader's warnings name it, so an editor marks a dropped element or attribute on its
  own line and a cook's warning says where; CSS highlighting waits for the editor's move, whose
  code editor (GUI.Code) lexes CSS already.
- **E11 The web.** The lab and the engine's GUI on wasm. Built (vg, 2026-10-23): a web build no
  longer forces the experimental GUI off (the tooling group still is): the lab, its satellites,
  their suites, Engine.GUI and the web player build for wasm with no other change than the web
  player's entry importing the GUI's modules, as the desktop player's does. Not yet run in a
  browser: a web game showing the engine's GUI (its canvas, its WebGPU pass) is the check owed.

#### After section E (the user, 2026-10-23)

First the cleanup: the lab moves from `Code/Experimental` into Foundation, and the `lab` namespace
alias gives way to proper `gui` naming (before the samples move, so they are moved once, onto the
final names). Then the engine's sandbox and the native samples (`Code/Samples`) move onto
`Engine.GUI`, then the sample games. After that, the editor's move is planned properly before any
of it is built: a design for the whole editor, its sheet, its icons, its transitions, everything
(which also gives the mirror engine time to port the GUI).

The cleanup (built 2026-10-23, the user's rulings): the lab's libraries moved to
`Code/Foundation/GUI*` (modules `foundation.gui*`, namespace `foundation::gui`, CMake
`Foundation::GUI*`), its pipeline to `Code/Pipeline/GUI.Pipeline` (`gui.pipeline`, `Pipeline::GUI`)
and its sandbox to `Code/Samples/GUISandbox`; `OPTION_EXPERIMENTAL_GUI` and every guard went (the
GUI is built on every platform); code that qualifies foundation.ui (`ui::`) qualifies the GUI the
same way (`gui::`), the rest names its types plainly under `using namespace foundation::gui`; the
drag payload types became `gui/tab`, `gui/dock-surface` and `gui/dock-stack`. This record keeps
the old paths and names where it was written with them.
