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

## The libraries

| Library | Module | What it is | foundation.ui's | State |
|---|---|---|---|---|
| GUI | `experimental.gui` | The core: nodes, layout, styling, text, input, widgets, scripting | UI | Rounds one and two |
| GUI.Shell | `experimental.gui.shell` | The platform input bridge | UI.Shell | Built |
| GUI.VFS | `experimental.gui.vfs` | A resource provider on the VFS | UI.VFS | Built |
| GUI.Inspector | `experimental.gui.inspector` | The inspector (a tool) | none | Built |
| GUI.Testing | `experimental.gui.testing` | Test helpers, image comparison (a tool) | none | Built |
| GUI.Hosting | `experimental.gui.hosting` | The host on `foundation.graphics`: one GUI, a root per window, each window's VG context, renderer, targets and input; the frame; offscreen pictures; the picture baker | UI.Runtime | H1 to H3 |
| GUI.Viewport | `experimental.gui.viewport` | A node showing 3D (or any GPU) content drawn into a target of its own, with input gated to it | UI.Viewport | V1, V2 |
| GUI.Docking | `experimental.gui.docking` | Docking: splits, tab groups, drop zones, floating panels in the window and as OS windows, layouts saved and restored; its default sheet | UI.Toolkit's docking and UI.Application | D, after its survey |
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

### H3 The sandbox on hosting

GUISandbox becomes an application on the runtime host (`runtime::RunApplication`), its window, its
GUI and its frame the host's: its pages, the inspector pane, the image tests, the bench, the sweep,
the soak and `--keys` unchanged in what they do and show. Screen: every sandbox page's reference
image unchanged.

### V1 The viewport

`ViewportView`, a node that shows content drawn into a colour target (and depth) of its own, sized
to the node in device pixels: an `OnRender` the application fills, recorded before the window's
pass, the target registered with the window's renderer as an external texture, so the node draws
it as an image. Its input is gated: a shell input surface over the node's area, so a 3D camera
reads the keyboard and mouse only while the node is hovered or focused, and a drag that leaves it
keeps going. It follows the window it is in (a node moved to another window's root re-registers).

### V2 The sandbox's viewport

A Viewport page: a spinning cube a camera flies around, the page's controls over it.

### D Docking

Designed from a survey of the reference UIs (Godot's docks, the Zero editor, ezEngine, Dear
ImGui's dock nodes through Lumix, traktor, zed's panes and docks, foundation.ui's own port and its
known faults), then built in phases, its sandbox page last: panels docked, tabbed, split, floated
in the window and as OS windows, a layout saved and restored.
