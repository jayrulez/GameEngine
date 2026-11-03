# Scripting + runtime shape: retire facades, one bus, cleaner god object

> Status: UNDER EVALUATION (ideas, not a decision). Author: Opus, 2026-08-22, from a design
> exchange after farming the Traktor engine (jayrulez/traktor fork) and its two sample games
> (apistol78/zombie, apistol78/kartong, cloned under traktor/bin). Next: Fable review -> user
> decision -> a build spec in Documentation/Specs.
>
> Scope: how scripts reach the engine (facades vs reflection), how the Game/Level/behavior tiers
> are driven and talk to each other, and where networking lives. NOT a spec - it records the
> directions we settled on and the leanings, so Fable can turn them into a spec with rulings.

## Why this exists

We already have a reflection system comparable to Traktor's, yet our script surface is a large
hand-written facade layer while theirs is auto-reflected. We farmed Traktor to understand the gap
and decide what to adopt. The findings below are evidence-backed (file:line); the ideas after them
are our leanings, argued through in the exchange.

## Findings

### Traktor (what "more organized" actually is)

- **One generic reflection bridge, zero per-API facades.** `AutoRuntimeClass<T>` generates the
  arg-unbox/call/rebox stub from a member-function pointer at compile time
  (traktor `code/Core/Class/AutoMethod.h:52-66`). The only hand-authored text is a declarative
  `addMethod("name", &Class::method)` list in per-module `*ClassFactory.cpp`, pointing at the REAL
  class (`code/World/WorldClassFactory.cpp:188-200`). `ScriptServer` finds every factory by RTTI
  and funnels all classes through one Lua bridge (`code/Runtime/Impl/ScriptServer.cpp:63-70`);
  `import(traktor)` copies that namespace into script globals. No wrapper classes, no
  service-resolution stubs, no name aliases.
- **Scripts subclass the native component.** `EnemyController = ... class("EnemyController",
  traktor.world.ScriptComponent)` (zombie `EnemyController.xdi:41`). The native `world::ScriptComponent`
  is reflected like any class; the Lua `class(name, super)` helper flattens its members; the native
  component and the script instance are the SAME object (the constructor is passed `this` and the Lua
  table carries the native pointer as light-userdata) (traktor `ScriptComponent.cpp:112-117`,
  `ScriptManagerLua.cpp:619-690`). Value types (`Vector4`) are the second mode: boxed, copied, no
  identity.
- **No god DRIVER; there IS a god ACCESSOR.** Three separate big objects: `Application` (frame pump,
  script-agnostic), `IEnvironment` (the aggregate accessor of every server - physics/render/input/
  audio/resource/world/online - injected everywhere and set as a script global,
  `ScriptServer.cpp:163`), and `Stage` (the per-screen gameplay + orchestrator-script host,
  disposable each transition). Entity scripts reach out via inherited `self.owner`/`self.world`, the
  injected `context` (= the Stage) handed to `update(context, total, delta)`, and
  `context.environment.*` for services/resources (zombie `Main.xdi`, `EnemyController.xdi:658`).
- **Networking is decoupled leaves, opt-in from script.** `Net` (transport), `Online`
  (session/lobby, app-owned but config-gated), `Jungle` (the `Replicator`, zero in-tree consumers).
  Games construct a `Replicator` and tick it themselves; nothing in the runtime constructs or ticks
  it. Both sample games ship single-player, online disabled.

CORRECTION / bound on the ambition (user note, 2026-08-22): part of why Traktor feels "facadeless"
is NOT reflection at all - **they implement whole frameworks (notably their UI) IN Lua**, so those
are just script classes a game uses natively, no binding surface to write. They can afford that
because they have exactly ONE scripting backend. We are deliberately MULTI-BACKEND (AngelScript +
Luau [[scripting-backend-neutrality]]), so we CANNOT write a framework in one script language without
either abandoning neutrality or reimplementing it per backend. That is a real, permanent cost of
backend neutrality: our higher-level frameworks (UI, etc.) stay NATIVE and are reached from script via
reflection/`.of`, where Traktor's are script all the way down. So "retire facades" for us means the
facade LAYER over native APIs (adopt `.of`/direct reflection) - it does NOT mean frameworks become
script-native. It is unknown whether Traktor's own approach would even be facadeless if their UI were
native like ours; their facadelessness there is a single-backend affordance, not purely a reflection win.

