# Runs: a type for one running game

Status: PROPOSED, revised 2026-10-30 for the user's full review. Written on the Sedulous side
2026-10-16 as "Run context"; adopted here and led here, Sedulous mirroring it through its port
queue. Decided so far (user 2026-10-30): the name is `Run`; the run and its manager are interfaces
(`IRun`, which `GameInstance` implements; `IRunManager`, which `DefaultApplication` implements);
the runtime library knows the run library, so the subsystem manager holds the run manager; the
run owns its event bus. The subsystem manager's rename, this spec's first P0, was split out and
is done (`Specs/subsystem-manager-rename.md`). Making the script context's names explicit is
separate work, decided regardless of this spec (Related work, below).

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
  the app-wide `Audio` facade finds the run it plays into. The run's event bus reaches each scene
  the same way, separately: `SceneManager::SetSceneEventBus` (`SceneManager.cppm:60`) into a
  borrowed `Scene::m_eventBus`, null outside a run (`Scene.cppm:617`).
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
- A subsystem a game's native plugin registers (`IRuntimePlugin::OnLoad(SubsystemManager&)`,
  loaded into the editor's embedded runtime by `EditorNativeModule`, into the player's by the
  player) receives only the subsystem manager. Whatever lets subsystems find the runs must be
  reachable from it.

