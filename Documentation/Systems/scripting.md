# Scripting

> Status: CURRENT
> Verified: 2026-08-11 @ 9c9046f8
> Track: [[script-behaviors-p1]] / [[luau-backend-track]] / [[scripting-backend-neutrality]]

Backend-neutral, reflection-driven gameplay scripting. A language backend is a LIBRARY,
never the architecture: the neutral contract exposes the ENTIRE engine reflection surface
to any backend for free, and a backend is "done" only when the shared conformance battery
is green. Two backends are certified and independently switchable (see
[[script-backend-gating]]): **AngelScript** and **Luau** (the go-forward primary). Wren was
the neutrality-proving first backend and was RETIRED 2026-08-19 (1b220034) once it had
served that purpose - historical mentions in older specs refer to it.

## Architecture (the neutral contract - `foundation.script`)

- `IScriptManager` / `IScriptContext` - VM-agnostic; everything crosses as `Variant` +
  `TypeInfo`. `ScriptObject::Invoke(method, Span<Variant>)` calls a script method;
  `IScriptErrorHandler` carries kind/module/line/message; a context's host services are found
  by type (`CurrentService<T>`): its run host answers with its runtime binding, then its run's
  services, then the application's (subsystems register theirs with their manager), so there are
  no process globals and no lists of what to install.
- `RegisterReflectedTypes(manager)` emits the whole reflection registry into a backend in
  one call. Facades are ordinary reflected classes, so a new backend inherits the full
  engine API from reflection.
- **Backend registry** (`ScriptBackendRegistry`): `{languageId, displayName, fileExtensions,
  factory}`; resolves a backend by language id or file extension. No consumer names a
  concrete backend type. `CreateScriptManagerForLanguage(id)` is the resolve entry point.
- **Conformance battery** (`Code/Foundation/Script.Tests/BackendConformance.h`): a shared
  doctest battery every registered backend must pass - reflected-type emission (incl.
  statics on constructible classes), Variant round-trips, per-context services +
  `CurrentScriptContext` nesting, error propagation, GC hook, and the optional-capability
  sections. A backend is certified when the battery is green, not "hopefully".
- **Capability flags** (`ScriptCapabilities`, reported by `IScriptManager::Capabilities()`):
  Coroutines, Delegates, Bytecode, Debugger. The MANAGER is the single source of truth -
  consumers with a backend in hand ask it. AngelScript + Luau: all four
  (Coroutines+Delegates+Bytecode+Debugger).
- **Two-phase type registration** (the AngelScript rule): `RegisterType` COLLECTS;
  `IScriptManager::FinalizeTypes()` (called by `RegisterReflectedTypes`, and defensively
  before the first `CreateContext`) does declare-all-types-then-bind-all-members. A backend
  may defer all emission to finalize (AngelScript needs it; Luau finalizes lazily).
  Facade authors write against the contract, not a backend quirk (`builder.Constructor()`
  is part of the documented contract).

## Tiers + the run host (`engine.script`)

Three tiers, all built. The run host (`ScriptRunHost`) resolves the backend from
`ScriptClass.language` - the whole stack is backend-neutral.

- **Game** (project orchestrator, the scripted `IApplication` counterpart): a `Game` class
  driven `new()` / `launch()` / `update(dt)` / `exit()`, coordinating ABOVE scenes.
- **Level** (per-scene): a `Level` class with `onStart`/`onUpdate`/`onFixedUpdate`/`onStop`,
  bound to a value-facade Scene.
- **Behavior** (per-entity): scripts ticked under simulation. This is the main surface.

**One gameplay context per run** (locked): a run owns ONE `IScriptContext` hosting the game
script + all behavior instances (shared module space). The FIRST-loaded behavior locks the
run's language; a behavior in a different language is refused and disabled
(`EnsureContext`/`WarnLanguageMismatchOnce`) - mixing languages in one run is by design not
supported. Stop tears the context down (isolation is between RUNS, the PIE rule).

## Entity behaviors

- **`ScriptComponent`** (serializable, value-pool, one per entity): an ordered array of
  `Behavior{ script: Ref<ScriptClass>, enabled, overrides: [{nameHash, Variant}] }` plus a
  runtime `ScriptObject` instance. Execution order = array order; cross-entity order is
  unspecified. Prefab members work via the component-granular delta machinery (overrides
  live in the payload).
- **Authoring idiom** is shape-identical across backends, spelled in each language's OOP: a
  constructor takes the owner (`new(entity)` / Level `new(scene)` / Game `new()`), then
  handlers dispatch by presence (`onStart`/`onUpdate(dt)`/`onFixedUpdate(dt)`/`onEnable`/
  `onDisable`/`onDestroy`, plus event handlers). Ticking is simulation-gated
  (`OnlyWhenSimulating`); optional per-behavior `updateInterval` throttles.
- **Properties**: declared per language (AngelScript typed member fields + `[metadata]`;
  Luau instance fields walked from the constructed table).
  The **cook harvests** them (compile the class in the cook VM, construct + walk / read the
  typed members), so the editor renders the inspector with NO VM. Editor values are stored
  as name-HASHED override blobs (rename-safe) and re-applied after hot reload.
