# The samples onto the engine GUI

The sample games and the native samples move from the old game UI (foundation.ui, Engine.UI,
Engine.UI.Script, UI.Pipeline: `.sml` UI documents, `.sss` UI themes, the `ui` script facade) onto
the engine GUI (foundation.gui, Engine.GUI, GUI.Pipeline: `.gml` documents, `.css` sheets, the
`gui` script facade). WebScene moved first (2026-10-23) and is the model. The editor's pages for the
GUI's assets (Documentation/Specs/editor-gui.md, 5.4, layer 6, part 5) and `gui_validate` come
before it, as the tools the ports are made and checked with.

## Rulings (user, 2026-10-26)

1. Scripts move to the new facade's own idioms: `Label.of(gui::root().find("score"))` where the old
   `ui::findLabel("score")` was, `node.on("click", fn)` for `onClick`, a tween for `fadeTo` and
   `moveTo`, `push("{guid}")` for a push by `Guid`. The facade gains only what has no way at all (a
   pulse), one way to do each thing, as `Component.of` is the one way to a component.
2. PhysicsPlayground (canvas, billboard, world panel) and GameUiSandbox (onto GUI.Gamekit) are
   ported; UISandbox, the old UI's own showcase, is retired, the GUISandbox covering the new GUI.
   Retiring it is a removal, done when the user says.
3. A game's old UI documents and theme (and their copies in its `Tools/`) are deleted in the same
   commit as its port, once the port has been played and approved: each game has one UI. The
   engine's old UI libraries stay until the user rules otherwise.
4. The games move smallest first: Shader Gallery (1 screen) to prove the path, then Masquerade,
   Snowline, Lamplight (its guard meter on a billboard), PaperKid (its theme, the minimap markers),
   and Sky Hopper last (10 screens, a theme, the most script).

## What there is (surveyed 2026-10-26)

| Game | Old documents | Theme | Old `ui` call sites | Notes |
|---|---|---|---|---|
| Shader Gallery | 1 (Gallery) | none | about 19 (Gallery.as) | its generator in `Tools/` |
| Masquerade | 4 (Hud, Title, Pause, Results) | none | about 56 (Masquerade.as) | a text box; its generator in `Tools/` |
| Snowline | 5 (Hud, Title, Pause, Finish, Ending) | none | about 68 (Snowline.as) | mostly finds with visibility; generator in `Tools/` |
| Lamplight | 5 (Hud, Title, Pause, Results, GuardMeter) | none | about 62 over 6 scripts | `ui.Billboard` in 6 scenes (11 uses); generator in `Tools/` |
| PaperKid | 7 (Hud, Title, Pause, Settings, Cleared, Failed, GameOver) | PaperKidTheme | about 62 over 3 scripts | minimap markers placed by script |
| Sky Hopper | 10 (Hud, Title, Intro, Pause, Settings, LevelClear, LevelComplete, GameOver, Victory, Fade) | GameTheme | about 87 (PlatformerGame.as) | transforms and sliders |

The old markup's dialect (`<screen mode= transition= default-focus=>`, `Frame`, `Flex direction=`,
`gravity=`, `rounded-rect(...)` backgrounds) is not the GUI's: each screen is written again as a
`.gml` document over a `.css` sheet, not renamed. A theme becomes a sheet in the project's
`guiStyleSheetIds`; a loading document, `guiLoadingDocumentId`. The fonts stay the project's UI
fonts, which the GUI shares.

The new facade covers what the games use (screens through `ScreenStack.of(...)`: push, replace,
pop, clear, back; nodes: find, visible, enabled, opacity, text, value, classes, events, tweens).
Gaps to settle as they are met: a pulse; a text colour set directly (a class instead); the old
rich text's runs and text box (Masquerade's one use).

## Phases

- P0. The tools: `gui_validate` (MCP), the editor's GUI document and sheet page with its live
  preview. A port then loops: write the `.gml` and `.css`, `gui_validate`, look at the page's
  preview with `editor_screenshot`, then play it.
- P1. The native samples: PhysicsPlayground, then GameUiSandbox onto GUI.Gamekit.
- P2 to P7. The games in the ruled order, one at a time, each on its own: its screens written
  again, its theme a sheet, its scripts on the new facade, its `Tools/` generator writing the new
  files, its project's settings, Integration.Mcp's read of it green; then the user plays it, and on
  approval its old UI assets go with the port's commit.
- P8. With every sample moved, the old UI's own sample (UISandbox) retired when the user says, and
  what else of the old UI may go put to the user.

Each phase lands with its tests; each game's port is checked in play-in-editor and in the exported
player (a different host), and its screens are handed to the user to look at.
