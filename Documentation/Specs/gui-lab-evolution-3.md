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
| GUI.Toolkit | `experimental.gui.toolkit` | Editor widgets: tool and status bars, the property grid, colour, curves, gradients, the timeline, node graphs | UI.Toolkit less docking and the code editor | Designed (T0 to T7) |
| GUI.Code | `experimental.gui.code` | The code editor | UI.Toolkit's CodeEditView | Not this round (ruling T3) |
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
- **T2 Compound editors.** Vectors with mixed values per component; containers (add, remove,
  reorder); nested objects; default providers with revert and the changed marker; copying and
  pasting values.
- **T3 The reflected source.** Core reflection and its attributes (display name, range, enum,
  `visibleWhen`) as a property source; the sandbox edits a reflected object on the lab's undo stack.
- **T4 Colour.** An inline picker and a swatch editor on the engine's sRGB colour; HDR intensity in
  stops where a property has one; alpha; hex.
- **T5 Curves, gradients, the timeline.** On ruling 2's Core types, after that `master` work.
- **T6 The node graph.** A view over a graph source.
- **T7 The rest.** A breadcrumb bar; toasts, unless the gamekit's serve.
