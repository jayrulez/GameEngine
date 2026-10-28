# Viewport Input

> Status: CURRENT
> Verified: 2026-08-12 @ 7e4ebae7
> Track: [[viewport-input]]

One mechanism for routing input into a content surface placed in a window through a fit transform -
covering both the editor's embedded-game panel and the runtime fit mode with a single path. Shipped
(`foundation.shell`; the GUI's `foundation.gui.viewport::Viewport` owns an `InputSurface`; the old
`foundation.ui.viewport` was deleted 2026-10-28).

## The unified problem

The editor embedded game (game renders to an offscreen target shown in a panel; input restricted to the
panel + remapped into game-content space, with focus so keyboard/gamepad reach the game only when the
panel is active) and the runtime fit mode (game/UI rendered at a design resolution, placed into the
window with a letterbox/stretch fit; pointer remapped through the fit, bars not hitting content) are the
SAME problem: a content surface at some region of a window, through a fit transform, that owns
focus/capture and remaps input into content space. Sedulous built two converging paths
(`GameMouseAdapter` + `EngineCanvasMouseAdapter`); this is one.

## The layer

```
OS events
  -> SDL pump: tag each input event with windowID
  -> per-window device state + a per-window EVENT STREAM (the source of truth)
  -> InputRouter: focus / capture / hover ownership + dispatch order
  -> InputSurface: ContentFit transform + gate; maintains a polled snapshot
  -> consumers: poll surface->Mouse()/Keyboard()/... (drop-in), or receive events (UI)
```

- **Per-window tagging + focus** (`foundation.shell`): the SDL pump routes each input event to
  per-window device state keyed by `windowID`; the manager exposes `HoverWindow()` (the mouse routing
  authority) and a focused-window authority (keyboard/gamepad target). Foundation for the editor's
  floating/detached panels. The raw untransformed manager stays for global input; surfaces layer on top.
- **Event-first with a poll snapshot** (`InputTypes.cppm`): events are the source of truth (a
  capture/target/bubble dispatch pipeline + hover enter/leave, for the retained-mode UI); the device
  interfaces are a SNAPSHOT the surface maintains FROM that stream, so poll and event views never
  disagree.
- **`InputSurface`** (`InputSurface.cppm`): a rectangular slice of a window (a `ContentFit`) that
  presents gated + transformed `IMouse` / `IKeyboard` / `IGamepad` / `ITouch` (`Mouse()` / `Keyboard()`
  / ...). Window-space positions map through the `ContentFit` into content-normalized `[0,1]`
  (`ContentMouse()`); letterbox bars return "no hit." A consumer (`FlyCamera`, an ImGui feed) polls
  `surface->Mouse()->X()` instead of the raw manager and is otherwise unchanged (drop-in), so there is
  no per-consumer viewport code. `SetFit` / `SetFitMode` retarget the region.
- **`InputRouter`**: owns focus/capture/hover
  and dispatch order, consulting `HoverWindow()` + the event stream.

## One fit function, one enum

Draw and hit-test both derive from ONE fit function (Sedulous's `ComputeContentRect` + `ScreenToTexture`
gem, kept), so letterbox bars are consistently non-interactive. There is ONE `FitMode` enum + one
`ContentFit` struct - not Sedulous's triplicated `ViewportFitMode` / `RuntimeGraphics.FitMode` / editor
`FitMode` kept aligned by matching ordinals. All input KINDS are gated by the surface (not Sedulous's
mouse-only handler), and there is a single coordinate pipeline (not the world-UI path polling raw
`mouse.X/Y` and disagreeing).

---

The Sedulous reference read (the gem kept + the smells NOT ported - fit-enum triplication, mouse-only
gating, two `IMouse` adapters, dead fit-aware infra, the bypassing world-UI path, scattered gating) is
in `Documentation/Archive/viewport-input-design-history.md`.
