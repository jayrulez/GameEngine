# Runs: a type for one running game

Status: PROPOSED, in revision. Written on the Sedulous side 2026-10-16 as "Run context"; adopted
here 2026-10-30 and revised against this engine's code (the scene layer stays off the runtime
layer, subsystems keep their own per-run state, the subsystem manager's rename split out to
`Specs/subsystem-manager-rename.md` and done; the run type's name is open, `run::Run` its working
name). Led here; Sedulous mirrors it through its port queue. Needs more thought before approval
(user 2026-10-30); open questions at the end.

## The problem

A **run** is one running game: a player's one game, each editor Game tab's game, and an
in-editor dedicated server. Today it has no type of its own.

- `GameInstance` (`Code/Engine/Engine.GameInstance/GameInstance.cppm`) is the run in all but
  name: it owns the run's scene group and event bus, its networking, input, save, script host,
  its time-scale term and its TLS and HTTP, and scripts reach it as `run`. It is an engine type,
  so the foundation cannot name it.
- The foundation sees a run as an opaque key: `Scene` holds `const void* m_run`
  (`Code/Foundation/Scene/Scene.cppm:480`), set by `SceneManager::SetSceneRun`
  (`SceneManager.cppm:62`); the script binding carries the same key
  (`ScriptBinding::run`, `Code/Foundation/Script.Facades/ScriptFacades.cppm:105`), which is how
  the app-wide `Audio` facade finds the run it plays into.
- Two application-wide subsystems keep per-run state in private tables keyed by that pointer:
  `AudioSubsystem` (`RunGroupFor`, `EndRun`, `SetRunPaused`, `SetFocusedRun`,
  `Code/Engine/Engine.Audio/AudioSubsystem.cppm:904-975`) and `GuiSubsystem` (a screen tier per
  run: `ScreensFor`, `EndRunScreens`, `SetRenderRun`,
  `Code/Engine/Engine.GUI/GuiSubsystem.cppm:500-508`). `NetworkSubsystem` and `InputSubsystem`
  reach every run through visitors the application hands them (`SetEndpointSource`,
  `SetActionRuntimeProvider`, `DefaultApplicationImpl.cpp:229, 248`).
- Nothing announces a run's lifecycle, so every host relays it to every subsystem by hand, and
  the hosts disagree. The Game tab's Stop ends the run's audio and its screens
  (`Code/Editor/Editor.Play/GamePageImpl.cpp:606, 611`); `DefaultApplication::ReleaseInstance`
  ends only its audio (`DefaultApplicationImpl.cpp:447`). Its one caller today, the Game tab's
  close, calls Stop first (`GamePageImpl.cpp:900`), so nothing leaks yet; a host that released
  an instance without that Stop would leave its screen tier in the GUI's table. Pause and focus
  are one call site, in the Game tab, for audio only (`GamePageImpl.cpp:531, 700, 704`). A new
  per-run subsystem, or a new host, means finding every relay.
- The key is compared by address. A host that skips an end call leaves a dead address in a
  table, which a later run allocated at the same address would inherit.

