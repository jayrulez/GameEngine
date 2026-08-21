# Scene composition & observation

> Status: PROPOSED
> Track: [[game-instance-track]]

The scene layer of the engine is a "world model + systems" foundation (`foundation.scene`) driven by the
runtime tier (`foundation.runtime`, `engine.*` subsystems) through a single broker, `SceneSubsystem`
(`engine.scene`). This document proposes a redesign of that broker and the assembly of per-scene systems.
It keeps the architecture's dependency direction, value-pool components, and multi-instance model intact,
and replaces the **imperative, duplicated, partially-ordered** wiring between the runtime tier and the
scene tier with **declarative composition + ordered observation**.

Related: [[game-instance]], [[runtime-host]], [[scripting]] (the one-context rule, now per instance).

## Why (the strains in the current design)

The current design is correct at the layer level but strained at the integration level. Five concrete
problems, found in code:

1. **Scene assembly is imperative, duplicated, and coupled to live subsystems.** Each engine subsystem
   registers itself as scene-aware in `OnReady()` via
   `ctx->GetSubsystem<SceneSubsystem>()->RegisterSceneAware(this)` (Render, UI, Physics, Script, Audio,
   Navigation, Animation, Net, …), then its `OnSceneCreated` reaches *into* the `Scene` and `AddSystem`s
   its managers. `Engine.SceneSurface::AddAllSceneManagers` duplicates the *same* manager list for headless
   consumers (CLI export transcode, MCP `scene_validate`) and keeps the two in sync with a **count
   tripwire** — `engine::kSceneSystemCount = 34`, asserted in `SceneSurfaceTests`. The tripwire exists only
   because composition has two sources of truth that can drift.

2. **`ISceneAware` conflates two unrelated concerns and bakes in one ordering.** "Inject my systems"
   (assembly) and "react to a scene coming/going" (observation) are the same interface, and
   `SceneAwareRegistry::NotifyCreated` hardcodes a two-pass `Created -> Ready` convention. A third hook, or
   a semantic dependency between two aware subsystems (physics-after-render, script-after-metadata), has no
   place to live. Ordering currently leaks into the unrelated `Subsystem::UpdateOrder()` axis.

3. **`SceneSubsystem` is a "pure registry" doing three jobs.** It owns the app-wide observer list, owns the
   manager list, *and* is a lane-driving `Subsystem` (`UpdateOrder() = -500`, `BeginFrame`/`Update`). Its
   `ForEachManager`/`ForEachScene` sweeps are a missing first-class "collection" abstraction.

4. **The time model is implemented twice, inconsistently.** `SceneManager::BeginFrame` applies the context
   scale *inside* (`rawHostDt * contextScale * groupScale * sceneScale`); `SceneManager::Update` receives
   already-context-scaled dt and applies only group×scene; `GameInstance::TickScript` recomputes
   `hostDt * contextScale * instanceScale * sceneScale` by hand in a third place. The `Context` also carries
   its own `SetFixedTiming`/`FixedAlpha()` and a `FixedUpdate` phase that the scene lane does **not** use
   (`SceneSubsystem` overrides only `BeginFrame`/`Update`). Two parallel fixed-step notions never meet.

5. **Registration is raw-pointer + linear de-dup** (`RegisterManager(SceneManager*)`,
   `Register(ISceneAware*)`, `Unregister*`). It works, but the dangling-pointer hazard is real enough that
   `DefaultApplication::ReleaseInstance` carries an explicit "unregister first or a borrowed pointer gets
   ticked" comment.

## What stays (the kept invariants)

These are non-negotiable and the redesign preserves them verbatim:

- **Dependency direction points down.** `GameInstance` (runtime tier) owns a `SceneManager`; the scene
  library never references the runtime tier. The scene lib stays runtime-free and context-free.
- **Per-scene systems as value-pool component managers.** Components are plain value data in sparse sets
  (`Component.cppm`); behavior lives on `SceneSystem`/`ComponentManager<T>`, not on components.
- **Type-indexed, RTTI-free composition.** `Context::GetSubsystem<T>()` and `Scene::GetSystem<T>()` resolve
  through reflected `TypeOf<T>()` (`-fno-rtti`).
