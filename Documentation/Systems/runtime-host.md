# Runtime Host

> Status: CURRENT
> Verified: 2026-08-12 @ 42273d13
> Track: [[runtime-and-engine-buildout]] / [[runtime-host-v3]]

The host layer: a single `IApplication` IS the app/game; a generic `ApplicationHost` drives it over a
shared GPU device + N windows; the editor embeds the SAME application against a persistent runtime
context. Shipped (multi-window platform + graphics host + the inverted single-application model + the
embedded host). Run ownership (scenes + script brain) lives one layer up in `GameInstance` - see
[[game-instance-track]].

## The inversion

`IApplication` (`foundation.runtime.client`, `Application.cppm`) IS the app - exactly ONE per host. It
owns subsystem registration in `Configure(host)` (the host forces nothing in), so the subsystem set is
the app's alone and identical standalone or embedded. Hooks: `Settings`, `Configure`, `OnStartup`,
`OnLaunch`, `OnUpdate`, `OnFixedUpdate`, `OnRenderWindow`, `OnExit`, `OnShutdown`. This collapsed the
old EngineApplication-vs-client split and the composable-module-list model (there is no
`IApplicationModule`; behavior lives in the one `IApplication`).

`DefaultApplication` (`engine.defaultapp`) is the conventional `IApplication`: it registers all the
standard engine subsystems and owns `Array<GameInstance>`. The player is a thin subclass; the editor
embeds the same class. A game `: DefaultApplication` gets the defaults + its own; a game
`: IApplication` gets only what it declares.

## `ApplicationHost` + windows + graphics

`ApplicationHost` (`foundation.runtime.client`, `ApplicationHost.cppm`) is the concrete, generic host
that drives exactly one `IApplication`. It owns a `Context`, a borrowed optional `GraphicsDevice`, and
the LIST of `RenderWindow`s it presents. It is infrastructure - NOT subclassed - and LOOP-AGNOSTIC:
the shell layer drives `Start`/`Tick`/`Stop` (a blocking loop on desktop via `RunApplication` in
`foundation.runtime.desktop`; a callback on Emscripten via `foundation.runtime.web`), so the host holds
no run loop. It implements `IApplicationHost`, the view the application gets (`Ctx`, `Graphics`,
window open/close, exit). `Start` also owns the engine-wide `JobSystem`'s lifetime
(`InitGlobalJobSystem` before `Configure`, `ShutdownGlobalJobSystem` last in `Stop`), so every
`ResourceManager` built during startup with `HasGlobalJobSystem() ? &GlobalJobs() : nullptr` gets
the workers. A test that wants them makes its own `JobSystem`.