- **Hot reload** (edit during play/simulate): resource reload -> per-instance re-instantiate
  -> re-apply hashed overrides; old-instance coroutines stop. (Live-value migration is a
  deferred follow-up - see [[Archive/scripting-followups-history]].)

## Events into scripts

- **Physics**: `onContactBegin/End(other, point, normal, speed)`, `onTriggerEnter/Exit`.
  Script does NOT depend on physics - a composition-root `ScriptContactBridge`
  (`DefaultApplication` owns it) resolves an entity-packed body user word to the exact
  entity and dispatches.
- **entity.send**: `entity.send("heal", amount)` invokes `onHeal(amount)` on every behavior
  of the target that declares it. Delivery is DEFERRED (queued + drained at the tick's top
  level - re-entrant VM calls are unsafe). Payload is a single `Variant`, not N overloads
  (see [[overloaded-name-contract]]).
- **Event bus**: `emit(name, payload)` -> `on<Event>` (NATIVE name-keyed bus, C++-first;
  `foundation.messaging`). ONE bus per run scope (messaging.md): a GameInstance owns its run bus and
  injects it into every scene it creates, so `scene.events` and the run bus are the SAME object - a
  behavior's emit reaches the Game tier with NO relay. An edit-mode scene's scope is its EDITOR PAGE (a page-owned bus, injected + drained by the page); a scope-less scratch scene has a NULL bus and never emits (there is NO owned fallback - revised decision 2). The owning scope drains once per frame; borrowing scenes never drain it.
- **Delegates**: `IScriptDelegate` wraps a script function as a native callback (certified
  both typed backends; the `DelegateSignal` facade uses it), with a Detach-on-teardown
  protocol.
- **Coroutines** (all backends): `startCoroutine` / `waitSeconds` / `waitUntil`, resumed by
  `AdvanceCoroutines` and cancelled per-instance on destroy/disable (and on entity
  deactivation - [[entity-active-state]]). AngelScript + Luau use pooled resumable
  contexts/threads.

## Engine API surface (facades)

The reflection registry exposes every reflected type. On top, a curated gameplay facade set
(`foundation.script.facades` + per-subsystem facades, kept OUT-OF-TREE per [[facade-pattern]]):
`entity` (transform/name/destroy/findChild/components), `Scene.spawn(prefab, x, y, z)` (the scene's
own `PrefabSpawnSystem`, see [[text-scenes]]; no host callback) /
`Scene.find`, `Input`, `Audio`, `Physics.rayCast`, `UI`, `Net`, `Log`, `Time`, `Random`. Rule:
a facade lands only WITH a sample that uses it (no API-first surface). Write facades with
natural C++ types (i32/i64), not f64 ([[script-facade-numerics]]). `Engine.Composition`
(`RegisterAllScriptFacades`, each domain's facade registrar declared on its DomainModule) is
the composition root for the complete bound surface (engine-composition.md).

## Cook + runtime resources

- **`ScriptClass`** (cooked product, `foundation.script.resource`): source text + harvested
  metadata (className, properties, declared handler set, `sourceName`, version) + optional
  cooked **bytecode**.
- **Per-language cook** (`IScriptLanguageCook` / `ScriptLanguageCookRegistry`): compile-check
  (surfaces file/line cook errors), harvest properties/handlers, emit tier starters. The
  cook fingerprint includes the vendored Luau version (Luau bytecode is not stable across
  versions).
- **Bytecode consumed at runtime** (Luau + AngelScript): the run host LOADS the cooked
  bytecode instead of recompiling source. Luau loads each class's blob as its own chunk;
  AngelScript LoadByteCode's each class into its own module (`CreateInstance`/`FindFunction`
  search all owned modules newest-first). Debug info + section names are preserved, so
  breakpoints line up on bytecode-loaded classes.

## Debugger

In-process step debuggers for **AngelScript and Luau** (breakpoints, step into/over, call
stack, locals, capture-object expand), driven through the run host with game-pause
(`IsDebugPaused` gates the tick) and editor UI (breakpoint gutter + call-stack/locals panel).
Suspension-based, non-blocking. Full detail: [[Systems/script-debugger]].

## Editor

- **ScriptPage**: edit source -> recook -> hot reload, with inline error surfacing, a bound-API
  browser (`ScriptApiBrowserView`) + autocomplete driven off `DescribeBoundApi()` (`ScriptApiSurface`).
- **Per-language editor UI** (`Editor.Script.{AngelScript,Luau}`): a CodeEditView lexer
  registered by language id; kept OUT of the UI-free Pipeline layer ([[pipeline-ui-free]]).
- **New-Asset starters**: one creator per registered backend per tier (Behavior/Level/Game),
  seeded from the cook's tier template - e.g. "Luau Behavior". A backend appears only when its
  COOK is registered (the cook registration also registers the backend).

## MCP / tooling

`IScriptManager::DescribeBoundApi()` reports the accurate, backend-reported bound surface
(reflection-vs-backend diff conformance check keeps it honest); the MCP `script_api` tool
reports it per language. See [[mcp-track]].

---

Design lineage + resolved open questions: `Documentation/Archive/scripting-design-history.md`.
Deferred follow-ups: `Documentation/Archive/scripting-followups-history.md` (open items: week-2026-09-05).