### Us (current state)

- **Facades are a hand-written wrapper layer.** ~15 registrars behind `RegisterAllScriptFacades()`
  ([ScriptSurfaceImpl.cpp:32-52], tripwire `kSubsystemFacadeNameCount = 32`), ~387
  `builder.Method/Property` call-sites, ~233 `REFLECT_*` bodies. For the `run`/`ui`/`Input`/`Net`
  surfaces we write a SEPARATE wrapper class (`class Run : public Object`,
  [GameInstance.cppm:114-188]) whose methods are resolve-service-then-forward stubs, registered one
  by one, with a `scriptName` alias.
- **Reflection already auto-binds - over a curated set.** `BindType()` walks `TypeInfo` and emits
  AngelScript registrations generically ([AngelScriptScriptImpl.cpp:1952]; Luau mirrors it). But the
  type set is hand-curated: a native class is script-visible only if someone wrote a facade/value type
  and hand-registered it. The one place we already do it "their way" is components: `.of(entity)`
  projects a component's editor-reflected properties to script ([RenderComponentsImpl.cpp:365-384]).
- **GameInstance is an intentional god object** ([GameInstance.cppm:245], `: public
  net::INetworkController`): owns the run host + `Game` object + run bus + net endpoint + input
  runtime + scene group + level-load orchestration; drives the Game tier (`TickScript`,
  [GameInstanceImpl.cpp:403]). Scene-behavior ticking is already delegated to `ScriptRunHost` +
  per-scene `ScriptSceneSystem` (good).
- **Networking breaks the subsystem pattern.** Every sim subsystem (physics/audio/anim/particles/
  nav/input) is a `foundation::runtime::Subsystem` that self-ticks under the Context. `NetworkSubsystem`
  is an empty shell ("does no per-frame work"); the live endpoint lives on GameInstance (which IS the
  `INetworkController`), and `DriveNetwork()` is hand-pumped by the app ([DefaultApplicationImpl.cpp:455]).
  ~28 net refs in the app impl, ~24 in GameInstance impl.
- **Fixed lane moved to per-scene.** The old context-level fixed lane was deleted in the FrameTime
  cutover - "zero subsystems overrode FixedUpdate" ([RuntimeContext.cppm:102-106]). The runtime
  `Subsystem` has only BeginFrame/Update/PostUpdate/EndFrame. Fixed stepping is now per-scene:
  `SceneSystem::OnFixedUpdate`, each Scene owns a `FixedStepper`, physics rides it as
  `PhysicsSceneSystem` ([PhysicsSubsystem.cppm:86,182], [SceneImpl.cpp:602,656-664]).
- **One EventBus type, two scoped instances.** `scene::EventBus` backs both the scene bus
  (`Scene::m_events`) and the run bus (`GameInstance::m_runEvents`) - "the SAME native scene::EventBus
  type" ([GameInstance.cppm:434]). What we layered on top is a CONVENTION: the explicit scene->run
  relay. So we do not have two messaging systems; we have one system at two scopes plus a manual relay.

## The ideas (with leanings)

### 1. Keep the god object; separate its roles

GameInstance stays the god object - "the running game," the one place everything is owned and the
driver of the Game tier. That is the right shape. The discipline to add, taken from Traktor's split
(`Application` / `environment` / `Stage`):

- **A god object should OWN/COMPOSE concerns, not BE every role.** Undo `GameInstance : public
  net::INetworkController` - compose a `NetworkController` it holds, do not inherit the interface.
- **Its script-facing FACE is the accessor + per-tier coordinator handles, not the whole instance.**
  Scripts get the accessor (below) plus a coordinator handle for their tier; they never get the raw
  god object as one undifferentiated blob.

### 2. The accessor = RuntimeContext (our `environment`)

`RuntimeContext` is our `IEnvironment`: the subsystem registry, scene-agnostic. It is what "reach
anything" resolves against. It must stay scene-agnostic - it knows nothing about scenes or scene
loading.

### 3. Retire facades via `.of`, two axes