Nothing is broken today: the payoff is the next per-run subsystem, the next host (a dedicated
server launch, an MCP-driven run), and an online service (the flagship's Steamworks layer) that
needs a run to attach to.

## The model

Three tiers, each with a type:

| Tier | Type | One per | Holds |
|---|---|---|---|
| Application | `runtime::SubsystemManager` (renamed from `Context` 2026-10-30, `Specs/subsystem-manager-rename.md`) | process, or an embedded runtime | the subsystems; the application time scale and fixed step |
| Run | `run::Run` (new; working name, open question 1) | running game | the run's identity, its lifecycle, and the services the run owns |
| Scene | `scene::Scene` | scene | entities, systems, the scene's own time scale |

The time model reads the same down the tiers: the host's delta x the application's scale x the
run's x the scene's.

## Decisions

D1. **The run type lives in `foundation::run`; its name is open (open question 1).** This spec
    writes `run::Run`, a working name. The Sedulous draft called it `Context`, and renamed the
    subsystem manager first to free the word; that rename is done (`runtime::SubsystemManager`),
    so `run::Context` is available again.

D2. **Two modules, so the scene layer stays off the runtime layer.** `foundation.scene` depends
    on Core, Resource and Messaging only.
    - `foundation.run` (`Code/Foundation/Run/`): `Run` and `IRunService`. Depends on
      `foundation.core` only. `Scene` gains this dependency and holds a `run::Run*` in place of
      `const void*`.
    - `foundation.run.registry` (`Code/Foundation/Run.Registry/`): `Runs : runtime::Subsystem`,
      the registry of live runs (D7). Depends on `foundation.run` and `foundation.runtime`.

D3. **A run is not a scheduler.** It holds services and runs their lifecycle hooks; it does not
    tick them. Its owner (`GameInstance`) keeps ticking what it ticks today, in the order it does
    today. Two schedulers would fight over order.

D4. **A run holds the services it owns, keyed by the stable type id** (the `TypeId` the
    subsystem manager keys by, for the library-boundary reason its comment gives), one per type:
    `Add<T>(args...)` constructs and owns (the owner's allocator), `Register<T>(T&)` borrows.
    `Get<T>()`, `Has<T>()`, `Remove<T>()`. Owned services are destroyed in REVERSE order of
    registration. These are the run's own things (D9), not subsystems' bookkeeping.

D5. **A run has sessions.** A Game tab keeps its `GameInstance` from tab open to tab close and
    plays many times in it (`ReleaseInstance` only at page close, `GamePageImpl.cpp:920`): its
    script host, debugger wiring and save survive a Stop. So a run lives as long as its owner,
    and plays are sessions inside it:

    `Idle -> Running <-> Paused -> Stopping -> Idle ... -> Disposed`

    - `Start()`, `SetPaused(bool)` (idempotent: the Game tab recomputes pause every frame from
      the toolbar and the debugger), `Stop()`, and disposal (which stops a running session
      first, then destroys the owned services).
    - Services the run owns may implement `IRunService` (`OnRunPaused`, `OnRunStopping`), called
      in reverse order; services added with `AddForSession<T>` are removed at Stop.
    - A player's one run has one session; nothing in the player changes shape.

D6. **Pause is the host's pause**: the editor toolbar and a debugger break. A game's own pause
    (`run.timeScale = 0`) is the game's business and is not a run pause: its music plays on, as
    `AudioSubsystem::SetRunPaused` documents today.

D7. **The registry of live runs, `Runs : Subsystem`**, creates and destroys runs
    (`Create(name)`, `Destroy(run)`), lists them, holds the **focused** run (the one the user is
    playing; null is no preference), and raises each run's lifecycle as events carrying the run
    and its id: `Created`, `Started`, `Paused(bool)`, `Stopping`, `Destroyed`, `FocusChanged`.
    `NetworkSubsystem` and `InputSubsystem` enumerate it instead of taking a visitor from the
    application. It lives in the subsystem manager that holds the game: the player's, and in the
    editor the EMBEDDED runtime's (where `DefaultApplication` and its GameInstances live), not
    the editor's outer one.

D8. **Subsystems keep their own per-run state, keyed by the run's id, and follow the events.**
    Audio's run groups and the GUI's screen tiers stay in their subsystems, which own them and
    the resources they hold; they create them on first use as now, end them on `Stopping`, pause
    on `Paused`, follow `FocusChanged`. No host relays anything. (The Sedulous draft moved this
    state onto the run as services; that hands a subsystem's objects to a lower tier, and its own
    first gotcha was the cost: a GUI tier owned by the run must still go before the GUI shuts
    down.)

D9. **The run's own services.** `GameInstance` stays in the engine and becomes the run's owner,
    not the run: it holds its `run::Run` and registers on it what the run owns, itself
    (`GameInstance`), its `NetworkController`, `ActionRuntime`, `RunSave`, `ScriptRunHost`, its
    `HttpScriptBinding` and TLS backend. Code holding a run asks `run.Get<GameInstance>()` for the
    orchestrator, and `NetworkSubsystem` reaches an endpoint through `Get<NetworkController>()`. A
    future online service is one more registration, by whoever owns it.

D10. **Each run has an id that is never reused** in the process (a counter), and a name
     ("Game 2", "Server"). Tables that must outlive a run (audio's ending groups, logs, MCP
     answers) key by the id, never by address.

D11. **Null stays "outside every run".** An editor page's scene and a test's scene have no run.
     They have no network, input map or save to hold.

D12. **The script surface does not change.** `run`, `Input`, `Net`, `Save`, `Http`, `Audio` and
     `gui` read the same to a script; `ScriptBinding::run` becomes a `run::Run*`. Whether the run
     host should install the run's script services from the run itself is P4, optional (see
     there).

## Phases

### P1: `foundation.run` and `foundation.run.registry`

The modules as D2-D7 and D10, each with its own `.Tests`. Nothing in the engine uses them yet.

- Tests: add/register/get/remove by type; owned services destroyed in reverse order, borrowed
  ones not; a type registered twice replaces (and destroys an owned predecessor); session
  services go at Stop and session-long ones stay; `OnRunStopping` and `OnRunPaused` reach the
  services that implement them, in reverse order; `SetPaused` repeated raises once; disposal
  while running stops first; ids never reused across destroy and create; the registry raises
  each event once, in order, and clears the focus when the focused run is destroyed. ASAN
  clean.

### P2: scenes and `GameInstance` carry a run

- `Scene::Run()` becomes `run::Run*`; `SceneManager::SetSceneRun` takes one;
  `ScriptBinding::run` likewise.
- `DefaultApplication::CreateInstance` creates the run through `Runs`, then the `GameInstance`
  over it (D9); `ReleaseInstance` destroys both. The primary instance likewise.
- The Game tab's Play starts the run's session, its pause calls `SetPaused`, its Stop calls
  `Stop`, focusing its viewport sets the registry's focus.
- Tests: `GameInstanceTests` (created and adopted scenes carry the instance's run,
  `GameInstanceTests.cpp:139-148`); a released instance's run is gone from `Runs`.

### P3: subsystems follow the registry

- `AudioSubsystem`: run groups keyed by run id, ended on `Stopping` (faded), paused on
  `Paused`, the focus from `FocusChanged`. `EndRun`, `SetRunPaused` and `SetFocusedRun` leave
  its public surface.
- `GuiSubsystem`: screen tiers keyed by run id, ended on `Stopping`; `EndRunScreens` goes;
  `ScreensFor` and `SetRenderRun` take a `run::Run*`.
- `NetworkSubsystem` and `InputSubsystem` enumerate `Runs`; `DefaultApplication`'s endpoint
  source and action-runtime provider go.
- Both hosts' relays go: `ReleaseInstance` and the Game tab make one call each.
- Tests: `RunAudioTests` and the GUI subsystem's run-tier tests over runs, same assertions; a
  run stopped WITHOUT any audio or GUI call leaves no run group or screen tier behind; a
  released run's id is not reused by the next.
- Acceptance: no `const void* run` remains (`grep -rn "const void\* run" Code` is empty).

### P4 (optional): the run host reads the run

Today the run installs its script services on each script context through a callback
(`ScriptRunHost::SetOwnerServices`, `GameInstance::InstallRunServices`), each under a string key
(`run.runtime`, `run.save`, `net.runtime`, `http.runtime`). P4 would have the run host install
them from the run: a service type that scripts reach declares its key
(`static constexpr StringView kScriptService`), and the host installs every registered service
that has one. App-wide services (`audio.runtime`, `render.runtime`) stay as they are, so the run
would not be the one list, only the run's part of it. Worth it only if the callback becomes a
burden.

## Gotchas

- **Teardown order.** `Runs` sits early in `UpdateOrder`, so it shuts down after the subsystems
  that subscribe to it; a subsystem unsubscribes in its own shutdown. Runs still live at
  shutdown are destroyed by their owners (the application's instances) before the registry.
- **Audio's fade outlives the session.** A stopped session's group keeps fading for
  `fadeSeconds`; the reap list holds the group handle, not the run.
- **The editor stops without disposing.** Tests must cover Stop then Start again on one run:
  per-session services come back fresh, session-long ones are the same objects, and the
  subsystems' per-run state starts over.
- **The allocator tripwire.** `Runs` and each run take their owner's allocator; nothing new
  reaches `DefaultAllocator()` outside the allowlist.

## Open questions

1. **The run type's name (D1): `run::Run` or `run::Context`.**
   - `run::Run`: scripts already call it `run`, the code calls a GameInstance "the run" and scenes
     hold `Run()`; it reads as `scene::Scene` does. In the code that uses it most, "context"
     still means the script context (`IScriptContext`; GameInstance names it eleven times,
     `InstallRunServices(context)` takes one), and a second kind of context there is easy to
     misread.
   - `run::Context`: the word is free since the subsystem manager's rename, it is the Sedulous
     draft's name (both engines would agree without a port note), and "the run's context" says
     what the type is: what a run's code reaches through it.
   - Whichever is chosen names the module's events and hooks too (`IRunService` or
     `IContextService`, `Runs` or `Contexts`).
2. **Events or services for subsystem state (D8).** Events keep ownership with the subsystem
   and avoid cross-tier teardown; services would give a scene's run direct access to its GUI
   tier (`scene.Run()->Get<GuiRunTier>()`) without a table lookup. Proposed: events.
3. **When.** Nothing is broken; the change crosses the scene layer, audio, the GUI, the default
   application and the Game tab. Proposed: after 0.1, unless the online layer for the flagship
   comes first, in which case P1 and P2 come before it.
4. **The run event bus** (`GameInstance::m_runEvents`): it could become a run service later;
   scenes borrow it through `SetEventBus` today and that works. Out of scope for now.

## Not in scope

- The subsystem manager's name: settled and renamed separately (`Specs/subsystem-manager-rename.md`).
- Moving the application time scale off the subsystem manager.
- A run for editor pages or tests (D11).
- The online service itself; the registry is what it will plug into.
