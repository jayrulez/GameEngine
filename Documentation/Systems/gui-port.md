# GUI - foundation.gui, the retained-mode GUI

> Status: CURRENT
> Verified: 2026-10-23 (the move out of Experimental)
> Track: [[gui-port]]

`foundation.gui` is a retained-mode GUI built on the engine's own stack (VG for drawing, Fonts for
text, Core's reflection for scripts), designed concern by concern from a survey of eepp, RmlUi,
Godot, browsers and others rather than ported from one. It began as an experimental lab
(`Code/Experimental`, 2026-10) and moved into Foundation once the engine hosted it. It lives beside
foundation.ui (the Sedulous-derived framework, deprecated in `Code/Deprecated/UI` with the UISandbox); the games, the samples and
the editor are on it, and Engine.UI is gone (Specs/engine-ui-removal.md).

## Where it is

- `Code/Foundation/GUI` (`foundation.gui`): nodes and widgets, layout (flex, grid, stack), the CSS
  engine (selectors, cascade, variables, transitions and animations, media queries), markup
  (`.xml`), text (shaping, bidi, rich text), events and input, data binding, localization, the
  script surface (every widget reflected; the `gui` facade).
- Satellites, each `foundation.gui.<name>` in `Code/Foundation/GUI.<Name>`: `Shell` (the platform
  input bridge), `Hosting` (windows, roots, renderers; drawing into a caller's pass), `Toolkit`
  (editor widgets), `Code` (the code editor), `Docking`, `Viewport` (3D content in a node),
  `Gamekit` (a game's screens and prompts), `Inspector`, `Testing`, `VFS`, `Resources` (cooked
  documents and sheets). Each has a `.Tests` suite beside it; `GUI.Fuzz.Tests` fuzzes the parsers.
- `Code/Pipeline/GUI.Pipeline` (`gui.pipeline`): `.xml` documents and `.css` sheets as validated,
  linked-source assets.
- `Code/Engine/Engine.GUI` (`engine.gui`): the engine's GUI (screen and scene tiers, components,
  the resource provider by guid, the overlays, the script surface); `Code/Editor/Editor.GUI`: the
  editor's pages for the assets.
- `Code/Samples/GUISandbox`: the sandbox, a page per area, and the game menu showcase;
  `Code/Integration/Integration.GuiScript`: the script backends driving all of it.

## The design record

The design and its rulings are in the specs, in order: `Specs/vg-and-gui-lab.md`,
`Specs/gui-lab-evolution.md`, `Specs/gui-lab-evolution-2.md` and `Specs/gui-lab-evolution-3.md`
(section E: the engine's GUI, the move out of Experimental and what follows). They name the code
by its paths and modules at the time (`Code/Experimental`, `experimental.gui`). The early eepp
analysis is in `Documentation/Archive/gui-port-design-history.md`.