`.of` is NOT a facade - it is reflection reuse (typed, safe, one registrar line per TYPE not per
method) and it is the pattern to WIDEN. Two clean axes, the return type tells you the scope:

- `RenderSubsystem.of(context)` / `AudioSubsystem.of(context)` / `InputSubsystem.of(context)` -
  subsystem <-> context (one subsystem per context; ALL subsystems are context-scoped).
- `PhysicsSceneSystem.of(scene)` / `NavSceneSystem.of(scene)` - scene system <-> scene (one per
  scene, owned by its subsystem). A subsystem reached via `.of(context)` can also vend its per-scene
  system when a script holds only the context.

Curation moves ONTO the reflected type: a subsystem exposes a deliberate subset via `REFLECT_MEMBERS`
(the safelist lives on the real type, methods point at the real impl - Traktor's `addMethod(&Real::method)`
model), instead of a parallel facade class. The `run`/`ui`/physics/audio/... static facades collapse
into `X.of(context)` / `X.of(scene)`.

- Leaning: curate a subset on the real subsystem type by default; add a small reflected "script view"
  only where the real API is too low-level/sharp to expose.

### 4. Value components stay value; "extend in script" = behavior + injection, not native subclassing

We do NOT ref-type components and do NOT fuse script identity into them (value pools move addresses;
raw-pointer identity would dangle - the versioned-handle rule). Traktor only makes ONE component
(ScriptComponent) subclassable anyway; other components are reached, not extended. Our equivalent:

- The behavior tier IS the "script-extends-entity" tier. We already hold behaviors in a value-pool
  `ScriptComponent` and instantiate them via `CreateInstance` ([ScriptSubsystem.cppm:390,806-882]).
- Give it Traktor-like ergonomics by INJECTING a typed `owner` handle at creation, so authors write
  `RigidBody.of(owner)` / `owner.get<RigidBody>()` - not by native subclassing.
- Uniform across Luau AND AngelScript because the mechanism is convention (lifecycle method names:
  onStart/onUpdate/onFixedUpdate/on<Event>) + injection + reflected `.of`. It must NOT rely on native
  inheritance: Luau (dynamic metatables) and AngelScript (static single-inherit-from-registered-class,
  dispatch caveats) diverge. Inheritance sugar may sit on top per backend but is never load-bearing.

### 5. Game tier: a required base backed by an injected coordinator handle