Nothing is broken today: the payoff is the next per-run subsystem, the next host (a dedicated
server launch, an MCP-driven run), and an online service (the flagship's Steamworks layer) that
needs a run to attach to.

## The model

Three tiers:

| Tier | Type | One per | Holds |
|---|---|---|---|
| Application | `runtime::SubsystemManager` | process, or an embedded runtime | the subsystems; the application time scale and fixed step; the run manager |
| Run | `run::IRun` (implemented by `GameInstance`) | running game | its identity, its lifecycle, its event bus, the services it owns |
| Scene | `scene::Scene` | scene | entities, systems, the scene's own time scale |

The run manager (`run::IRunManager`, implemented by `DefaultApplication`) lists the live runs,
holds the focused one and announces each run's lifecycle. The time model reads the same down the
tiers: the host's delta x the application's scale x the run's x the scene's.

## Decisions

D1. **The names.** Namespace `foundation::run`, module `foundation.run`
    (`Code/Foundation/Run/`): `IRun`, `IRunManager`, `IRunService`, `IRunListener`, and the two
    helpers of D4 and D7. (The Sedulous draft's `Context` was considered and set aside: in the
    code that uses the run most, "context" means the script context.)

D2. **Layering.** `foundation.run` depends on `foundation.core` and `foundation.messaging` (the
    bus, D12) and on nothing else; in particular it never imports `foundation.runtime` or
    `foundation.scene`, which both depend on it:

    ```
    core <- messaging <- run <- runtime   (the subsystem manager holds the run manager)
                            <- scene     (a scene holds its run)
    engine (GameInstance, DefaultApplication, the subsystems) <- all of them
    ```

    The runtime gains a dependency it has not had (Core and Policy today), and through it
    Messaging, a one-module leaf over Core that nearly every runtime user already has through
    Scene. Breaking the rule (the run library importing the runtime or scenes) would make a cycle.

D3. **A run is not a scheduler.** It holds services and runs their lifecycle hooks; it does not
    tick them. Its implementor (`GameInstance`) keeps ticking what it ticks today, in the order it
    does today. Two schedulers would fight over order.

D4. **`IRun`**, the interface a run is reached through, and **`RunBase`**, the helper that
    implements it once:

    ```cpp
    class IRun
    {
    public:
        virtual ~IRun() = default;
        [[nodiscard]] virtual RunId Id() const noexcept = 0;     // never reused (D10)
        [[nodiscard]] virtual StringView Name() const noexcept = 0;
        [[nodiscard]] virtual RunState State() const noexcept = 0; // D5
        [[nodiscard]] virtual messaging::EventBus& Events() noexcept = 0; // D12
        // The services the run owns (D9), one per type, keyed by the stable type id:
        template <typename T> T* Get() noexcept;                  // over a virtual Find(TypeId)
        template <typename T> bool Has() const noexcept;
    };
    ```

    `RunBase : IRun` holds the id, name, state machine, bus and service table, with
    `Add<T>(args...)` (constructs and owns, the run's allocator), `Register<T>(T&)` (borrows),
    `Remove<T>()`; owned services are destroyed in REVERSE order of registration; a type
    registered twice replaces (destroying an owned predecessor). `GameInstance` derives from it.
    The interface stays for a run that is not a GameInstance (a test's, a future server's), which
    reuses `RunBase` the same way.

D5. **A run has sessions.** A Game tab keeps its `GameInstance` from tab open to tab close and
    plays many times in it (`ReleaseInstance` only at page close, `GamePageImpl.cpp:920`): its
    script host, debugger wiring and save survive a Stop. So a run lives as long as its owner, and
    plays are sessions inside it:

    `Idle -> Running <-> Paused -> Stopping -> Idle ... -> Disposed`

    - `RunBase` has `Start()`, `SetPaused(bool)` (idempotent: the Game tab recomputes pause every
      frame from the toolbar and the debugger), `Stop()`, and disposal (stopping a running
      session first, then destroying the owned services).
    - A service the run owns may implement `IRunService` (`OnRunPaused(IRun&, bool)`,
      `OnRunStopping(IRun&)`), called in reverse order of registration; services added with
      `AddForSession<T>` are removed at Stop.
    - Each transition is announced through the run's manager (D7).
    - A player's one run has one session; nothing in the player changes shape.

D6. **Pause is the host's pause**: the editor toolbar and a debugger break. A game's own pause
    (`run.timeScale = 0`) is the game's business and is not a run pause: its music plays on, as
    `AudioSubsystem::SetRunPaused` documents today.

D7. **`IRunManager`**, implemented by `DefaultApplication` (which already creates, releases and
    enumerates the instances: `CreateInstance`, `ReleaseInstance`, `ForEachInstance`), and
    **`RunRegistry`**, the helper that implements its bookkeeping once:

    ```cpp
    class IRunListener
    {
    public:
        virtual ~IRunListener() = default;
        virtual void OnRunCreated(IRun&) {}
        virtual void OnRunStarted(IRun&) {}
        virtual void OnRunPaused(IRun&, bool paused) {}
        virtual void OnRunStopping(IRun&) {}
        virtual void OnRunDestroyed(IRun&) {}
        virtual void OnFocusChanged(IRun* focused) {}
    };

    class IRunManager
    {
    public:
        virtual ~IRunManager() = default;
        virtual void ForEachRun(const Function<void(IRun&)>& visit) = 0;
        [[nodiscard]] virtual IRun* Find(RunId id) noexcept = 0;
        [[nodiscard]] virtual IRun* Focused() noexcept = 0; // null: no preference
        virtual void SetFocused(IRun* run) = 0;
        virtual void Subscribe(IRunListener& listener) = 0;
        virtual void Unsubscribe(IRunListener& listener) = 0;
    };
    ```

    - Events go to listeners in subscription order, once per transition; destroying the focused
      run clears the focus (raising `OnFocusChanged(nullptr)`).
    - **Where subsystems find it:** `SubsystemManager::SetRunManager(run::IRunManager*)` and
      `RunManager()`. `DefaultApplication` sets it as it configures and clears it in its
      `OnShutdown`. Every subsystem reaches it through `Manager()->RunManager()`, an engine
      subsystem or one a game's plugin registered, whenever it was registered.
    - **A late subscriber catches up:** a subsystem subscribing after runs exist (a plugin's,
      a hot reload's) lists them with `ForEachRun` and treats each as created (and started, if
      running).
    - A subsystem manager with no game has no run manager (`RunManager()` null): the editor's
      outer runtime, a tool, a test. Subsystems handle null as "no runs". In the editor the run
      manager is on the EMBEDDED runtime, where `DefaultApplication` and its instances live.

D8. **Subsystems keep their own per-run state, keyed by the run's id, and follow the events.**
    Audio's run groups and the GUI's screen tiers stay in their subsystems, which own them and
    the resources they hold; they create them on first use as now, end them on `OnRunStopping`,
    pause on `OnRunPaused`, follow `OnFocusChanged`. `NetworkSubsystem` and `InputSubsystem`
    enumerate the runs (`ForEachRun`) instead of taking visitors from the application. No host
    relays anything. (The Sedulous draft moved this state onto the run as services; that hands a
    subsystem's objects to a lower tier, and its teardown then has to cross tiers.)

D9. **The run's own services.** `GameInstance`, being the run, registers on itself what it owns
    and others reach through the run: its `NetworkController`, `ActionRuntime`, `RunSave`,
    `ScriptRunHost`, `HttpScriptBinding` and TLS backend. `NetworkSubsystem` reaches an endpoint
    through `run.Get<NetworkController>()`, `InputSubsystem` an action runtime through
    `run.Get<ActionRuntime>()`. A future online service is one more registration, by whoever
    owns it. (Members stay members; registering one borrows it.)

D10. **Each run has an id that is never reused** in the process (a counter in `foundation.run`),
     and a name ("Game 2", "Server"). Tables that must outlive a run (audio's ending groups, logs,
     MCP answers) key by the id, never by address.

D11. **Null stays "outside every run".** An editor page's scene and a test's scene have no run.
     They have no network, input map, save or run bus to hold.

D12. **The run owns its event bus.** `IRun::Events()`; `RunBase` holds it (today's
     `GameInstance::m_runEvents`). A scene reaches it through its run: `Scene::Events()` stays as
     the accessor scene systems call, now `Run() ? &Run()->Events() : nullptr`, and
     `Scene::SetEventBus` / `SceneManager::SetSceneEventBus` go (a scene is handed its run once,
     not its run and its bus). `run.events()` in scripts reads the same bus as today.

D13. **The script surface does not change.** `run`, `Input`, `Net`, `Save`, `Http`, `Audio` and
     `gui` read the same to a script; `ScriptBinding::run` becomes an `IRun*`. Whether the run
     host should install the run's script services from the run itself is P4, optional.

## Phases

### P1: `foundation.run`

`IRun`, `RunBase`, `IRunService`, `IRunListener`, `IRunManager`, `RunRegistry`, and the id
counter, as D1-D7, D10 and D12, with `Run.Tests`. Nothing in the engine uses them yet.

- Tests: services add/register/get/remove by type; owned destroyed in reverse order, borrowed
  not; a type registered twice replaces (destroying an owned predecessor); session services go
  at Stop and session-long ones stay; `OnRunStopping` and `OnRunPaused` reach the services that
  implement them, in reverse order; `SetPaused` repeated is announced once; disposal while
  running stops first; ids never reused across destroy and create; the registry announces each
  transition once, in order, to every listener; a listener subscribed late catches up by
  enumerating; destroying the focused run clears the focus. ASAN clean.
- Acceptance: `foundation.run` links Core and Messaging only.

### P2: the runtime, scenes and the game carry runs

- `SubsystemManager::SetRunManager` / `RunManager()` (`foundation.runtime` links
  `foundation.run`).
- `Scene::Run()` becomes `run::IRun*`; `SceneManager::SetSceneRun` takes one; `Scene::Events()`
  reads the run's bus and `SetEventBus` / `SetSceneEventBus` go; `ScriptBinding::run` becomes an
  `IRun*`.
- `GameInstance` derives from `RunBase`; its bus moves into it; it registers its services (D9).
- `DefaultApplication` implements `IRunManager` over a `RunRegistry`; `CreateInstance` and
  `ReleaseInstance` announce created and destroyed; the primary instance likewise; it sets the
  subsystem manager's run manager.
- The Game tab's Play calls `Start`, its pause `SetPaused`, its Stop `Stop`; focusing its viewport
  calls `SetFocused`.
- Tests: `GameInstanceTests` (created and adopted scenes carry the instance as their run, and its
  bus, `GameInstanceTests.cpp:139-148`); a released instance is gone from the manager and its
  destruction announced; a subsystem finds the run manager through its `Manager()`.

### P3: subsystems follow the run manager

- `AudioSubsystem`: run groups keyed by run id, ended on `OnRunStopping` (faded), paused on
  `OnRunPaused`, the focus from `OnFocusChanged`; `EndRun`, `SetRunPaused` and `SetFocusedRun`
  leave its public surface.
- `GuiSubsystem`: screen tiers keyed by run id, ended on `OnRunStopping`; `EndRunScreens` goes;
  `ScreensFor` and `SetRenderRun` take an `IRun*`.
- `NetworkSubsystem` and `InputSubsystem` enumerate the runs; `DefaultApplication`'s endpoint
  source and action-runtime provider go.
- Both hosts' relays go: `ReleaseInstance` and the Game tab make one call each.
- Tests: `RunAudioTests` and the GUI subsystem's run-tier tests over runs, same assertions; a run
  stopped WITHOUT any audio or GUI call leaves no run group or screen tier behind; a released
  run's id is not reused by the next; a subsystem registered after runs exist (as a plugin's
  would be) catches up.
- Acceptance: no `const void* run` remains (`grep -rn "const void\* run" Code` is empty).

### P4 (optional): the run host reads the run

Today the run installs its script services on each script context through a callback
(`ScriptRunHost::SetOwnerServices`, `GameInstance::InstallRunServices`), each under a string key
(`run.runtime`, `run.save`, `net.runtime`, `http.runtime`). P4 would have the run host install
them from the run: a service type that scripts reach declares its key
(`static constexpr StringView kScriptService`), and the host installs every registered service
that has one. App-wide services (`audio.runtime`, `render.runtime`) stay as they are. Worth it
only if the callback becomes a burden.

## Gotchas

- **Shutdown order.** The application host stops in this order: `OnExit`, then
  `SubsystemManager::Shutdown`, then the application's `OnShutdown` (`ApplicationHost::Stop`);
  `DefaultApplication`'s instances, the runs, outlive all three. So a subsystem unsubscribes in
  its own shutdown, while the runs still exist; `DefaultApplication` clears the subsystem
  manager's run manager in `OnShutdown`; the runs' destruction, after that, reaches no
  subsystem.
- **Plugin reload.** A plugin's subsystem unsubscribes as it is unregistered (the editor
  reloads native modules); its catch-up on the next load rebuilds its per-run state.
- **Audio's fade outlives the session.** A stopped session's group keeps fading for
  `fadeSeconds`; the reap list holds the group handle, not the run.
- **The editor stops without disposing.** Tests cover Stop then Start again on one run:
  per-session services come back fresh, session-long ones are the same objects, the subsystems'
  per-run state starts over.
- **The allocator tripwire.** `RunBase` and `RunRegistry` take their owner's allocator; nothing
  new reaches `DefaultAllocator()` outside the allowlist.

## Related work

- **The script context's names made explicit** (user 2026-10-30, regardless of this spec):
  parameters, locals and members that hold an `IScriptContext` and are called `context` or
  `m_context` become `scriptContext` / `m_scriptContext`, so a reader never has to ask which
  context. Mechanical, its own commit, before or alongside P2.
- **The subsystem manager's rename**: done (`Specs/subsystem-manager-rename.md`).

## Open questions

1. **Events or services for subsystem state (D8).** Events keep ownership with the subsystem and
   avoid cross-tier teardown; services would give a scene's run direct access to its GUI tier
   (`scene.Run()->Get<GuiRunTier>()`) without a table lookup. Proposed: events.
2. **When.** Nothing is broken; the change crosses the runtime, the scene layer, audio, the GUI,
   the default application and the Game tab. Proposed: after 0.1, unless the online layer for the
   flagship comes first, in which case P1 and P2 come before it. (The user did the subsystem
   manager's rename before 0.1 for being cheaper then; the same argument may hold here.)
3. **`GameInstance` deriving from `RunBase`, or holding one** and forwarding `IRun` to it.
   Proposed: deriving (one object, nothing to forward); holding keeps GameInstance's class
   shape as it is.

## Not in scope

- Moving the application time scale off the subsystem manager.
- A run for editor pages or tests (D11).
- The online service itself; the run manager is what it will plug into.
