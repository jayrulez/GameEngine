# Run screens - each Game tab's screens are its own

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

## The problem

The editor runs a game per Game tab inside one embedded application, and that application had one
UI screen tier: every run's `ui::push` (menus, HUD, pause) landed on the same stack, and every tab
composited it over its own scene. With two tabs side by side (user, 2026-10-04): "Input is only
going to the last one and appearing in both tabs. So I press down and both instances title menu
respond. but clicking in the first instance does not respond." The 3D scenes were per tab; the
screens were shared, and the game UI's input source and scene binding belonged to whichever tab
had started last.

## As built

- **A screen tier per run** (`UISubsystem`): a tier is a scene-less root, its `ScreenStack`, and the
  game resolution it lays out at. The shared tier stays (the player, C++ callers, the global
  overlay layer). With `SetRunScreens(true)` (the editor turns it on at startup, beside the input's
  unbound-scene policy) `ScreensFor(run)` gives each run key (a `GameInstance`, as `Scene::Run` and
  a script context's run carry) its own tier, made on first use; `EndRunScreens(run)` drops it.
  Off, every run uses the shared tier, so the player is unchanged.
- **Scripts reach their own run's screens**: the `ui` facade asks the binding's `stackForRun` with
  the calling script's run (`CurrentRun()`); the default application answers it with `ScreensFor`.
- **A tab draws its own run's screens**: around its `RenderOverlays` the Game tab sets the render
  run (`SetRenderRun`) and its tier's resolution (`SetScreenResolution(run, ...)`); the shared
  tier's global overlays draw above.
- **Input reaches the bound run's screens**: the UI pump takes its screen tier from the run of the
  scene the input subsystem is bound to. A Game tab claims that binding each frame while the
  keyboard is in it (its surface is focused, as a click gives it) or while a playtest scripts it
  (`pie_run`); otherwise the binding stays with the tab that took it last, so a lone tab keeps it
  from Play on. Hovering alone does not claim: the keys would leave the tab being played.
- **A run's end takes its screens** (the Game tab's stop, beside its audio).

## Tests

- `Engine.UI.Tests`: run screens off share the one tier; on, each run has its own, the same run
  the same tier; input bound to a run's scene lands keyboard focus on that run's menu, not the
  other's; ending a run drops its tier and keeps the other's.
- `Integration.ScriptFacades`: two contexts in different runs `ui::push` onto their own run's
  stack and find their own controls; nothing lands on the shared tier.
- In the editor (over MCP): two Game tabs each draw their own title, a scripted Down moves only its
  tab's menu, one tab can enter a level while the other stays on its title, and stopping one leaves
  the other's screens. Real clicks across two tabs are the user's check.

## Not in scope

Hover-driven game UI (button hover highlights) in a tab without the keyboard; the player's split
screen (one run, one tier, as before).