Multi-window is uniform: the main window is `windows[0]`; every frame renders the whole list;
`OpenWindow`/`CloseWindow` run at runtime (the basis for detachable UI windows) with close deferred to
frame end. The GPU layer is three modules to keep the core host backend-agnostic and dodge a GCC
modules bug (importing a backend module into the core interface breaks GCC's reader):

- **`foundation.graphics`** - `GraphicsDevice` (shared: backend/adapter/device/queue + the
  frame-in-flight ring), `RenderWindow` (one per OS window: surface + swapchain + per-window present
  sync), `FrameContext` (the per-window per-frame host/consumer boundary).
- **`foundation.graphics.gpu`** - `CreateGraphicsDevice` (Vulkan/DX12).
- **`foundation.graphics.null`** - `CreateNullGraphicsDevice` (headless).

Headless stays first-class: no `GraphicsDevice` => no windows => render skipped (null-platform tests
untouched).

## Subsystems + per-window render

`Subsystem` (`foundation.runtime`) has the frame phases `BeginFrame` / `FixedUpdate` / `Update` /
`PostUpdate` / `EndFrame` plus lifecycle (`OnInit` / `OnReady` / `OnRegister` / `OnUnregister` /
`OnPrepareShutdown` / `OnShutdown`). There is NO `Subsystem::Render` phase - the original design
proposed one but it was never added; per-window rendering is driven by `IApplication::OnRenderWindow`
(given a `FrameContext` per window per frame), and rendering subsystems draw through that path.

## Screenshots (DefaultApplication, 2026-09-19)

`DefaultApplication::CaptureScreenshot(path)` writes the next presented frame of the main window
as a PNG. Legacy Sedulous had the request and the GPU copy and stopped there (the readback was
never mapped); Raptor's `ScreenshotCapture` (`engine.defaultapp:screenshot`) is the whole path:
armed by the request, `FinishFrame` records the copy off the backbuffer inside the frame
(RenderTarget -> CopySrc -> RenderTarget, so the host's own Present transition still holds) into a
256-byte-pitch GpuToCpu buffer; the next `OnUpdate` waits the device idle (a one-off hitch),
unpacks the rows into RGBA8 (BGRA surfaces swizzled), writes the file through
`foundation.image.io`, and logs `Screenshot: wrote '<path>' (WxH)`. Non-8-bit surfaces are refused
with a log line, never a silent no-op. Three ways in:
- **F11** in any DefaultApplication: `screenshot_<ticks>.png` in the working directory (the legacy
  sandbox binding, now on the base app so the player has it too).
- **`--screenshot <png> [--screenshot-frame N | --screenshot-after S] [--screenshot-count N]
  [--screenshot-exit]`** (`ScreenshotOptionsFromArguments`, read by `OnCommandLine` for every app):
  capture at rendered frame N (default 30) or at the first frame past S seconds (frame-rate
  independent), and exit once the file is written when asked - a screenshot with no hand on the
  keyboard and no desktop capture tool (Wayland has none an unprivileged process may use). With a
  count, that many consecutive frames from there, as `<png stem>-<i>.png` (frame-to-frame change:
  TAA's jitter in an exported build), exiting after the last. Exit code 1 when the capture could
  not be produced.
- The base `OnRenderWindow` is `RenderFrame` (the scenes + window overlays) then `FinishFrame`. A
  subclass that draws its own overlay (ImGui, a HUD - every sample does) overrides it and calls
  the two around the overlay, so the overlay is in the shot; the Sandbox, which renders offscreen
  and blits, calls `FinishFrame` after its blit and ImGui.
- `IApplication::OnCommandLine(argc, argv)` is how the flags reach an app: the desktop `APP_MAIN`
  calls it before running, a hand-written main calls it itself (Sandbox, the player, the samples
  with their own entry). DefaultApplication's override reads the `--screenshot` flags; web entries
  have no argv and never call it.
The WebGPU swapchain now asks for `CopySrc` when the surface offers it (the Vulkan surface already
carried TRANSFER_SRC where the driver allows). The player's `--exit-after <seconds>` was parsed and
never applied; it is wired to `SetExitAfterSeconds` now. Tests: `Engine.DefaultApp.Tests/
ScreenshotTests` (flags, the row unpack + swizzle, and an RGBA8 + BGRA8 clear captured, written
and loaded back on Vulkan + WebGPU).

## Embedded host (the editor)

The editor holds EXACTLY TWO contexts, forever: the editor-app context (chrome/tools/UIHost) and ONE
persistent embedded runtime `Context` owning all scene hosting (editing scenes, Simulate, preview
pages, and the Game tab's runs) plus every engine + game subsystem. `EmbeddedApplicationHost`
(`EmbeddedHost.cppm`) is the adapter: it routes `Ctx()` to the embedded runtime context (distinct from
the outer editor-app context) while SHARING the outer host's shell and the REAL `GraphicsDevice` (a
deliberate deviation from Sedulous's Graphics=null, which forced re-pointing subsystem device/window
after Configure). The embedded app renders into viewport textures, so `MainRenderWindow` is null and
window open/close are refused; "exit" stops the play session.

This is what lets a native game module `Configure()` ONCE and have its managers inject into the same
`SceneSubsystem` that hosts editing scenes - custom components are inspectable while editing and
playable in the Game tab, identical to standalone.

## Time + run ownership

Time is layered `host dt x context x group/instance x scene`, with the group/instance + fixed-step
accumulator owned by each `SceneManager` (see [[game-instance-track]] - the SceneManager model
superseded this doc's original per-scene-time framing). Run ownership - the script brain, the scenes,
per-instance net/input - moved up to `GameInstance`; `ApplicationHost` and the subsystems hold no run
state.

---

Design history (the original v1/v2 module-list model, the Sedulous wart table it corrected, the
GraphicsDevice/RenderWindow/FrameContext API sketches, and the v3 embedded-host phasing) is in
`Documentation/Archive/runtime-host-design-history.md`.
