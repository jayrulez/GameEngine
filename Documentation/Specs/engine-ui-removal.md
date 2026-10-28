# Removing Engine.UI

> Status: DONE (2026-10-28)

The editor and all six sample games are on the engine's GUI (Engine.GUI over foundation.gui), so
the old game UI tier, Engine.UI (`engine.ui`) and its script surface Engine.UI.Script
(`engine.ui.script`), can go. Engine.UI imports Engine.UI.Script, so the two go together. What used
them changes to keep compiling; nothing else is deleted in this work.

## The user's rulings (2026-10-28)

1. Delete Engine.UI and Engine.UI.Script; change what uses them so the build stays green.
2. The UISandbox sample stays (it is on foundation.ui, not Engine.UI).
3. Three features Engine.UI gives the player have no GUI version yet; each is ported to the GUI
   before Engine.UI goes: the in-game profiler overlay, the player's default loading screen, and
   the touch-controls guide.
4. UI.Pipeline, UI.Resource and the project settings `defaultUiThemeId` and `loadingDocumentId`
   wait for the next step. Then come UI.Gamekit and UI.Viewport, each in a commit of its own.

## What uses Engine.UI today

Surveyed 2026-10-28.
- **The libraries:** `Code/Engine/Engine.UI` (UISubsystem, about 4,200 lines), `Engine.UI.Script`
  (the `ui` facade and the UIView handles), and their two test suites.
- **Engine.DefaultApp:** the UISubsystem is a subsystem; the `ui` screen binding; the profiler
  overlay host (`ProfilerOverlayHost`, on Profiler.UI's ProfilerOverlay, F3,
  `--profile-overlay`, `run.setProfilerOverlay`); the screen resolution and orientation; the
  canvas textures. Each of these has a GUI line beside it already, except the overlay.
- **Engine.Composition:** `UiDomain()` and `UiScriptDomain()` in the domain list.
- **Engine.Player:** the UI's text input target, fonts and theme, and the splash. Without a GUI
  loading document (`guiLoadingDocumentId`) it falls back to an old UIDocument
  (`loadingDocumentId`) or the engine's own screen (`BuildDefaultLoadingScreen`: the logo, a
  bar, "Loading").
- **The editor:** Editor.App (the run screens, the theme and fonts in EditorSession), Editor.Scene
  (the canvas textures in the scene viewport) and Editor.Play. The Game tab asks the old UI
  whether the game wants text input; that question goes to the GUI's focus instead.
- **Tests:** Engine.Script.Tests (two world-space UI component cases),
  Integration.ScriptFacades (the `ui` facade suite), Engine.DefaultApp.Tests,
  Engine.Composition.Tests (the domain and factory counts), AngelScriptCookTests (a script calling
  `ui::clear`), McpProjectFlowTests (the `ui` script name).
- **Nothing in the samples:** no scene holds a UICanvas, UIBillboard or UIWorldPanel; no script
  calls `ui`; every project's old UI settings are nil. No MCP tool is the old UI's.

## The layers

Each layer is one commit with its tests; the build stays green after each.

1. **The profiler overlay on the GUI.** Profiler.GUI gets a `ProfilerOverlay` widget with the old
   one's content: the frame time graph, the frame statistics, the top six CPU scopes, the top five
   GPU passes and the counters, on one translucent panel in a chosen corner. It takes no pointer,
   so the game under it plays on. Its look is Profiler.GUI's sheet. The host in Engine.DefaultApp
   shows it on the GuiSubsystem's overlay layer, with F3, `--profile-overlay` and
   `run.setProfilerOverlay` unchanged, and stops using Profiler.UI.
2. **The default loading screen on the GUI.** The player's splash, when the project names no GUI
   loading document, is the engine's own built on the GUI: the embedded logo, a bar following the
   load, and "Loading" under it, on the dark background the old one had. The old UIDocument
   fallback goes.
3. **The touch-controls guide on the GUI.** Once a finger touches the screen, the GuiSubsystem
   draws every touch binding of the sets in effect: a button as a circle filling its zone, with its
   action's name; an idle stick as a small ring low in its zone; a held stick as a ring where the
   finger came down and a knob where it is. It is the old guide's drawing, on the GUI, and takes no
   pointer.
4. **The rest off Engine.UI:** the Game tab's text input from the GUI; the editor (Editor.App,
   Editor.Scene, Editor.Play) and the player without the UISubsystem; DefaultApp without it; the
   tests that used the old facade retargeted to `gui`.
5. **The deletion:** Engine.UI and Engine.UI.Script out of the composition, their libraries and
   tests deleted, the counts in the composition tests, the allocator allowlist, and the documents
   (game-ui.md archived; the other systems docs, the guides, GETTING-STARTED and README brought in
   step).

After this, UI.Gamekit and Profiler.UI have no users left; UI.Gamekit goes next (ruling 4).
