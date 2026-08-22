# Scene composition & observation

> Status: SHIPPED (composition + observation shipped; `FrameTime` lane cutover deferred)
> Track: [[game-instance-track]]

The scene layer of the engine is a "world model + systems" foundation (`foundation.scene`) driven by the
runtime tier (`foundation.runtime`, `engine.*` subsystems) through a single broker, `SceneSubsystem`
(`engine.scene`). This document proposes a redesign of that broker and the assembly of per-scene systems.
It keeps the architecture's dependency direction, value-pool components, and multi-instance model intact,
and replaces the **imperative, duplicated, partially-ordered** wiring between the runtime tier and the
scene tier with **declarative composition + ordered observation**.

Related: [[game-instance]], [[runtime-host]], [[scripting]] (the one-context rule, now per instance).

## Implementation status

The design is shipped end-to-end and green across every affected test suite (`Scene.Tests`,
`Engine.Scene.Tests`, `Engine.Navigation.Tests`, `Engine.Physics.Tests`, `Engine.Audio.Tests`,
`Engine.Script.Tests`, `Engine.UI.Tests`, `Engine.GameInstance.Tests`, `Engine.Render.Tests`,
`Engine.SceneSurface.Tests`, `Engine.Net.Tests`, `Engine.DefaultApp.Tests`, `Integration.Mcp` — zero
failures):

- **`foundation.scene:composition`** (`Code/Foundation/Scene/SceneComposition.cppm`) defines
  `SceneLifecycleStage`/`ISceneObserver`, `SceneModule`, `SceneComposition` (topological `Build` +
  `Instantiate` + `RegisterReflection`), and the pure `SceneRegistry` — plus `FrameTime` for the time
  model. Covered by `Code/Foundation/Scene.Tests/SceneCompositionTests.cpp`.
- **`SceneManager` owns ONLY type-erased install/uninstall hooks** (`SceneInstaller`/`SceneUninstaller`).
  There is no `SceneAwareRegistry` member and no legacy fallback: `CreateScene` assembles via the installer,
  teardown notifies via the uninstaller.
- **`SceneSubsystem` is a thin adapter over `SceneRegistry`.** It exposes an `ISceneObserver` broker
  (`RegisterObserver`/`UnregisterObserver`), owns the composition (`SetComposition`/`Composition()`),
  and wires each registered manager with an installer (composition assembly → `SystemsReady`) and an
  uninstaller (`Destroying`). The old `ISceneAware` broker and `AwareRegistry()` accessor are **deleted**.
- **Every engine domain contributes a `SceneModule`** to `FullSceneComposition()` (Render, Animation,
  Particles, Physics, Navigation, Audio, Script, UI, Net). Their subsystems are now either pure reflection
  registrars (Animation, Net) or `ISceneObserver`s that wire reactive per-scene state at `SystemsReady` /
  `Destroying` (Render, Physics, Audio, Script, Particles, UI, Navigation). `OnSceneCreated` no longer
  exists anywhere.
- **The runtime path adopts the composition.** `DefaultApplication::Configure` calls
  `SetComposition(engine::FullSceneComposition())`; the editor's scratch scenes (export transcode/
  reachability scan) are assembled via `engine::AddAllSceneManagers` (the same composition). The
  `ISceneAware`/`SceneAwareRegistry` types are removed from `foundation.scene` entirely (the partition was
  deleted), not merely deprecated.
- **`GameInstance::TickScript` uses `FrameTime::SceneDt()`** for the game script's update dt.

