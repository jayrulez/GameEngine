# Game-ready scripting: reflected surface + tier communication + authored effects

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

**Status:** CLEARED TO BUILD (final review 2026-08-04) - build in phase order; both review passes folded into the body, records in Sections 9-10; all open questions (OQ-1..5) resolved. **Compat:** breaking changes allowed - deliberately.
**Owner:** Opus writes from docs/specs; Fable reviews. Built after the weekend reflection review.

Grounded against the local Traktor copy (`/home/robert/Dev/CPP/traktor`) and the shipped
Draconic scripting stack. Two structural gaps make the engine not game-ready, both proven by
the "Roll Call" sample game (SampleGame/): script cannot reach the engine except through
hand-curated static facades, and the Level/Game script tiers cannot communicate with
behaviors/components - forcing all cross-cutting rules onto a "GameManager" entity behavior,
the exact anti-pattern the Level/Game tiers exist to kill.

This spec covers two systems, in two tracks:
- **A. Reflected script surface** - reach the real subsystems/scene-systems/components
  directly, phasing down scope-lying facades.
- **B. Tier communication** - a scene/run message+event bus and a message inbox on the
  Level/Game tiers. This IS the generic "deliver/invoke an event" facility - it is
  payload-agnostic and knows nothing about sounds/particles/etc.

A third idea (a Traktor-style `EventSet` effects component) is **PARKED** - see Section 4 for
why it is a narrow effects system, not the generic event delivery it sounds like.

---

## 0. What already exists (the critical reframing)

Most of the machinery is already shipped - this spec mostly *wires* it, it does not invent a
reflection or dispatch layer. Confirmed in code:

| Concept | Traktor | Draconic - already present |
|---|---|---|
| Neutral boxed value | `Any` | `Variant` (+ borrow mode) |
| Method reflection builder | `AutoRuntimeClass::addMethod` | `TypeBuilder::Method<&T::f>("f")` (Reflection.cppm:1515) |
| Generated marshalling stub | `AutoMethod` / `IRuntimeDispatch` | `MethodReflect<>` / `InvokeMethod`/`InvokeStatic` |
| Overload resolution | arity/type `accept()` | `FindMethod` / `ResolveOverload` (param-type match) |
| Language-neutral backends | Lua + Remote | Wren + AngelScript (both iterate `Methods()` and dispatch) |
| Named-method inbox on an object | `findRuntimeClassMethod(...)->invoke` | `ScriptObject::Invoke(method, args)` (IScriptContext.cppm) |
| Closure-as-callback (subscribe) | `IRuntimeDelegate` | `IScriptDelegate::Invoke(args)` |
| Per-run host/service injection | environment injection | `ScriptRuntimeBinding` via `GetService(kScriptRuntimeService)` |