- **The multi-scene / multi-instance model.** `SceneManager` as a first-class *group*, `0..N GameInstance`
  (play-in-editor + in-editor headless dedicated server).
- **The time layering** host × context × group × scene — this is the right mental model; it just gets
  implemented in one place (`FrameTime`, below).

## The redesign

The reframing in one sentence:

> Separate **"what systems a scene is made of"** (a static blueprint) from **"who reacts to a scene's
> lifecycle"** (observation). Today those are the same `ISceneAware` and the same imperative code path.

Five new pieces, all in `foundation.scene` (keeping the scene lib runtime-free):

### 1. `SceneModule`

The single, declarative unit of "how to build a scene." One object per domain declares three things the
domain already knows today:

- `install(Scene&)` — the manager/settings-system construction (today's `Add<Domain>SceneManagers`).
- `registerReflection()` — the component reflection (today's `Register<Domain>ComponentReflection`).
- an optional `dependsOn` list for cross-domain ordering.

```cpp
// engine.render module (replaces the free function + reflection pair)
const scene::SceneModule kRenderModule{
    u8"render",                 // stable id
    &AddRenderSceneManagers,            // existing body
    &RegisterRenderComponentReflection, // existing body
    {},                          // dependsOn
};
```

A new domain adds **one `SceneModule` object**, registered in **one place**; the runtime path, the headless
path, and any future composition all pick it up. No tripwire to bump, no `OnReady` boilerplate, no forgotten
`RegisterSceneAware` silently dropping a component type.

### 2. `SceneComposition`

The one-time-built blueprint produced from a module list. It topologically sorts by `dependsOn` and exposes:

- `Instantiate(Scene&)` — adds every declared system in order.
- `RegisterReflection()` — registers every domain's component reflection.

This is the **single source of truth**:

- the runtime path instantiates scenes from it;
- `SceneSurface`'s headless scratch instantiate scenes from the *same* composition — no parallel list, no
  `kSceneSystemCount` tripwire;
- because composition is *data*, it yields per-composition variations for free (a "minimal headless server"
  composition, an "editor-only" composition) that the current all-or-nothing wiring cannot express.

### 3. `ISceneObserver` + `SceneLifecycleStage`

Replaces `ISceneAware`'s hardcoded two-pass with explicit, ordered stages:

```
Composing → SystemsReady → Started → Stopped → Destroying
```

Live subsystems that must *react* (Render dropping per-scene render-data providers on destroy, Physics
holding a per-scene world) register as observers with a stage and an order. Assembly no longer happens
through observation. `ISceneAware` remains as a thin shim until all callers migrate.

### 4. `FrameTime`

One value type carrying `rawHostDt` and the `host × context × group × scene` multipliers plus the fixed-step
accumulator. Both scene lanes *and* `GameInstance::TickScript`/input/net consume it, eliminating the three
duplicated dt computations and unifying the `Context` fixed timing with the per-scene stepper.
`SceneManager::BeginFrame`/`Update` take a `FrameTime` instead of the current ad-hoc
`(rawHostDt, contextScale, contextStep)` / `(contextScaledDt)` split.

### 5. `SceneRegistry` (value type) + thin `SceneSubsystem`

Extract the pure state out of `SceneSubsystem` — the `SceneComposition`, the observer list, the
`SceneManager*` list, the cross-scene sweeps — into a non-`Subsystem` `SceneRegistry`. It becomes
unit-testable with **no `Context`** (fixing strain #3). `SceneSubsystem` shrinks to the thin lane adapter
that fans `BeginFrame`/`Update` over the registry.

## Before / after wiring

### Today

```cpp
// app: CreateInstance/Configure path
m_scenes = host.Ctx().AddSubsystem<SceneSubsystem>();
m_scenes->RegisterManager(&instance.Scenes());
instance.Scenes().SetAwareRegistry(&m_scenes->AwareRegistry());   // 3-step, pointer-matching

// each subsystem OnReady():
ctx->GetSubsystem<SceneSubsystem>()->RegisterSceneAware(this);   // 8+ copies of this

// headless:
AddAllSceneManagers(scratch);   // parallel list kept honest by a count tripwire
```

### After

```cpp
// app, once at startup:
scene::SceneRegistry registry;
registry.SetComposition(scene::SceneComposition::Build({
    &kRenderModule, &kPhysicsModule, &kScriptModule, &kAudioModule, /* ... */ }));
registry.RegisterReflection();

// app, per instance:
registry.RegisterManager(&instance.Scenes());   // manager pulls composition from the registry

// headless — no drift by construction:
scene::SceneComposition::Build({ /* the same module array */ }).Instantiate(scratch);

// a subsystem that only reacts:
registry.AddObserver(&kRenderObserver, SceneLifecycleStage::Destroying);
```

## Type / partition layout

- New partition `foundation.scene:composition`, file `Code/Foundation/Scene/SceneComposition.cppm`, holds
  `SceneModule`, `SceneComposition`, `ISceneObserver`, `SceneLifecycleStage`, `FrameTime`, `SceneRegistry`.

### Naming notes

- **`SceneModule` (the type) is free today.** `Code/Foundation/Scene/SceneModule.cppm` is the *primary
  module interface unit* for `foundation.scene` — it holds only `export import :...` of the partitions and
  defines **no type**. So `SceneModule` can be used as the type name immediately, with no C++ collision.
  The only wart is the adjacent *file name* squats the name, which is a file-convention problem, not a
  symbol problem.

- **PMIU convention (future, out of scope): name every primary interface unit `Module.cppm`.** It is
  self-describing ("the module's interface unit"), identical across modules, and can never squat a domain
  type name — `Module` is reserved for the file's *role*, not a concept. The mechanical pass renames, e.g.,
  `SceneModule.cppm` → `Module.cppm`, `RuntimeModule.cppm` → `Module.cppm`,
  `SubsystemModule.cppm` → `Module.cppm`, `AnimationSubsystemModule.cppm` → `Module.cppm`, with zero
  semantic change. This is recorded as **Deferred**, not part of this change.

## Migration plan (incremental, non-breaking)

1. Add the `foundation.scene:composition` partition with `SceneModule` + `SceneComposition`. Port one domain
   (Render) to a `SceneModule` object, keeping `AddRenderSceneManagers` as the module's `install`.
2. Add `SceneRegistry` as a value type and have `SceneSubsystem` delegate to it (mechanical, no behavior
   change).
3. Introduce `ISceneObserver`/`SceneLifecycleStage` alongside `ISceneAware`; migrate observers one domain at
   a time, keeping `ISceneAware` as a thin shim until all callers move.
4. Route `SceneManager::CreateScene` through `SceneComposition::Instantiate`; delete the parallel
   `AddAllSceneManagers` list and the `kSceneSystemCount` tripwire once headless consumes the same
   composition.
5. Introduce `FrameTime` and cut over `SceneManager`/`GameInstance::TickScript`/input/net to it; delete the
   now-redundant `Context` fixed-timing/`FixedUpdate` plumbing on the scene lane.

## Decision log

- **`SceneModule` as a function table, not a reflected type.** The module's three responsibilities
  (`install`, `registerReflection`, `dependsOn`) are static construction-time data; reflection would add a
  type registry round-trip for no runtime benefit. A plain value type with function pointers is sufficient
  and keeps `foundation.scene:composition` dependency-light.
- **`dependsOn` as data, not `UpdateOrder`.** Cross-domain construction order (e.g., script-after-metadata)
  is a property of the *composition*, not of a per-frame tick order. Keeping the two axes distinct avoids
  the current leakage of assembly ordering into `Subsystem::UpdateOrder()`.
- **`SceneRegistry` as a value type, not another `Subsystem`.** The pure state (composition, observers,
  manager list) is testable without booting a `Context`; `SceneSubsystem` remains the only thing that
  *drives* a frame lane.
- **`FrameTime` over individual dt parameters.** Centralizes the host×context×group×scene math and the
  fixed-step accumulator, giving one place to reason about determinism, pause, slow-mo, and fixed-step
  interpolation across scenes *and* scripts.

## Deferred

- Rename primary module interface units to `Module.cppm` (mechanical, out of scope for this change).