Still deferred (Why #4's host-loop decision): cut the `SceneManager`/`SceneRegistry` lane fan-out, the
`PhysicsSubsystem` interp read, and the input/net drives over to sharing one `FrameTime` (the host-loop
ownership question in `ApplicationHost::Tick` needs deciding before the lane signatures change).

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
## FABLE REVIEW of the implementation (2026-08-19) - ADOPTED with a fix pass

The DeepSeek-built implementation on this branch is structurally faithful to the
proposal and ~85% right: all nine domains migrated, FullSceneComposition() is the
single shared source (runtime + headless + MCP all consume it - the crux), the
FrameTime ambition was correctly scoped down to the scale-chain value type with
the lane cutover deferred, and a module-count check survives in
SceneSurfaceTests. Four defects found and FIXED in-review:

1. **CRITICAL - the composition retained caller pointers.** SceneComposition
   stored `const SceneModule*`; the branch's own UI tests built modules in a
   block scope, so CreateScene dereferenced a dead stack object -
   reintroducing the exact dangling-pointer strain (#5) the redesign exists to
   remove. GCC segfaulted (Engine.UI.Tests); clang passed on stack-layout luck,
   which also shows the branch was never validated on the full two-compiler
   battery. FIX: Build() copies modules BY VALUE and drops `dependsOn` (build-
   time-only data); compositions are self-contained. Regression test added
   (build from block-scoped modules, destroy them, Instantiate).
2. The dependency-cycle break was SILENT (emit-in-arbitrary-order, no
   diagnostic) - against the house fail-loudly rule. FIX: LOG_ERROR naming the
   module.
3. The `Composing` stage was declared but never fired (a dead stage). FIX: the
   installer notifies Composing before Instantiate, SystemsReady after.
4. Doc/comment drift: SceneSurface's interface still claimed "the RUNTIME keeps
   per-subsystem injection" - false since the ISceneAware removal made the
   composition the sole assembly path. Rewritten to state the real contract
   (full set everywhere; absent subsystems leave their systems unwired/inert).
   Plus an alias duplication + mangled indent in AudioSubsystem from the edit.

Verified after the fix pass: full two-compiler battery ALL_GREEN (the GCC
segfault gone) + ASAN clean over Scene/Engine.Scene/Engine.UI/SceneSurface.

Behavior change to be aware of (accepted): every scene now carries the FULL
system set regardless of which subsystems exist; absent subsystems leave their
systems unwired (inert pools / no-op ticks). Per-configuration compositions
remain the designed escape hatch when a consumer wants a subset.

ADDENDUM (same review, found while answering the layering question): the
`Started`/`Stopped` stages are ALSO never fired in production - only the
registry unit tests call Notify with them directly. Scene::Start/Stop are
invoked by pages/instances straight on the Scene, where the registry cannot
see them. Either wire them (a SceneManager start/stop hook -> registry
Notify, the uninstaller pattern) or drop the two stages until a consumer
exists. Queue this WITH the FrameTime lane cutover - same seam.

FrameTime cutover LAYERING RULES (user question 2026-08-19, verified against
the import graph): foundation.scene and foundation.runtime are strict
SIBLINGS - zero imports in either direction - and must stay that way.
Context/Subsystem never see scene::FrameTime; foundation.runtime keeps
exporting plain floats (TimeScale/FixedTimeStep). FrameTime is CONSTRUCTED at
the engine bridge (SceneSubsystem / GameInstance - they already import both
sides) and handed DOWN. No `import foundation.scene` may ever appear under
Code/Foundation/Runtime. The `contextScale` FIELD NAME is established scene
vocabulary (SceneManager::BeginFrame's parameter + the documented
host x context x group x scene chain), not a dependency - acceptable as-is.
Deletion side is safe: the Context fixed lane is written each frame
(ApplicationHost::SetFixedTiming) but only SceneSubsystem reads
FixedTimeStep; NOBODY reads Context::FixedAlpha and NO subsystem overrides
the runtime FixedUpdate lane - the step config moves to the scene side and
the runtime plumbing can go.

Remaining deferred (unchanged from the plan): the FrameTime LANE cutover
(SceneManager/input/net consuming FrameTime end-to-end) - its own phase with
determinism criteria; and the PMIU Module.cppm rename, which rides the
reorg/role-grouping branch.