Two consequences:
1. **There is no "method-reflection primitive" to build - it is done.** The current facades
   (`Physics`, `Input`, `Audio`, `Ui`) are already reflected types: e.g. the `Physics` facade
   declares ~40 methods via `builder.Method<&Physics::moveCharacter>(...)`
   (PhysicsSubsystemImpl.cpp). The facade-vs-reflection distinction is **scope/identity**, not
   glue: `Physics` is a reflected *static singleton* that lies about scope ("the scene's first
   character/world"); the goal is to reflect the *actual* per-scene/per-entity objects.
2. **The tier-inbox and subscribe primitives already exist** (`ScriptObject::Invoke`,
   `IScriptDelegate`). The bus routes through them; it does not add a new script abstraction.

Cross-backend boundary (important, and it is a *rule*, not a feature to build): reflection
governs **native** types crossing into/between scripts. Neither Traktor's `IRuntimeClass` nor
our reflection makes a *script-defined* class visible to another backend (Traktor sidesteps
this by being single-backend Lua). The only thing that crosses Wren<->AngelScript is a native
reflected value type boxed in `Variant`. **Therefore: any event payload that crosses the
C++/script or Wren/AngelScript boundary must be Variant-expressible (a reflected value type or a
primitive), never a raw script object.** (Section 3 B3 keys the bus on a name, not a native type,
so script games still author their own events freely.)

---

## 1. Access model (ownership is the spine)

`scene.*` is scene-owned (one per scene); the run/Game tier reaches app-owned subsystems (one
per app). Getting this right is what makes the facades unnecessary.

| Script root | Ownership | Reaches | Example |
|---|---|---|---|
| `entity` | the behavior's entity | its components (`get` by type), `entity.scene`, targeted `send` | `entity.get(Character).move(x, z)` |
| `scene` (Level ctor arg / `entity.scene`) | **one per scene, scene-owned** | component managers / scene-systems, this scene's physics world, its **UI root view** (`scene.ui`), `find`/`spawn`, the **scene** event bus | `scene.physics.raycast(...)`, `scene.ui` (the scene RootView), `scene.events.emit(...)` |
| `run` (Game tier; the single root name - NOT `game`, which clashes with the mandatory `Game` class) | **one per app, app-owned** | subsystems (audio engine, physics service), the **run** event bus, scene loading (absorbs `SceneLoader`, S5) | `run.events.emit(...)`, `run.loadScene(...)` |
| utility facades (KEPT) | app-global, entity-less | `Log` `Time` `Random` `Input` `Audio`(one-shot) | `Input.axis2d("move")` |

`scene.physics` is the scene's **physics scene-system** (its world + `RigidBodyComponentManager`),
not the app `PhysicsSubsystem`. The facade we replace is the one that misrepresents scope
(`Physics` pretending there is one character/world); genuinely app-global, entity-less facades
(`Log`/`Time`/`Random`/`Input`/`Audio` one-shot) stay - a static is the right ergonomics there.

**Layering note (`run` root):** `entity` and `scene` are Foundation types (`draconic.scene`), so
their facades live in Foundation `Draconic.Script.Facades`. The `run` root is the Game tier
(`Draconic.Engine.GameInstance`, Engine) and has NO Foundation type - so the `run` facade is an
**out-of-tree Engine facade** (the `Net` pattern, registered via `RegisterExtraFacadeName`), NOT
part of `Draconic.Script.Facades`. Foundation never references the run tier. (There is exactly one
root name, `run`; `game` is avoided - it collides with the mandatory `Game` class.)

**Screen-tier UI is out of scope (deliberate future fix, not dismissed).** The engine also has a
*screen* overlay tier (`IScreenOverlay`/`IScreenRenderer`, window-space chrome - main menu, loading
screen, anything shown BEFORE any scene is loaded). That is app-owned and would surface on the
`run` tier (a future `run.ui`), not `scene`. It is genuinely useful precisely because it works with
no scene loaded - but its script surface is deferred to a later spec. This spec covers only the
scene-tier `scene.ui`.

---

## 2. Phase A - reflected script surface

**A1. Reflect gameplay methods on the real types.** Each scene-system / component manager /
component gains a `.Method<>()` block (in its IMPLEMENTATION unit, per the GCC gcm-cluster
hygiene rule) exposing its gameplay API. Uses the existing builder + dispatch; both backends
bind it for free via the existing emitter. No core changes.

**A2. `RigidBody.of(entity)` type-driven component access - a RE-RESOLVING handle, never a borrow.**
**Shape decided by the Fable ruling in Section 12 (OPTION 1, per-type static factory)** - the
emitter audit (Section 11) proved bare value tokens (`entity.get(RigidBody)`) are infeasible on
either backend and AngelScript cannot do a generic `get`. So the surface is a **static factory per
component class**: `RigidBody.of(entity)` returns a re-resolving handle of the component's type -
the typed `GetComponent` with the type in one mention, zero casts, and identical on both backends
(Wren wraps by dynamic `Type()`; AngelScript boxes by the factory's declared return type). The
handle re-resolves the live component **on every property/method access** via `MakeComponentRef`
(committed: `Scene::MakeComponentRef` -> a RESOLVE-mode Variant) - one O(1) sparse-set lookup, a
dead entity / removed component resolves to a clean null-op.

Two blessed primitives make it work (Section 12): a **general "additional emission roots" API** on
the Wren collector + `GlobalTypeRegistry().Register` for component types (they are not script-visible
today), and a **ReturnType-override on a reflected method** (declared return = the component type
while the body returns the RESOLVE Variant; `From<Variant>` passthrough composes) with the dispatch
**validating** that the resolved Variant's runtime type matches the declared return (mismatch =
clean null, never a type-confused handle).

**This is a safety requirement, not a style choice** (Fable Correction 1, verified in
`Component.cppm`): component storage is a sparse set with swap-remove compaction, so a cached raw
address moves on pool growth AND - worse - can silently point at a *different* entity's component
after any removal (undetectable by ASAN, no crash). Borrow mode is only safe for the
reflection-domain graphs (assets, curves, module trees) mutated through reflected ops; **live
scene state uses re-resolving handles.** The upshot: the natural `_body = entity.get(RigidBody)`
in `onStart`, used every `onUpdate`, is correct by construction - there is no unsafe way to hold
the handle.

Manager-level ops (query/spawn/iterate) go through the NAMED scene-system accessors (`scene.physics`),
the primary manager surface; a generic `scene.manager(Type)` has the same token problem and follows
the same per-type-static pattern (`RigidBody.managerOf(scene)`) or waits (Section 12). **OQ-2
revised:** the "token" is the component's emitted CLASS name (`RigidBody`); the duplicate-name
startup-error rule applies to emitted class names, unchanged. (In Wren, classes are first-class
values, so an `entity.get(RigidBody)` sugar could ride later as a few-line Wren-only alias over the
same resolver - it gates nothing now.)

**A3. Per-module registration + auto-discovery.** Each module registers its reflected script
classes; discovery via `TypeRegistry` / `EnumerateDerived` (our `findAllOf`). Adding a module
surfaces its API with no central edit.

**A4. Facade supersession (not deletion, yet).** Per the "new scripting first" decision:
reflected access lands alongside the facades. When the reflected surface wants a name a facade
holds, **rename the facade out of the way** (the reflected object gets the canonical name). No
facade is deleted in Phase A. Scope-lying facades (`Physics`) are retired only in Phase E once
parity is proven.

**A5. `scene.ui` - access to the scene-tier UI ROOT VIEW** (resolves Fable review note 3 via its
option (a): specify the surface). `ISceneOverlay` is the render layer's scene-tier overlay role
(`RenderApi.cppm`: *"content scoped to a SCENE's rendered output - HUD canvases"*); the `UISubsystem`
already owns one `UIContext` and holds a per-scene `RootView` (`UISubsystem::SceneRoot(scene)`), and
`UICanvasComponent`s attach their instantiated view tree under that scene root. `scene.ui` exposes
that **`RootView`** - code/scripts build a HUD by adding UI components/views under it, which render
on the scene tier. It is deliberately NOT a `setText(id, ...)` document API: **per-control
manipulation from script (`label.text = ...`) falls out for free once the UI toolkit's
`View`/control types are themselves reflected** - i.e. Track A applied to `draconic.ui`, the natural
follow-on. Until then, obtaining the root IS the scriptable surface, and that is enough for now.

**Wiring (OQ-5 ruling - the inversion pattern).** The RootView lives in the `UISubsystem`'s
scene-keyed map (`SceneRoot(scene)`), which is Engine-owned state - putting a borrowed pointer on
the Foundation `Scene` would smear UI onto "Scene is data" and create a second source of truth. So
`ScriptRuntimeBinding` (Foundation) gains ONE hook slot `sceneUiRoot: Function<Variant(scene::Scene*)>`
- typed purely in Variant/scene terms, NO `draconic.ui` dependency. The Foundation `Scene.ui`
property calls the slot; an empty slot returns null (headless runs, dedicated servers, no-UI tests
correct by default). `draconic.engine.ui` installs the implementation (it owns the `Ui` facade +
UISubsystem map) and returns the bound `RootView` handle. This is the CONVENTIONS inversion rule
(capability flows in through installed pointers), as `spawnPrefab` did before `scene.spawn` moved onto
the scene's own `PrefabSpawnSystem` (2026-09-19); `run.ui` later reuses
the same slot pattern, run-scoped.

**Acceptance (honesty bar).** `scene.ui` must *resolve the root*; property/method access on it
**no-ops until the `draconic.ui` View types are reflected** (the named follow-on). A demo must NOT
imply per-control editing works before then - this phase lands the root-resolution seam and the C++
side, nothing more.

**A6. Optional parameter names in reflected methods (small enabling addition - verified NOT
possible today).** `ParamInfo` already carries `const char* name` ("optional; '' when unknown"),
but nothing sets it: `MakeParams` hardcodes `ParamInfo{&ParamTypeOf<A>, ""}` for every parameter,
and all ~131 `.Method<>()` sites pass only the method name (there is no overload, fluent setter, or
names list anywhere). C++ cannot recover parameter names, so add a `Method`/`Constructor` builder
overload that takes them explicitly and writes them into `ParamInfo::name`, e.g.:
```cpp
builder.Method<&Physics::rayCast>("rayCast",
    {"fromX", "fromY", "fromZ", "dirX", "dirY", "dirZ", "maxDistance"});
```
Backends and tooling then bind real names (script signatures, hover/inspector, named args on any
backend that supports them) instead of `arg0..argN`. Opt-in per method and more to maintain - a
deliberate stopgap until C++ static reflection provides names natively. Applies to the gameplay
methods reflected in A1; ships as a tiny P-A1 prerequisite.

**Requirement (final review):** the names-list length is checked against the method arity **at
compile time** (array-reference overload + `static_assert(N == arity)`), never at runtime - a
mismatched list is a programmer error that must not survive to a test run. Naming is **all-or-none
per method**; partial lists are not a thing.

```wren
// A behavior, reflected access - no Physics facade, addresses the RIGHT objects
onStart() {
    var body = _entity.get(RigidBody)   // component handle by type token
    body.mass = 2.0                      // reflected property setter
    body.applyImpulse(0, 5, 0)           // reflected method
}
onUpdate(dt) {
    var move = Input.axis2d("move")                     // Input stays a utility facade
    _entity.get(Character).move(move.x * 5, move.y * 5)  // THIS character, not "the first one"
}
```

---

## 3. Phase B - tier communication (message + event bus, and a tier inbox)

Traktor solves this with named-method `Invoke` + `IRuntimeDelegate`; it has NO generic
string/type-keyed pub/sub. We go one step further with a first-class typed bus, built on
primitives we already have.

**B1. Named inbox on Level and Game.** Extend the `on<Message>` dispatch behaviors already have
to the Level and Game tiers, delivered through the existing `ScriptObject::Invoke(name, args)`
(which already drives their lifecycle). This alone retires the GameManager-on-an-entity pattern.

**B2. The bus is a NATIVE facility keyed by NAME; script is one consumer.** It must NOT deliver
"directly to script" - a C++-only game is first-class and must publish/subscribe with no scripting
present. A scene-owned native `EventBus` keys subscribers on **`StringHash(eventName)`** and
delivers a `Variant` payload; native code calls `Publish`/`Subscribe` with native callbacks.
Delivery is deferred and drained at the tick top-level; a handler's own `emit` is delivered in the
**same** drain (cascaded), bounded to **N=8 passes** with an error log on runaway (the `send`
precedent, explicit). Delivery order within one event = **subscription order** (determinism for
replays/tests). Script participates through a BRIDGE (see Placement) - exactly as physics contacts
reach script via `ScriptPhysicsContactBridge`; the bus itself has zero script dependency.
`run.events` is the same facility, run-scoped, for the Game tier.

**B3. One name-keyed bus serves both C++ and script events (Fable Correction 2 - no second bus).**
The key is a name (`StringHash`), the payload a `Variant`:
- **Native/engine events:** name = the reflected value type's name, payload = the boxed reflected
  value. Unchanged for C++ publishers and cross-backend typed events.
- **Script-authored events:** `scene.events.emit("OrbCollected", payload)` - free-form name,
  payload any Variant-expressible value (numbers, strings, bools, `Entity` handles, reflected
  values). It crosses backends and reaches C++ subscribers uniformly *because it was never a
  script object* - so games author their own events in script with no C++ type per event.
- The bridge fans BOTH to `on<Name>(payload)`; the handler cannot tell (and must not care)
  whether the emitter was C++ or script.

**B4. Subscription - static ships first, dynamic follows with mandatory teardown (OQ-1).** Native
subscribers register a native callback (`Function<void(const Variant&)>`). Script subscribers:
- **Static** `static events { ["OrbCollected", ...] }`, harvested like `properties` (no lifetime
  questions, ~90% case) - ships with P-B2.
- **Dynamic** `subscribe(name, delegate)` (an `IScriptDelegate`) follows in the same phase, but
  ONLY with **mandatory auto-unsubscribe on owner teardown** (behavior destroyed, Level stopped,
  script faulted) - a stored delegate outliving its owner is the `Ui.onClick` lesson; never ship
  subscribe without the teardown sweep. Explicit `unsubscribe(handle)` also exists.

The script bridge is itself just one native subscriber that fans events to the declaring
`on<Name>` handlers.

**Placement (layer-clean, native-first):**
- **Native scene `EventBus`** in `draconic.scene` (Foundation) - a scene-owned facility,
  `Variant` payload keyed by **`StringHash(eventName)`** (matches B2/B3), `core`+`resource` deps
  only. **C++-only games use it directly**, no scripting involved.
- **Script emit is direct, no host hook.** The Foundation bound `Scene` facade already carries a
  `scene::Scene*`, so `scene.events.emit(name, payload)` calls `scene->EventBus().Publish(...)` - both
  Foundation, so like `scene.spawn` today (the scene's `PrefabSpawnSystem` carries the content DB;
  historically it went through a `ScriptRuntimeBinding` hook) it needs NO binding hook. So no `emitSceneEvent` hook is added to `ScriptRuntimeBinding` at all.
- **Script receive is a BRIDGE** in `Draconic.Engine.Script` (mirroring `ScriptPhysicsContactBridge`):
  one native subscriber that fans bus events to the declaring `on<Type>` handlers via
  `ScriptObject::Invoke`; installed by the Engine composition root. The bus stays script-agnostic.
- **Run bus** is the same native facility, run-scoped, in `Draconic.Engine.GameInstance`; its
  `run` facade is out-of-tree (Net pattern). **Foundation never references the run/Game tier.** A
  behavior/Level needing a run event emits a scene event; the Game tier (owning both) relays
  scene->run.

```wren
// Roll Call, done RIGHT: GameController becomes the LEVEL script - no GameManager entity.
// Events are emitted BY NAME with a Variant payload (B3) - no script event class needed.
class Level {
    construct new(scene) { _scene = scene; _total = 6; _collected = 0 }
    static events { ["OrbCollected", "PlayerFell"] }   // subscribe by name (static form)

    onStart() {
        _hud = _scene.ui   // the scene-tier UI root view; build the HUD under it (A5)
        Log.info("Collect all %(_total) orbs!")
    }
    onOrbCollected(amount) {
        _collected = _collected + amount
        Log.info("Collected %(_collected) / %(_total)")   // per-control text edits: A5 follow-on
        if (_collected >= _total) { _scene.events.emit("LevelWon", 0) }
    }
    onPlayerFell(unused) { Log.info("Game over") }
}
```
```wren
// Pickup behavior: broadcast by name - it does not know or find who scores
onUpdate(dt) {
    // ... proximity ...
    _entity.scene.events.emit("OrbCollected", 1)   // a HUD behavior could ALSO subscribe, zero coupling
    _entity.destroy()
}
```

---

## 4. PARKED - authored effects (EventSet)

**Decision (2026-08-04): parked.** The name misleads. Traktor's `EventSetComponent` +
`EventManagerComponent` are NOT a generic event-delivery/invocation mechanism - that is the
Phase B bus, which is payload-agnostic and needs no knowledge of concrete effect types. The
EventSet is instead a *narrow authored fire-and-forget effects* system that must know about
concrete producers (particles, sounds, sub-entities). Because it is that specific - and
because everything communication-related is already covered by Phase B - it is not worth
building now. Recorded here so the analysis is not lost; revisit only if a designer-facing
"fire a named effect" convenience is actually wanted, as its own separate spec.

**How Traktor's works** (for reference only - not being built):
- `IEntityEvent` - an effect *definition* (resource): emit a particle system / play a sound /
  spawn a sub-entity. `createInstance(...)` yields a running instance.
- `IEntityEventInstance` - one live firing; `update()` returns "still alive", plus cancel
  (immediate/end).
- `EventManagerComponent` (world/scene singleton) - OWNS and ticks live instances; `raise(event,
  sender, offset)`, `cancelAll(when)`.
- `EventSetComponent` (per-entity) - a NAMED table of `IEntityEvent`s; `raise("explosion",
  offset)` resolves the name and forwards to the manager.

**Draconic mapping:**
- `IEntityEvent` -> an **effect-definition resource** referencing a particle effect / sound /
  prefab (composing the shipped particles + audio one-shot + `scene.spawn` + decals paths).
- `EventComponentManager` -> a scene-owned component manager (like `RigidBodyComponentManager`)
  that owns + ticks live instances.
- `EventSetComponent` -> a component holding a named map of effect refs.
- Script: `entity.get(EventSet).raise("explosion")` (rides Phase A's `entity.get`).

**Placement:** a NEW Engine module **`Draconic.Engine.Effects`** (component + manager + the
`IEntityEvent` framework + concrete effect types), sitting above Engine.Particles / Engine.Audio
/ prefab-spawn. NOT `draconic.scene` (Foundation, `core`+`resource` only - an effects component
there would invert the layer dependency). Only a pure `IEntityEvent` interface could live low;
the useful concrete effects are Engine-level.

```wren
// Fire an authored effect by name (Phase C) - orthogonal to the event bus
onContactBegin(other, point, normal, speed) {
    _entity.get(EventSet).raise("impactSpark", point)
}
```

---

## 5. Backwards-compat / migration (breaking allowed)
- Phase A/B are additive; nothing breaks yet. Facades may be RENAMED to free canonical names.
- Phase E deletes the scope-lying facades (`Physics`, and any other that misrepresents
  scope/identity). Every `.wren`/`.as` using them breaks and moves to `entity.get(...)` /
  `scene.<system>.*`. Sample scripts + Roll Call are rewritten as the reference migration.
- `entity.send` stays (targeted messaging remains useful); the bus is additive.
- **`run` absorbs the shipped `SceneLoader.*`** (final review fold-in): `run.loadScene` /
  `loadSceneAsync` / `loadProgress` / ... become the canonical level-load surface when the `run`
  root lands (P-B2+); the existing `SceneLoader` follows the A4 supersession path (kept, renamed out
  of the way if needed, deleted in Phase E with the other statics). Two parallel level-load APIs
  must never coexist unplanned.
- **Reserved names:** when P-B2 lands, add `run` to the reserved-name list in
  `docs/design/adding-facades.md` alongside `Level`/`Game`/`SceneLoader`.

---

## 6. Phasing
- **P-A1** the `Method`/`Constructor` param-name builder overload (A6, tiny prerequisite), then reflect one scene-system end-to-end (Physics: `scene.physics` + `entity.get(RigidBody/Character)`, re-resolving handle). Parity is a **TEST that drives BOTH the reflected path and the `Physics` facade against the same scene and compares outcomes** (not an eyeball - it is the migration safety net for Phase E), both backends.
- **P-A2** `entity.get(Type)` bare type tokens + resolve-through-manager (re-resolving handle), duplicate-name startup error; per-module auto-discovery.
- **P-B1** Level/Game named inbox (route `on<Msg>` to `ScriptObject::Invoke`).
- **P-B2** native name-keyed scene event bus + bridge; static subscription first, then dynamic `subscribe` + auto-unsubscribe (same phase); run bus. Rewrite Roll Call as the reference game (GameController -> Level).
- **P-A3 / E** reflect remaining scene-systems + spatial components; retire scope-lying facades; migrate all sample scripts. Keep `Log`/`Time`/`Random`/`Input`/`Audio`-one-shot.
- **PARKED** authored effects (EventSet) - Section 4. Not scheduled.

Ordering rationale: A before B (a bus handler often needs to reach a component).

**Build progress (branch `game-ready-scripting`, 2026-08-05):**
- DONE - A6 param-name overloads + compile-time arity check + tests (commit 2d8603c7, green clang+gcc).
- DONE - the native `entity.get` resolution foundation: `Scene::FindManagerByComponentType` + the
  Correction-1 re-resolution safety suite (commit d354dbc3, green clang+gcc).
- DONE - the core re-resolving primitive: `Variant` RESOLVE mode (type-erased resolver + inline
  context, +8 bytes, no heap; `ToInstance` resolves fresh), commit 9356a43e, green clang+gcc.
- DONE - `Scene::MakeComponentRef` - the `entity.get(Type)` resolver as a RESOLVE Variant, tested
  end-to-end (re-resolves across swap-remove), commit af913fd9, green clang+gcc. **The native core
  of entity.get is complete.**
- DONE - `From<Variant>` pass-through so a reflected method can return a runtime-typed handle
  (commit, green clang+gcc). All CORE enablers for `entity.get` are now in place and committed.
- DONE - OPTION 1 primitives (Fable ruling, Section 12): `Method<Member, ReturnAs>` ReturnType-
  override + validating dispatch (81bc3abb); the general additional-emission-roots API
  (`RegisterExtraScriptRootType`) + Wren collector seeding (9cd480a0).
- DONE - **OPTION 1 proven end-to-end on Wren** (9cd480a0): a component reached only via
  `Gadget.of(entity)` is emitted, the factory returns a re-resolving handle, and `g.power = 5`
  mutates the LIVE component. Green clang+gcc. (An emitted component class also needs its name as an
  extra facade name to be import-visible in behavior preludes.)
- DONE - **OPTION 1 proven cross-backend** (c6d57a43): `Component.of(entity).field = v` mutates the
  live component on BOTH Wren and AngelScript (the ReturnType-override drives AS static boxing).
  Green clang+gcc. The entity.get mechanism is complete and backend-neutral.
- DONE - generic `ComponentOf<T>` factory helper (94bfadb3) - a one-liner per component.
- DONE - **real physics components reachable from script** (97ce9af9): `RigidBodyComponent`/
  `CharacterComponent` reflect `of(entity)`; `RegisterPhysicsScriptFacade` registers + seeds + names
  them; test proves `RigidBodyComponent.of(_entity).friction = 0.5` mutates the live physics
  component. Green clang+gcc. **The P-A1 component-access deliverable is functional on real components.**
- DONE - **`scene.physics` as `ScenePhysics.of(scene)`** (3a07c160): same OPTION 1 factory shape (a
  scene-bound handle is an Engine type, same cross-backend wall as entity.get), reflected value
  returned by value (concrete return, no hook slot). `rayCast`/`gravityY`/`setGravity` on THIS scene's
  world; authored param names (A6). Tests both backends. **This is also the reflected-vs-facade
  parity** - the reflected path is verified against the world ground truth, which is exactly what the
  `Physics` facade reads (both delegate to `PhysicsSceneSystem->World()->Gravity()`).
- DONE - **fuller physics surface** (c6bd189e, 18867a96): per-entity **character control** on the
  component (`CharacterComponent.of(entity).move/jump/grounded/positionY` - component-data ops, fixes
  the static facade's "first character only") + **scriptable impulse** on scene.physics
  (`ScenePhysics.of(scene).applyImpulse(entity, ...)` - a world op keyed by entity). Both backends.
  **Both Roll Call gameplay blockers (per-entity character, no-scriptable-impulse) are now fixed.**
- **P-A1 COMPLETE bar the deferred `scriptName` cosmetic.** Architectural rule established: component
  = auto-reflected DATA (`X.of(entity).field`); scene.physics = WORLD ops keyed by entity
  (`ScenePhysics.of(scene).op(entity, ...)`); the `Type.of(x)` factory is the one cross-backend shape
  for every typed engine handle. Small follow-ups: ray hit-point accessors (need per-scene state).
**Track B (event bus) - IN PROGRESS:**
- DONE - the **native `EventBus`** (838c67a4): scene-owned in `draconic.scene`, StringHash-keyed +
  Variant payload, C++ Publish/Subscribe with native callbacks (C++-only game first-class), drained
  at the scene tick top-level (before transforms), same-drain cascade bounded to 8 passes,
  subscription-order delivery. `Scene::Events()`. Native tests (zero script) cover the C++-only
  contract. Green clang+gcc.
- NEXT - the **script bridge** (engine.script, `ScriptPhysicsContactBridge` pattern): fan bus events
  to `on<Name>(payload)` handlers via `ScriptObject::Invoke`; the Foundation `scene.events.emit(name,
  payload)` facade calls `scene->Events().Publish` directly; static `subscribe` list + dynamic
  subscribe with owner-teardown auto-unsubscribe (OQ-1). Then P-B1: Level/Game named inbox.

## 6b. Tests (required - the C++-only contract is load-bearing)
- **Native bus, Foundation, ZERO script involvement**: publish/subscribe, cascaded same-frame drain, the N=8 bound + runaway log, subscription-order determinism. Proves the C++-only path stands alone.
- **Bridge, both backends**: a script `on<Name>` handler receives a C++-published event, and a C++ subscriber receives a script-published one.
- **Cross-backend round-trip**: Wren emits -> AngelScript receives, and the reverse; payload survives as `Variant`.
- **Teardown / auto-unsubscribe**: a dynamic subscriber whose owner is destroyed / stopped / faulted stops receiving (no stale `IScriptDelegate`).
- **Re-resolving-handle regression suite** (Correction 1): component removed -> handle clean null-ops; ANOTHER entity's component removed (the swap-remove case) -> handle still resolves to the RIGHT component; pool-growth realloc -> still correct.
- **P-A1 parity**: reflected `entity.get(...)`/`scene.physics` vs the `Physics` facade, same scene, compared outcome-for-outcome.

---

## 7. Open questions - ALL RESOLVED (OQ-1..4 Fable 2026-08-04; OQ-5 final review 2026-08-04)
- **OQ-1 (subscription surface):** RESOLVED - both, in order: static `static events {[...]}` list
  ships with P-B2 (harvested like `properties`); dynamic `subscribe(name, delegate)` follows the
  same phase, but only with mandatory auto-unsubscribe on owner teardown + explicit `unsubscribe`.
  (Folded into B4.)
- **OQ-2 (type-token syntax):** RESOLVED - bare names (`entity.get(RigidBody)`); a duplicate bare
  name is a loud STARTUP error at registration. Qualified aliases only if third-party collisions
  become real. (Folded into A2.)
- **OQ-3 (event payloads):** RESOLVED - one name-keyed bus (`StringHash` + `Variant`); C++ events
  use the reflected type's name, script events use a free-form name, both fan to `on<Name>(payload)`.
  Payloads crossing a language boundary must be Variant-expressible. (Folded into B3.)
- **OQ-4 (native subscribers):** RESOLVED - the bus is native from the start; C++-only games are
  first-class publishers/subscribers, and script is a bridge consumer (`ScriptPhysicsContactBridge`
  pattern), never the delivery target. The bus has zero script dependency. (Folded into B2.)
- **OQ-5 (`scene.ui` wiring) - RESOLVED (final review):** neither offered option. The `RootView`
  is Engine-owned (the `UISubsystem`'s scene-keyed map), so a pointer on the Foundation `Scene`
  would smear UI onto "Scene is data". Instead `ScriptRuntimeBinding` gains ONE hook slot
  `sceneUiRoot: Function<Variant(scene::Scene*)>` (no `draconic.ui` dep); `Scene.ui` calls it
  (empty = null), `draconic.engine.ui` installs it - the inversion rule, like `spawnPrefab`. Folded
  into A5, with the acceptance note (root resolves; per-control access no-ops until UI reflection).
  `run.ui` reuses the same slot pattern later.

---

## 8. Reference map
Draconic seams: `TypeBuilder::Method<>` / `MethodInfo` / `InvokeMethod` (Reflection.cppm);
`ScriptObject::Invoke`, `IScriptContext`, `IScriptDelegate` (Draconic.Script);
`ScriptRuntimeBinding` + `GetService(kScriptRuntimeService)` (Draconic.Script.Facades);
`ScriptSceneSystem::DrainMessages`/`EnqueueMessage` (Draconic.Engine.Script/ScriptSubsystem.cppm);
run/Game tier (Draconic.Engine.GameInstance).

Traktor references: `Core/Class/{IRuntimeClass,AutoRuntimeClass,AutoMethod,IRuntimeDispatch,IRuntimeDelegate}.h`;
`Script/{IScriptManager,IScriptContext}.h`, `Script/Lua/ScriptManagerLua.cpp`;
`Runtime/Engine/Stage.{h,cpp}`, `Runtime/GameClassFactory.cpp`;
`World/Entity/{EventManagerComponent,EventSetComponent}.h`, `World/IEntityEvent.h`,
`World/{Entity,World,IEntityComponent,IWorldComponent}.h`;
example game `/home/robert/Dev/CPP/traktorprojects/p1/data/Source/Scripts/Main.xdi`.

---

## 9. Fable REVIEW (2026-08-04): approved with two substantive corrections + OQ rulings

The section-0 reframing is verified-correct (the method machinery exists; the
gap is scope/identity), the layering discipline is exactly right (native bus,
script as a bridge consumer, run facade out-of-tree, Foundation never
touching the run tier), and parking EventSet is the right call for the right
reason. Approved to build - after folding in the following.

### CORRECTION 1 (safety, must change): `entity.get(...)` returns a
### RE-RESOLVING HANDLE, never a borrow

Verified in Component.cppm: component storage is a SPARSE SET - `m_dense` is
a contiguous Array<T> with swap-remove compaction. Component addresses move
on ANY pool growth AND, worse, after any other entity's removal a stale
address can point at a DIFFERENT entity's live component - silent
cross-entity state corruption, undetectable by ASAN, no crash. Native
spawn/despawn does this every frame; the 2b generation guard only covers
REFLECTION-driven mutation and cannot help here.

So the rule, engine-wide: **borrow mode is for the reflection-domain graphs
(assets, curves, module trees - mutated only through reflected ops). LIVE
scene state uses re-resolving handles.** `entity.get(Type)` returns a thin
handle `{ scene, EntityHandle, managerTypeId }` that re-resolves
`manager.Get(entity)` ON EVERY property/method access - exactly how bound
Entity already works (resolve by handle + Live() guard), one O(1) sparse-set
lookup per access, which is fine for script frequency. A dead entity /
removed component resolves to a clean null-op error, same as Entity. The
natural script pattern `_body = _entity.get(RigidBody)` in onStart, used
every onUpdate, is then CORRECT BY CONSTRUCTION - which is the no-gotchas
bar. Cache-the-handle is safe; there is no unsafe way to hold one.

### CORRECTION 2 (the spec contradicts its own example): script-authored events

B3 says bus events are reflected NATIVE value types; the Roll Call example
then defines `class OrbCollected` IN WREN - which by B3's own rule cannot
ride the native bus. Requiring a C++ type per GAME event would gut scripting
usability (games author their events in script). Fix by unifying the key
space instead of adding a second bus:

- The bus keys on **StringHash(eventName)**. Payload is a `Variant`.
- Native/engine events: name = the reflected value type's name, payload =
  the boxed reflected value. Exactly B3, unchanged for C++ publishers and
  cross-backend typed events.
- Script-authored events: `scene.events.emit("OrbCollected", payload)` -
  name is free-form, payload is any Variant-expressible value (numbers,
  strings, bools, Entity handles, reflected values). Crosses backends and
  reaches C++ subscribers uniformly, because it was never a script object.
- The bridge fans BOTH to `on<Name>(payload)` by name - the handler cannot
  tell (and should not care) whether the emitter was C++ or script.
- Rewrite the Roll Call example accordingly (emit by name; the OrbCollected
  Wren class disappears or stays as a local convention the script itself
  unpacks).

### OQ-1 (subscription): both, in this order - static list FIRST

The static `static events { [...] }` harvest form ships with P-B2 (it is
`properties`-style harvesting, no lifetime questions, covers the 90% case).
Dynamic `subscribe(name, delegate)` follows in the same phase ONLY with
mandatory auto-unsubscribe on owner teardown (behavior destroyed, Level
stopped, script faulted) - a stored IScriptDelegate outliving its owner is
the Ui.onClick lesson; never ship a subscribe without the teardown sweep.
Unsubscribe also exists explicitly (`unsubscribe(handle)`).

### OQ-2 (type tokens): bare names, collisions are a STARTUP ERROR

`entity.get(RigidBody)` - bare. The full-descriptive-names convention
already makes our component names unique, we own the whole namespace today,
and the ergonomics win is real. Registration detects duplicate bare token
names and fails LOUDLY at startup (not first-use). If a collision ever
becomes legitimate (third-party modules), add qualified aliases THEN -
do not tax every script now for a future that may not come.

### Smaller notes (fold in, no discussion needed)

1. **Drain semantics**: specify same-frame cascaded drain (a handler's emit
   is delivered in the same drain), bounded to N=8 passes with an error log
   on runaway - the `send` precedent, made explicit. Delivery order within
   an event = subscription order (determinism for replays/tests).
2. **Pick ONE root name: `run`.** The spec alternates run/game; `game`
   invites confusion with the mandatory `Game` class (the SceneLoader
   name-clash lesson - and `Game`-adjacent names are now a reserved family).
3. **`scene.ui` appears only in an example** and is defined nowhere - either
   specify it in Phase A (the scene's UI scene-system surface) or fix the
   example to use the existing Ui facade. Do not let surface sneak in
   through examples.
4. **Missing tests section** - add one: native-bus tests in Foundation
   (publish/subscribe/cascade/order, ZERO script involvement - the C++-only
   contract is load-bearing); bridge tests both backends; a cross-backend
   event round-trip (Wren emits, AngelScript receives, and reverse);
   teardown/auto-unsubscribe; the re-resolving-handle regression suite
   (component removed -> clean null-op; ANOTHER entity's component removed
   -> handle still resolves to the RIGHT component - the swap-remove case;
   pool-growth realloc -> still correct).
5. **P-A1 parity bar**: "prove parity with the Physics facade" should be a
   TEST that drives both paths against the same scene and compares outcomes,
   not an eyeball - it becomes the migration safety net for Phase E.

With corrections 1+2 folded in, build in the spec's phase order.

---

## 10. Fable FINAL review (2026-08-04): CLEARED TO BUILD, with the OQ-5 ruling + three fold-ins

The refinement is faithful: corrections 1+2 are correctly integrated (A2's
re-resolving handle with the swap-remove rationale, B3's name-keyed bus with
a legal example), the tests section covers the load-bearing contracts, and
the new A5/A6/screen-tier content is well-reasoned. Verdict: build in phase
order. Remaining items, all small:

### OQ-5 RULING: `scene.ui` wires through a HOOK SLOT (the inversion pattern)

Neither of the two offered options as stated; the deciding fact is that the
scene root does not live ON the Scene - it lives in the UISubsystem's
scene-keyed map (`SceneRoot(scene)`), which is Engine-owned state. Storing a
borrowed root pointer on the Foundation Scene object would smear UI state
onto "Scene is data" and create a second source of truth beside the map. So:

- `ScriptRuntimeBinding` (Foundation) gains ONE slot:
  `sceneUiRoot: Function<Variant(scene::Scene*)>` - typed purely in
  Variant/scene terms, NO draconic.ui dependency.
- The Foundation `Scene` facade's `ui` property calls the slot; empty slot =
  null (headless runs, dedicated servers, and no-UI tests are correct by
  default - same guard style as every facade).
- `draconic.engine.ui` installs the implementation (it already owns the Ui
  facade, UiScriptHost, and the UISubsystem map) returning the bound
  RootView handle.

This is exactly the CONVENTIONS inversion rule (capability flows in through
installed pointers) and mirrors how spawnPrefab already works. It also
keeps `run.ui` trivial later: same slot pattern, run-scoped.

### A5 honesty note (acceptance, not design)

Until the draconic.ui View/control types are reflected (the named follow-on),
a script holding `scene.ui` can do almost nothing with it - that is fine and
the surface is still worth landing (it fixes the root-resolution seam and
the C++ side), but A5's ACCEPTANCE must say "the root resolves; property
access on it no-ops until UI reflection lands" - do not let a demo imply
per-control editing works before it does.

### A6 approved, with a compile-time arity check

The param-names overload is verified-needed (ParamInfo::name exists, nothing
sets it) and the explicit-list design is right. Requirement: the names list
length must be checked AGAINST THE METHOD ARITY AT COMPILE TIME
(array-reference overload + static_assert on N == arity), not at runtime - a
mismatched list is a programmer error that must not survive to a test run.
Partial naming is not a thing: name all or none per method.

### Fold-ins (mechanical)

1. STALE LINE: Placement bullet 1 still says "Variant payload keyed by
   reflected type" - update to "keyed by StringHash(eventName)" to match
   B2/B3 (the one spot the refinement missed).
2. `run.loadScene` (Section 1 table) overlaps the SHIPPED `SceneLoader.*`
   facade. Decide it now so two parallel level-load APIs never coexist
   unplanned: `run` ABSORBS SceneLoader - `run.loadScene/loadSceneAsync/
   loadProgress/...` become the canonical surface when the `run` root lands
   (P-B2 or later), and `SceneLoader` follows the A4 supersession path
   (kept, renamed out of the way if needed, deleted in Phase E with the
   other statics). Add to the Phase E migration list.
3. Reserved-name list: add `run` alongside `Level`/`Game`/`SceneLoader` in
   docs/design/adding-facades.md when P-B2 lands.

---

## 11. DESIGN CHALLENGE for Fable (2026-08-05): entity.get cross-backend shape

**Status:** OPEN - blocks the P-A1 script layer. The native core of `entity.get` is DONE and
committed (5 commits, green clang+gcc: A6, FindManagerByComponentType + safety, Variant RESOLVE
mode, MakeComponentRef, From<Variant> pass-through). Only the *script-facing* shape is in question,
because a deep emitter audit found that two resolved assumptions are infeasible as written.

### The emitter reality (audited, with refs)
- **Bare token globals are not possible today.** Every top-level script identifier is a *class*;
  there is no global-constant mechanism on either backend - no `RegisterGlobalProperty` in the
  AngelScript emitter, no Wren global-variable injection (`WrenScript.cppm` prelude only imports
  class names; `AngelScriptScriptImpl.cpp` only `RegisterGlobalFunction`). So `entity.get(RigidBody)`
  with `RigidBody` a bare token value cannot work without NEW machinery on both backends. This
  contradicts OQ-2's "bare names" ruling, which assumed feasibility.
- **Components are not script-visible.** `DRACONIC_REFLECT_VALUE` only patches `TypeOf<T>()` in
  place; it never enters the registry (`Reflect.h`). AngelScript emits every *registry* type
  (`RegisterTypes.cppm` feeds `registry.All()`), so components need `GlobalTypeRegistry().Register`.
  Wren emits only constructor-seeded or statically-reachable types (`CollectEmittableTypes`,
  `WrenScript.cppm:538-625`); `entity.get` returns an opaque Variant that reaches nothing, and there
  is NO extra-root API - I'd add one.
- **AngelScript cannot do a generic `entity.get`.** Return boxing keys on the *declared* return type
  (`SetGenericReturn`, `AngelScriptScriptImpl.cpp:951`), and there is ZERO cast infrastructure (no
  `opCast`/`asBEHAVE_REF_CAST` anywhere). One `entity.get(...)` returning varied component types is
  impossible in AS without new cast machinery. Wren wraps returns by dynamic `Type()`
  (`WrenScript.cppm:244`), so Wren has no such problem.
- **Payoff is real once visible:** a component's `mass` property auto-binds get/set on both backends
  the moment its type is an emitted class (Wren `WrenScript.cppm:1288`, AS `...Impl.cpp:1650`).

### The three options (Opus put to the user; user leans OPTION 3, wants Fable's ruling)
1. **Per-type static factory** `RigidBody.of(entity).mass = 2.0`. Statically-typed return = the
   component type, so it works on BOTH backends with no token/cast/global machinery. Cost: register +
   Wren-seed the components, and a small reflection addition - a method whose *declared* reflected
   return type is the component while the body returns the RESOLVE Variant (the C++ returns Variant;
   the reflected ReturnType must be overridden to the component type). Cleanest cross-backend answer.
2. **Wren-first `entity.get("RigidBody")`** (name-string token - the only feasible token now). Ships
   the primary backend fully this pass; AngelScript generic-get deferred (needs the cast machinery).
   Matches the "likely Wren" lean; smallest immediate step.
3. **Full spec vision** - build the new machinery on both backends: global-constant token
   registration (Wren + AS) for bare `entity.get(RigidBody)` AND AngelScript ref-cast dispatch keyed
   on the box's dynamic `Type()`. Closest to the written spec; the largest new-emitter surface and
   the highest risk. **This is the user's lean.**

### What Fable is asked to rule
- Is OPTION 3's machinery worth it vs OPTION 1's ergonomics, given OPTION 1 is cross-backend for far
  less code? (OPTION 1 changes the authoring syntax from `entity.get(RigidBody)` to `RigidBody.of(entity)`.)
- If OPTION 3: confirm the two new mechanisms (global-const token registration on both backends;
  AS `opCast`/`REF_CAST` keyed on `BoxedVariant::value.Type()`) and whether they should be their own
  phase before the components are wired.
- Either way: bless adding a **Wren extra-root API** + `GlobalTypeRegistry().Register` for the
  components (needed by every option), and revisit OQ-2 (bare-token feasibility) + the "both backends"
  acceptance line in light of the audit.

---

## 12. Fable RULING on the entity.get shape (2026-08-05): OPTION 1, and here is the decisive argument

The audit is correct and changes the facts under OQ-2 - the ruling is revised
accordingly, openly: bare value tokens do not exist in either backend, and
that was my assumption, not the emitters'.

### Why option 1 (with respect to the option-3 lean): option 3 buys nothing in EITHER backend

Walk the payoff per backend, because it is not symmetric:

- **AngelScript:** even WITH global-const tokens AND ref-cast machinery,
  `entity.get(RigidBody)` must DECLARE a return type - AS has no
  return-type-from-argument-VALUE inference. The declared return is a base
  handle, so every use is `cast<RigidBody>(entity.get(RigidBody))` - the type
  named TWICE plus cast noise. Option 3 makes AngelScript ergonomics WORSE
  than option 1's `RigidBody@ body = RigidBody.of(entity)`, which is typed in
  one mention with zero casts. (Per-token typed overloads of `get` could fix
  that, but that is a THIRD new mechanism - N synthetic overloads injected
  into the Entity registration - on top of the two option 3 already needs.)
- **Wren:** dynamically typed, so `entity.get(RigidBody)` vs
  `RigidBody.of(entity)` is a pure spelling difference - zero capability
  delta. Option 3's entire machinery (two new emitter mechanisms, the
  largest-risk item in the audit) purchases a syntax preference in one
  backend and a regression in the other.

`RigidBody.of(entity)` is also not a consolation syntax: it IS the typed
GetComponent (Unity's `GetComponent<RigidBody>()` with the type in the same
position), it autocompletes from the type, and the component class becomes
the natural future home for manager-level statics. So: **OPTION 1.**
Option 2 is rejected outright - it splits backend parity on the core
surface, against the proven neutrality principle.

### Not foreclosed, on the record

Option 1 loses nothing permanently. In Wren, classes are already first-class
VALUES - if a class-object->typeId mapping is ever added to the emitted
component classes, `entity.get(RigidBody)` becomes a few-line Wren-only
SUGAR over the same resolver, no global-const machinery involved. If that
sugar is ever wanted, it rides then; it gates nothing now.

### Blessed (needed by every option)

- `GlobalTypeRegistry().Register` for component types + the Wren extra-root
  seeding API (make it a GENERAL "additional emission roots" API on the
  collector, not a component special case - the UI View reflection follow-on
  will want the same door).
- The ReturnType-override on a reflected method (declared return = component
  type, body returns the RESOLVE Variant) - with one constraint: the
  dispatch VALIDATES that the resolve Variant's runtime type matches the
  declared return (mismatch = clean null + error log, never a type-confused
  handle). `From<Variant>` passthrough (adb08780) composes with this.

### Consequential updates

- OQ-2 REVISED: "bare names" now means the component's emitted CLASS name is
  the single token surface (`RigidBody.of(...)`); the duplicate-name
  startup-error rule applies to the emitted class names, unchanged.
- The generic `scene.manager(Type)` in A2 has the same token problem - it
  follows the same pattern (a per-type static, e.g. `RigidBody.managerOf(scene)`)
  or waits; the NAMED scene-system accessors (`scene.physics`) are unaffected
  and remain the primary manager surface.
- The "both backends" acceptance line STANDS - option 1 satisfies it
  symmetrically; the parity test drives `RigidBody.of(entity)` on both.

---

## 13. DESIGN CHALLENGE for Fable (2026-08-05): dynamic `subscribe` owner-teardown

**Status:** OPEN - the LAST Track B piece. Everything else in Track B is DONE and committed
(green clang+gcc): native `EventBus`; `scene.events.emit(name, payload)` facade (overloaded
none/f64/String/bool/Entity + a **generic `Variant` sink** so any reflected value rides the bus -
a raw-`Variant` reflected param needed an `AcceptArg`/`ConvertArg` passthrough + Wren
`SlotMatchesParam` + AS `?&in`); the **script bridge** (a per-scene `ScriptEventSubscriptions`
fans a fired event to every declaring behavior AND the Level, P-B1); event names DERIVED from the
harvested `on<Event>` handlers (no separate `events` list); DIRECT dispatch at bus-drain time (the
emit->publish->drain-later path already gives the deferral `entity.send` needs its queue for);
`Clear()` on scene stop. Roll Call is rewritten on it (Level scores via events, no GameManager entity).

The OQ-1 ruling was "static-first, THEN dynamic `subscribe(name, delegate)` with **mandatory
auto-unsubscribe on owner teardown** (the `Ui.onClick` lesson)." The static/declared half shipped.
The dynamic half hits a wall worth a ruling.

### The wall: mandatory owner-teardown vs the no-ambient-state design
- The facade layer is deliberately **ambient-free**: every handle carries its own scene/entity ptr
  and works from any call site (update, onDestroy, a stored callback, a coroutine). There is NO
  "currently-executing behavior" the runtime tracks (confirmed: no such state in ScriptSubsystem or
  the facades; the Scene/Entity handles all comment "never ambient").
- So a bare `scene.events.subscribe(name, fn)` **cannot know who is subscribing**, and therefore
  cannot auto-unsubscribe when *that owner* (a behavior on one entity) is destroyed mid-run while the
  scene keeps running. The `IScriptDelegate` self-detaches only on **VM/context teardown** (scene
  stop / host teardown) - NOT on a single entity's destroy (its closure stays alive because the bus
  RefPtr holds it). That is exactly the `Ui.onClick` leak the ruling wants to prevent.
- `Ui.onClick`'s own answer: the delegate is held by the **button view**, so destroying the view
  drops it. The owner there is a concrete object with a lifetime. The bus has no such owner unless we
  give the subscription one.

### The three options (Opus leans OPTION A; wants Fable's ruling)
A. **Entity-scoped `entity.subscribe(name, fn) -> handle`.** The owner IS the entity - its handle is
   explicit (no ambient state), so the ScriptSceneSystem keys dynamic subs by owning entity and
   auto-unsubscribes them in `StopBehavior`/`ReleaseComponentInstances` (true per-behavior teardown,
   the strong "mandatory" form). The Level (not an entity) uses `scene.events.subscribe(...)` cleared
   on scene stop. Cost: an entity->sub-handles registry + teardown hook; two spellings (entity vs
   scene). This is the only option that delivers *mandatory* per-owner auto-unsub within the design.
B. **Scene-scoped `scene.events.subscribe(name, fn) -> handle` + explicit `unsubscribe(handle)`**,
   auto-cleared on scene stop (backstop vs cross-run leaks) but NO per-behavior auto-teardown - a
   behavior that subscribes MUST `unsubscribe` in `onDestroy`. Simplest, matches the written surface,
   but it is *explicit* teardown, not mandatory - it reintroduces the `Ui.onClick` footgun the ruling
   named.
C. **Defer dynamic subscribe.** The declared `on<Event>` path already covers Roll Call and the common
   case with zero wiring and correct auto-teardown (a destroyed behavior simply stops being iterated).
   Ship nothing speculative; add dynamic runtime-closure subscription when a concrete need appears.

### What Fable is asked to rule
- Does the "mandatory auto-unsubscribe" ruling REQUIRE OPTION A (entity-scoped owner), or is OPTION
  B's explicit-unsubscribe + scene-stop-backstop an acceptable read of "mandatory" given the
  ambient-free design? Or is OPTION C (defer) right until a use case forces it?
- If A: bless the entity-scoped spelling `entity.subscribe(name, fn)` alongside the Level's
  `scene.events.subscribe`, and the entity->handles teardown registry in the ScriptSceneSystem.
- If A or B: confirm the delegate param is `RefPtr<IScriptDelegate>` (already the reflected callback
  currency; both backends wrap a fn/funcdef into one) and that Invoke-at-drain-time (no VM active) is
  the right delivery point, same as the declared path.

---

## 14. Fable RULING on dynamic subscribe (2026-08-05): A is the DESIGN, C is the SCHEDULE

First, ratifying a deviation worth naming: deriving event subscriptions from
the harvested `on<Event>` handlers instead of a separate `static events` list
is BETTER than my B4 ruling - the handler IS the subscription, there is no
list to forget to update, and teardown is mandatory by construction (a
destroyed behavior simply stops being iterated). Good call; B4 is amended to
the derived form.

### The ruling

- **B is REJECTED, permanently.** Explicit-unsubscribe-or-leak is the
  `Ui.onClick` footgun with a scene-stop backstop taped over it. "Mandatory"
  in OQ-1 means the API cannot be used incorrectly - B can. It does not
  matter that it matches the originally-written surface; the surface was
  written before the ambient-free consequence was visible.
- **A is the blessed DESIGN.** Opus' analysis is exactly right: the
  ambient-free rule means the owner must be EXPLICIT, and
  `entity.subscribe(name, fn) -> handle` puts the owner in the call the same
  way `RigidBody.of(entity)` puts the receiver in the call - the two
  decisions are the same principle. Entity-keyed registry in the
  ScriptSceneSystem, auto-unsubscribed in StopBehavior /
  ReleaseComponentInstances; the Level's dynamic subs hang off
  `scene.events.subscribe` with the scene-stop clear as their owner
  teardown (the Level's owner IS the scene). Confirmed on the two details:
  `RefPtr<IScriptDelegate>` is the callback currency, and drain-time Invoke
  (no VM active) is the delivery point, same as the declared path.
- **C is the SCHEDULE.** Do not build A now. The derived declared path
  covers every use case anyone has named - including Roll Call - with
  teardown that cannot be gotten wrong; the workarounds for the exotic cases
  (conditional handling = a state check inside the handler; data-driven
  names = naming discipline) are trivial. Building A today adds a second way
  to subscribe with no consumer, which is documentation surface and test
  surface for nothing. This is NOT a vague deferral: the design above is
  complete - when the first concrete need for a runtime-computed
  subscription arrives, build A exactly as written here, zero new design
  rounds. Track B closes NOW on the declared path.

### OQ-1, final form

Static/declared: SHIPPED (derived from handlers - the improved form).
Dynamic: designed (option A, this section), scheduled on first concrete
need. Nothing with a teardown footgun ever ships - by construction on the
declared path, by explicit-owner design on the dynamic path when it comes.

---

## 15. DESIGN CHALLENGE for Fable (2026-08-05): AngelScript value-member resource properties

**Status:** OPEN, non-blocking - Track A phase 2 (audio) shipped; this is a parity papercut found
in passing. Handle-member resource properties WORK on both backends today (committed + tested); the
question is whether the bare value-member form is worth chasing.

### The gap
An editor-picked asset is delivered to a behavior as a resource id: a property typed `asset:Mesh`
marshals to a `Guid` (`PropertyValueToVariant` -> `Variant::From<Guid>`). The neutral apply path
Invokes `<name>=` with that Guid on the instance.
- **Wren:** the author writes a setter method `res=(v) { _res = v }`; it runs in-VM and stores the
  Guid. Works. A plain "member" in Wren is just a field the setter assigns.
- **AngelScript:** there is no setter method; the neutral path writes the member FIELD directly
  (`SetMemberField` -> `WriteTypedAddress`). This works for a HANDLE member `Guid@ res;` (the slot is
  a `BoxedVariant*` we set) but NOT for a VALUE member `Guid res;`.

### Root cause (instrumented, on the record)
Every reflected type is registered `asOBJ_REF` (a `BoxedVariant` box). A value member `Guid res;`:
- reports `GetPropertyTypeId` as an APP-OBJECT-by-value (obj bit set, OBJHANDLE clear);
- its slot is NULL right after `CreateInstance` (AngelScript defers construction);
- a native write of a box pointer into the slot "succeeds" but is DISCARDED - AngelScript lazily
  (re)constructs a default box for the value member before the first script access, so `res.field`
  reads the default. No fault, just a silently-default value (which is why phase 2's first AS test
  failed until it switched to constructing the Guid inline).
A HANDLE member `Guid@ res;` is null-initialized, stays the pointer we set, and reads back correctly.

### Current resolution (committed e0bc70ec)
- `WriteTypedAddress` writes reflected-type HANDLE members (unchanged); a reflected VALUE member is
  rejected and `SetMemberField` logs an ACTIONABLE warning ("property 'x' is a value member; declare
  it as a handle 'Type@ x'"). Regression test proves a `Guid@` handle property applies end-to-end.
- Net: AngelScript resource/reflected properties must be declared as handles (`Mesh@ mesh;`,
  `Guid@ clip;`) - idiomatic AngelScript for reference types anyway. Scalars/strings/enums are
  unaffected (they are value members and always worked).

### What Fable is asked to rule
Is the handle requirement the right resting place, or should bare value-member parity be built?
Options:
1. **Accept the handle requirement (current).** Zero further code; idiomatic AS; runtime warns on
   misuse. Cost: an asymmetry with Wren (a plain member works there) and a runtime-not-compile signal
   - an AS author who writes `Mesh mesh;` for a resource property gets a warning + a silently-unset
   member, not an error.
2. **Make value members apply via in-VM assignment.** For a reflected value member, the neutral
   setter stops poking raw memory and instead runs an AngelScript assignment (construct the box +
   opAssign) through the engine during property-apply. Robust parity; cost: executes script during
   apply, more backend code, and apply already runs at instantiate so re-entrancy must be checked.
3. **Cook-time enforcement.** The AngelScript property scanner flags a reflected-type property
   declared as a value member as a COOK ERROR (or auto-normalizes it to a handle), turning the
   runtime warning into an authoring-time failure. Cheaper than (2), makes the rule explicit, still
   asymmetric with Wren.

Opus leans 1 (handle is idiomatic + already works; the warning covers the footgun) with 3 as a cheap
upgrade if the runtime-vs-compile signal matters; 2 only if bare value-member parity is a hard
requirement. Wants Fable's read.

---

## 16. Fable RULING (2026-08-05): option 1 + option 3 TOGETHER; option 2 rejected

The handle requirement is the right resting place - but only WITH the
cook-time gate. Separately they are each half an answer:

- Option 1 alone fails the no-gotchas bar: "runtime warning + silently
  default member" means a game RUNS with a null mesh while the author greps
  a log. That is precisely the failure mode this track exists to kill.
- Option 3 turns the mistake into an authoring-time ERROR (not
  auto-normalize - rewriting the author's declaration at cook is magic;
  error with the exact fix in the message: "declare 'mesh' as a handle:
  'Mesh@ mesh;'"). The scanner already harvests properties, so the check is
  cheap and sits where the author is looking. Keep the runtime warning as
  defense-in-depth for non-cooked paths (dev hot-reload, consoles).

Option 2 is rejected on principle, not just cost: it builds VM-execution-
during-apply machinery (with instantiate-time re-entrancy questions) to
legitimize a spelling that is NON-idiomatic AngelScript in the first place -
`Type@` IS how AS declares reference types; `Mesh mesh;` for a ref-registered
type is the unusual form. The Wren asymmetry is surface-level: each backend's
IDIOMATIC member spelling works; we are not obligated to make every
unidiomatic spelling work identically across languages with different type
systems.

Fold-in: the rule ("AS resource/reflected properties are handles") goes in
docs/design/adding-facades.md next to the other AS gotchas, and one cook
test proves the error fires with the actionable message.

## 17. GAP for Fable (2026-08-08): CharacterComponent vs trigger/sensor contacts

Surfaced while closing the SampleGame FINDINGS list (finding 3). A dynamic
rigid body falling through a sensor raises `onTriggerEnter` (proven), but a
`CharacterComponent` WALKING into a sensor raises nothing. This is now a known
gap with a regression guard, not a silent hole.

Root cause (audited): the trigger event stream is produced by the world's
RIGID-BODY contact listener (`PhysicsWorldImpl.cpp` `OnContactAdded`, which maps
`a.IsSensor() || b.IsSensor()` to `ContactKind::TriggerEnter`). A
`CharacterVirtual` is not a body in that solver - it is a swept capsule advanced
by `ExtendedUpdate` (`UpdateCharacter`), so its overlaps never reach that
listener. Sensors also carry no collision response, so the character's own
collision resolution has no reason to surface them either. Net: zero
character->sensor events, regardless of the collision matrix.

Evidence in tree: `PhysicsSceneTests.cpp` has
"a CharacterComponent walking into a trigger does NOT yet raise a TriggerEnter
event (KNOWN GAP, spec 17)" asserting the CURRENT no-report behavior. When this
is implemented that test FLIPS (starts failing) - update it to assert the event
fires, and delete this section.

Proposed shape (for Fable to rule on):
- After `ExtendedUpdate`, read the character's contacts (Jolt
  `CharacterVirtual::GetActiveContacts()`, or install a `CharacterContactListener`)
  and keep a per-character set of sensor body ids currently overlapped.
- Diff against last step -> emit `ContactKind::TriggerEnter` for newly overlapped
  sensors and `TriggerExit` for dropped ones, into the SAME `m_events` stream the
  bridge already drains, resolved to entities via the packed userData (same path
  rigid-body triggers use). Reuse `TriggerEnter`/`TriggerExit` - a character is
  just another shape entering a volume as far as gameplay cares.
- Ensure the character actually DETECTS Trigger-layer bodies during the sweep
  (today it filters with `layers::From(PhysicsLayer::Dynamic, 0)`); the collision
  matrix / broadphase filter must let Dynamic-vs-Trigger overlaps through, or the
  contacts never appear to diff.

Questions for Fable:
1. Character contacts via the character's own listener/`GetActiveContacts`, or a
   separate broadphase overlap query per character each step? (The former reuses
   the sweep Jolt already does; the latter is a second query but decouples from
   `ExtendedUpdate`'s filtering.)
2. Enter/exit bookkeeping lives per-CharacterComponent (transient set) or in the
   world impl keyed by CharacterId? (Mirrors where rigid-body pair state lives.)
3. Does a character entering ANOTHER character's trigger-less capsule matter, or
   is this strictly character-vs-sensor for v1?

---

## 17. KNOWN GAP: CharacterComponent does not raise trigger events (recorded 2026-08-08)

A `CharacterComponent` walking into a sensor volume raises NO
`TriggerEnter`/`TriggerExit`. Cause: the trigger stream comes from the
physics world's RIGID-BODY contact listener, and a `CharacterVirtual` is a
swept capsule outside that solver - its sensor overlaps never reach the
listener. A guard test asserts the CURRENT behavior
(`Engine.Physics.Tests/PhysicsSceneTests.cpp`, "walking into a trigger
raises NO TriggerEnter"); when character->sensor contacts are implemented,
that test FLIPS to assert the event fires and this section is deleted.

Candidate fix (when wanted): after each character step, overlap-query the
character capsule against sensor bodies and synthesize
TriggerEnter/Exit pairs into the same contact stream (Jolt exposes the
CharacterVirtual contact callbacks / a shape overlap query for this).
Gameplay workaround until then: give the trigger logic to the volume side
(a kinematic body contact with a rigid proxy), or poll distance in a
behavior.
