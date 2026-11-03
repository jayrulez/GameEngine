# GameInstance

> Status: CURRENT
> Verified: 2026-08-12 @ 33b89288
> Track: [[game-instance-track]]

"A running game" is a first-class object. The standalone player and the editor's Game tab share ONE
run bracket, and the editor can host more than one simultaneously (multi-instance play-in-editor + an
in-editor headless dedicated server for networking). The whole track shipped: run ownership moved off
the app/subsystems and onto `GameInstance`, with a `SceneManager` owning each group of scenes.

Related: [[runtime-host]], [[scripting]] (the one-context rule, now per instance), [[networking]]
(headless PIE), [[editor-track]].

## Two layers

- **Engine host** (`IApplication` / `DefaultApplication`) - "the engine is running": the Context +
  subsystems + resource infra + shell/graphics, configured ONCE, holding NO run state. It owns
  `Array<GameInstance>` and exposes `PrimaryScenes()` (returns a `SceneManager&`, so callers reach the
  primary group without naming `GameInstance`).
- **`GameInstance`** (`engine.gameinstance`, `Code/Engine/Engine.GameInstance`) - "a game is running":
  dynamic, 0..N, owned by the host. Each instance OWNS its `ScriptRunHost` (its script context + `Game`
  object + error sink), its `SceneManager` (its scenes + current + group config), its per-instance
  `NetworkController` (`m_network` - the `INetworkController` + the endpoint it owns, null = offline;
  networking-extraction.md P1: GameInstance COMPOSES it and forwards, it no longer inherits
  `INetworkController`), its per-instance `ActionRuntime` (`m_inputRuntime`), its RUN BUS (`m_runEvents`,
  a `messaging::EventBus` - messaging.md P2: injected into every scene it creates, so `scene.events` and
  the run bus are ONE object, drained once per frame by the instance; no scene->run relay), a headless
  flag, and an instance time scale. It BORROWS everything shared (subsystems, resource manager / content
  DB, factories, reflection registries).

The dependency arrow points DOWN (runtime -> scene): `GameInstance` owns a `SceneManager`; the scene
lib never references `GameInstance`.

## `SceneManager` (scene lib)

A group of scenes as a first-class scene-lib object - the multi-scene model (a run manages a SET of
scenes with a current one, not a single scene). It owns membership (create / adopt / load / switch /
unload / destroy + the current scene), group config (time scale, running/paused, headless, and the
group's fixed-step accumulator), and it TICKS its own group (`Update(hostDt)` / `FixedUpdate` at the
group rate). Scene assembly + lifecycle run through the DECLARATIVE composition layer
([[scene-composition]], landed 2026-08-19): `CreateScene` assembles from the registered
`SceneComposition` (via the type-erased installer `SceneSubsystem` wires onto each manager), and
teardown notifies `ISceneObserver`s at the `Destroying` stage. `ISceneAware` no longer exists.

`SceneSubsystem` is a thin adapter over the pure `SceneRegistry` value type: the composition, the
staged `ISceneObserver` list, the registered `SceneManager*` list it ticks (building ONE
`scene::FrameTime` per frame from the Context's plain floats - the runtime layer never sees the
type), and the read-only sweeps (`ForEachManager`, registry-wide `ForEachScene` for
prefab-rebuild / export scans). It owns no scenes. Every owner of scenes creates its own plain
`SceneManager` and registers it (`scenes->RegisterManager(&mgr)` wires the install/uninstall
hooks): a `GameInstance` owns one; each editor page (`ScenePage`, `MaterialPage`) owns one
(registered on open, destroyed + unregistered on close).

## Time model

```
effective dt a scene sees = host dt x context (app-wide) x group/instance x scene
```
The group/instance term is the manager's; the editor's editing managers use 1.0. The fixed lane's
accumulator is the manager's too. At N=1 in the player, context and instance collapse (which is why the
pre-track two-level model was correct for shipping).

## Run-host binding

Run host and scene manager are PAIRED. `ScriptSubsystem` keeps ONE run host (`m_ownedRunHost`) for the
default/editing scenes; each `GameInstance` owns one for its group. `OnSceneCreated` binds a scene to
the subsystem's default host; a `GameInstance` RE-BINDS its own scenes' `ScriptSceneSystem` to its host
after adopting them (legit: the instance is in the script layer; the lower `SceneSubsystem` cannot reach
`ScriptSceneSystem`). Each owner drives its own host (binding clock + GC + teardown). Routing is shared
logic on any host: `ScriptSubsystem::ConfigureRunHost(host)` installs the message route + service
configurator (`scene.spawn` needs no host hook: it reaches the scene's `PrefabSpawnSystem`); `MaybeTeardownRunHost(host)` tears a host down when nothing holds it, none of
its scenes simulate, and no live behaviors remain. The game-script hold is a per-host flag; the game
script talks to the instance's host directly.

So: one gameplay context PER INSTANCE - all of an instance's scenes' behaviors AND its game script
share its one run host, and the brain persists across level transitions (MainMenu -> Level1 -> Level2
is one game, one context).

## Scene-management facade

`SceneLoader` (a `SceneLoaderScriptBinding` installed per instance) gives the game script level-load
control routed through the owning `GameInstance`: `SceneLoader.loadSceneAsync(id)` kicks an async load
and returns a ticket (`loadComplete(t)` polled from a coroutine), `loadScene(id)` is a sync convenience,
`sceneReady` reports state. Certified across the three backends (starter snippets in each cook).

## Save data

Each instance owns its run's save (`RunSave`, the `:save` partition): the values the game keeps
between runs, the file they live in, and whether they changed since the last write. The host names
the file with `SetSaveFile(path)`, which reads it: the player uses `<user data>/<project>.save.xml`,
the editor's Game tab `<project>/Editor/<project>.save.xml` (ignored per-user state, so testing
never touches a player's save). The run's contexts get it as the `run.save` service, which the
`Save` facade resolves (`Save::setInt("best.level2", 4210)`, `Save::getInt(key, fallback)`, the
same for float, bool and string, plus `has`, `remove`, `clear` and `flush`). `StopScript` writes
whatever changed; `flush` writes at once. Writes are whole and atomic (`WriteFileAtomic`), so a crash
never leaves a torn save. Spec: `Documentation/Archive/save-data-history.md`.

## Standalone vs editor + headless

- **Player**: exactly one instance, created + started at launch. The current code IS the N=1 case.
- **Editor**: 0..N instances ("Play" appends one; "Play New Instance" appends another). `ScenePage` is
  NOT modeled as an instance (it has no script brain; snapshot->simulate->restore, not
  fresh-from-cooked->teardown).
- **Headless instances** (simulate, don't render, no listener) are the in-editor dedicated server for
  the networking track.

## Deferred

Player multi-scene compositing (N instances in the PLAYER hit the single-scene render path; the editor
is unaffected via per-viewport RTs), and the one-shared-editor-manager alternative to per-page managers:
`Documentation/Backlog/game-instance-followups.md`.

---

The design history (the retrofit diagnosis - the run bracket duplicated player vs editor, the original
ownership audit, what blocked multiple instances, the phased plan, the Zero `GameSession` precedent, and
the run-host-ownership investigation) is in
`Documentation/Archive/game-instance-design-history.md`.