Scene loading / run control (loadScene, requestExit) live on the GameInstance (or an exposed
SceneManager), NEVER on the scene-agnostic context. The Game script gets these through a required base
(Traktor's `Main extends Stage` shape) whose power comes from an INJECTED coordinator handle, not
native C++ inheritance:

- The engine already does `CreateInstance("Game")`; inject a coordinator handle (or bind a global)
  exposing loadScene/requestExit/etc.
- Leaning: if the "required base" ergonomic (`self:loadScene(...)` feeling inherited) is wanted, use a
  thin script-side base PER BACKEND that forwards to the injected handle - keeps it uniform without
  riding native inheritance.

### 6. Scene tier: injected scene + the same `.of`

The Level script gets the scene injected (we have the bound-Scene facade) plus `.of(context)` for
subsystems and `.of(scene)` for scene systems. No bespoke exposure model - the two-axis `.of` plus a
Scene handle is the whole surface. All three tiers reach the engine through the identical `.of` idiom,
differing only in which handles they are given (game: context + coordinator; scene: context + scene;
behavior: context + scene + owner).

### 7. Messaging: one bus per game instance, no relay, owner-held subscriptions

Collapse to ONE `EventBus` whose lifetime is per-GameInstance, shared by scene systems, behaviors, and
the game tier. Rationale corrected in the exchange:

- A per-instance bus has NO cross-instance re-entrancy problem for PIE (`Array<GameInstance>` = one bus
  each). The earlier "a flat bus can't work" conflated per-process with per-instance; per-instance flat
  is fine and better.
- One shared bus ELIMINATES the forced scene->run relay: a behavior emits `Delivered`, the game
  subscribes directly. That is the "subscribe/emit as you like" we wanted. (An optional Level relay
  remains available purely as an encapsulation/translation boundary, never as a tax.)
- Keep the bus scene-agnostic; make subscriptions OWNER-HELD tokens released on the owner's own
  teardown. The bus never learns about scenes/entities.
- The script subsystem is the owner for script subscriptions and already has the teardown hooks
  (`StopBehavior` / `ReleaseComponentInstances` / scene-system `OnDestroy`); tokens release there. No
  separate broadcast "scene teardown" event is needed for scripts.
- Keep the distinction: broadcast = the instance bus ("anyone interested in X"); directed =
  `entity.send` (to a specific entity).
- One surviving caveat: if multiple scenes are ever active in one instance simultaneously and need
  isolation, a single bus bleeds across them - handle that with a scene tag/topic on the message, NOT
  a second bus. If multi-active-scene is not a supported pattern (or scenes in one instance are meant
  to coordinate), one bus is strictly better and the caveat is moot.

### 8. Networking: move it out of BOTH GameInstance and DefaultApplication

Networking today is special-cased across the instance AND the app (~24 net refs in GameInstance impl,
~28 in DefaultApplication impl); every other subsystem is clean. This is an extraction + relocation
onto existing lanes, not a rewrite. Do NOT resurrect the deleted context-level fixed lane (it was
correctly removed as dead). Concretely:

Off **GameInstance**:
- Extract the live endpoint + `StartServer/Connect/StopNetworking` + `DriveNetwork` + the
  `INetworkController` impl into a dedicated `NetworkController`/endpoint object the instance COMPOSES
  (holds a `Ref<>`, forwards) - it stops BEING the controller (`GameInstance : public
  net::INetworkController` goes away).
- The per-`SetScene`/level-load "set replicated scene" wiring moves with the controller.

Off **DefaultApplication** (the app should carry no net-specific logic):
- The per-fixed-step `DriveNetwork` fan-out ([DefaultApplicationImpl.cpp:455]) stops - replication and
  the transport pump self-tick (below) instead of being hand-pumped by the app.
- `MakeEndpointOnlineHook` / the content-DB net-spawn (prefab) resolver
  ([DefaultApplicationImpl.cpp:644-680]) moves behind an injected net service, not inline in the app.
- `ApplyNetworkStartup` (the role-preset server/connect trigger, [DefaultApplicationImpl.cpp:680-690])
  becomes a call INTO the NetworkController (triggered by script or a role preset), not app-resident
  net logic.
- Net facade registration at startup ([DefaultApplicationImpl.cpp:191-193]) collapses with the rest of
  the facade retirement (section 3): net surface becomes `NetworkController.of(context)` /
  reflected components, not a hand-registered `Net` facade.

Where it goes (the lanes already exist):
- State replication (NetworkedTransform, spawned entities) -> a `NetworkSceneSystem::OnFixedUpdate`
  riding the SAME per-scene fixed lane as `PhysicsSceneSystem` - deterministic, physics-lockstep,
  consistent with every other simulation. The empty `NetworkSubsystem` shell becomes a real
  registrar/observer that owns one such scene system per scene.
- The transport pump (socket recv/send, connection lifecycle) is per-frame, not fixed - a context-level
  subsystem (BeginFrame drain recv, PostUpdate flush send), ticked uniformly rather than hand-pumped.
  Only the TICK becomes a subsystem responsibility; endpoint lifecycle (server/connect) is still
  triggered by script or a role preset via the NetworkController.
- Keep our native replication model (better for us than "script must call Replicator:update()").

## Open decisions (leanings noted; for Fable/user to lock)

1. **Script subscription release:** script-subsystem teardown hooks only, or also a bus-level weak
   token backstop? Leaning: hooks are enough; no bus-level weak refs.
2. **Subsystem curation:** curated `REFLECT_MEMBERS` subset on the real subsystem type, or a dedicated
   "script view" per subsystem? Leaning: curated subset by default; script view only where needed.
3. **Behaviors and the instance bus:** may behaviors subscribe directly (not just emit)? Leaning: yes,
   but only through lifecycle-bound tokens the script subsystem releases.
4. **Multi-active-scene isolation:** is simultaneous active scenes in one instance a supported pattern?
   If yes, define the scene-tag/topic convention; if no, one bus with no tagging.
5. **Game-tier base:** thin script-side base per backend over an injected handle, or a pure injected
   handle (no base type)? Leaning: injected handle is the mechanism; add the base only for ergonomics.
6. **Migration order + scope:** facades -> `.of` is a large sweep. Decide whether to convert
   subsystem-by-subsystem behind both patterns during transition, or cut over per surface.

## What this deliberately is NOT

- Not "become Traktor" - we keep a god object, keep native replication, keep value-typed components.
- Not a call to ref-type components or to rely on native script inheritance.
- Not a networking rewrite - an extraction + relocation onto existing lanes.

## Evaluation trail

- 2026-08-22 (Opus): drafted from the design exchange + Traktor farming. Leanings recorded above;
  open decisions listed for review.
- 2026-08-22 (Fable): REVIEWED. Direction endorsed with corrections + sequencing rules
  below; ready for the user's decision. Verdict per section, strongest first.

  **Endorsed as-is:**
  - §1 (compose, don't inherit INetworkController), §2 (context stays scene-agnostic -
    consistent with the FrameTime layering rules), §4 (value components + injection, never
    load-bearing inheritance - this preserves the versioned-handle and entity.get rules),
    §8's shape (networking extraction onto the existing lanes; the per-scene fixed lane for
    replication beside physics + a per-frame context subsystem for the transport pump is
    exactly right, and the doc correctly refuses to resurrect the deleted context fixed
    lane).
  - §7's core claim is verified-correct: per-instance flat bus has no PIE re-entrancy
    problem, and the scene->run relay is a convention tax (GameInstance.cppm:433 documents
    it as deliberate - it can be deliberately retired).

  **Two factual corrections that change the open decisions:**
  1. **Multi-active-scene IS a supported pattern TODAY** - GameInstance ships additive
     CreateScene(activate=false) + LoadSceneAsync + ActivateLoadedScene. Open decision 4's
     "if it is not supported, the caveat is moot" branch is dead. RULING-QUALITY LEANING:
     one bus, UNTAGGED in v1 anyway - game-semantic events ("Delivered", "OrbCollected")
     WANT cross-scene delivery, which is the common case; add the scene-tag/topic
     convention on the first real collision, not speculatively.
  2. **"One bus per GameInstance" has an unowned-scope gap: instance-less scenes.** The
     scene bus lives ON the Scene (Scene.cppm:497), which is what makes edit-mode Simulate
     work - editor scene pages have NO GameInstance, and physics-contact -> behavior
     eventing runs there today. The spec must name the bus owner for that scope (the
     page's edit context as the pseudo-instance, or the Scene keeps a local bus that IS
     the instance bus when no instance exists). This is the one structural hole in §7.

  **§3 (.of facade retirement) - endorsed as the IDIOM, with a semantics rule that will
  make script views COMMON, not exceptional:** the facades are not only curation - they
  carry semantics the raw subsystem methods do not: the ui surface's mutation-queue
  deferral (dispatch-UAF class - and that facade is DAYS old, built to review findings),
  natural-types marshalling (i32/i64 not f64), the overloaded-name contract flattening,
  and NAME STABILITY (the script surface is product surface - shipping docs + MCP
  script_api + user scripts). So the rule: a member reflected for script on a real
  subsystem type is API-FROZEN (or ScriptName-alias-pinned), and wherever the real method
  cannot preserve the required semantics (deferral, marshalling, safety), a reflected
  script view exists - expect that for ui, physics queries, anything dispatch-sensitive.
  The genuine win is retiring the resolve-service-then-forward BOILERPLATE, not deleting
  curation as a concept. kSubsystemFacadeNameCount survives as the bound-surface guard
  under any mechanism.

  **§5/§6 - endorsed, with one sequencing flag:** the injected-coordinator shape would
  supersede the run facade's MECHANISM, and run/ui are this week's builds that PaperKid P0
  is actively load-bearing on. Do not churn a days-old product surface twice: run/ui
  migrate LAST, in one deliberate cutover with parity tests (the Ui-facade hard-remove
  precedent), after PaperKid has proven the current surface.

  **The six open decisions:** (1) hooks only, no bus-level weak tokens - agreed. (2)
  curated subset by default, but per the §3 rule expect views wherever semantics demand -
  the test is semantic preservation, not API sharpness alone. (3) yes, behaviors subscribe
  via lifecycle-bound tokens the script subsystem releases - agreed. (4) resolved by
  correction 1: supported today; one untagged bus v1. (5) injected handle is the
  mechanism; per-backend sugar base optional and never load-bearing - agreed; it must not
  break the shipped required-Game-class contract. (6) migration order: NETWORKING
  EXTRACTION FIRST (independent of the facade question, cleans the worst wart, ~50 net
  refs leave the app+instance), then the .of infrastructure + ONE pilot subsystem (audio
  is a good pilot: small surface, no dispatch hazards), then the sweep per-surface with
  parity tests, run/ui last. Per-surface cutover, not a long both-patterns transition -
  the SceneLoader/Ui retirements are the model.

  Also for the spec when it graduates: the cook/validation VM registers the same reflected
  surface type-level (two-phase registration covers it - verify with a cook-VM test per
  pilot), and script_api's output is product surface - the MCP/shipping docs update in the
  same commits (the doc-sweep lesson from the Wren retirement).

- 2026-08-22 (user + Fable, follow-up exchange): THREE SPECS come out of this doc, and one
  review correction:
  - **No backward compatibility constraint** (user ruling): we are the only consumer of the
    script surface - the "API-frozen/alias-pinned" half of Fable's §3 rule is DROPPED.
    Clean breaks are fine; internal usage + tests update in the same commit. What SURVIVES
    of the rule is the semantic half only: a script view exists wherever the raw method
    cannot carry the required semantics (deferral, marshalling, safety) - never for
    stability, never as a blocker to a clean break.
  - **Spec 1 - networking extraction**: no open holdovers; spec written
    (Documentation/Archive/networking-extraction-history.md).
  - **Spec 2 - messaging**: user direction - the bus TYPE does not belong in
    foundation.scene; it moves to its own Messaging module (start of the cleanup; does not
    itself resolve the instance-less-scene ownership). Fable's proposed resolution for the
    ownership hole, for the user to confirm before the spec: **the bus is owned by the RUN
    SCOPE and scenes BORROW it** - GameInstance injects its bus into every scene it
    creates/adopts (Scene::SetEventBus, borrowed); the editor scene page (which already
    owns commands/selection/edit-context) is the run scope for edit-mode Simulate and
    injects a page-owned bus; a scene with NO scope falls back to an owned local bus (bare
    unit-test scenes keep working unwired, and a lone scene IS its own scope
    semantically). Emit/subscribe call sites keep using Scene::Events() unchanged - it
    resolves to the injected bus; the scene->run relay dies; entity.send stays directed
    and separate. NOTE (superseded 2026-08-22): the reorg branch is retired; a new foundation
    module just follows the standing folder==target==module convention
    (Process/CONVENTIONS.md).
  - **Spec 3 - script surface (.of)**: the clearer path, pending the curated-edges
    inventory. Fable's first-pass classification of TODAY's facades: DIRECT-REFLECT
    candidates (curated REFLECT_MEMBERS on the real type): audio (bus/music controls),
    input (map/rebind queries), navigation agent ops, render debug toggles. NEEDS-VIEW
    (semantics): ui (mutation-queue deferral - the dispatch-UAF class), anything returning
    engine internals scripts must not hold raw (resource/device handles), physics queries
    IF the per-scene last-hit statefulness is kept (else direct + explicit hit-result
    returns - cleaner under no-compat). INJECTED-HANDLE (not .of at all): the game-tier
    coordinator (loadScene/requestExit - scene loading never on the context), the Scene +
    Entity + owner handles (already exist). The spec's P1 is the audit that finalizes this
    table per facade, with each surface cutting over whole (parity tests), no
    both-patterns transition.

- 2026-08-22 (closure): Spec 3's P0 audit VERDICT accepted by the user - the
  .of(context) facade conversion is NOT built (Subsystem lives outside the reflection
  system; the stubs marshal rather than forward, so they relocate rather than die; the
  .of(scene) axis already shipped). §3 of this doc resolves to: the two existing .of
  axes + the five curated static views ARE the end state. The physics lastHit
  statefulness fix shipped from the audit. Specs 1 (networking) and 2 (messaging) are
  COMPLETE - all three cuts from this doc are now resolved.
