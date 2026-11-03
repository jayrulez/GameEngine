# Game-ready scripting 2: the remainder (run tier, scene.ui, UI reflection)

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

**Status:** GREENLIT to build (2026-08-18) - the gating prerequisite for the PaperKid sample game
([[paperkid]]): its scripted Game/run tier + script-driven HUD/screens need exactly this track. Build
order is the Phasing below; **P2-1 + P2-2 (the run bus + run facade) hard-block PaperKid P0**, so they
go first. Was: SPEC, ready to build (2026-08-08), successor to `game-ready-scripting.md` (that track's
two structural goals - reflected engine reach, tier communication - are CLOSED).

**Backend note (2026-08-18):** this track stays BACKEND-NEUTRAL, but its batteries target the SURVIVING
backends - **AngelScript + Luau**. Wren is slated for retirement ([[luau-backend-track]] P7); do NOT add
Wren-only machinery or migrate demos onto Wren here. The first consumer (PaperKid) exercises it in
AngelScript.

This doc collects everything found incomplete at the 2026-08-08 review plus the follow-ons that were
"deferred to a later spec" - this is that spec. All prior rulings apply (re-resolving handles for live
scene state, name-keyed native bus, hook-slot inversion, reserved names `Level`/`Game`/`SceneLoader`/
`run`, the Ui facade does not grow past its six ops).

Verified starting state: NO `run` facade/bus exists in Engine.GameInstance;
NO `sceneUiRoot` slot exists in Script.Facades; `SceneLoader.*` is the
live level-load surface; the scene `EventBus` + bridge + `on<Event>`
harvest are shipped for behaviors and Level.

---

## 1. The `run` root (the missing access-model row)

The spec's access table promised `run` - one per app, app-owned - and none
of it exists. Build order matters: the bus first (native), then the facade
over it.

### 1a. Run-scoped EventBus

- The SAME native `EventBus` class (draconic.scene owns the type; it is
  scene-agnostic - StringHash + Variant + subscriber list), instantiated as
  a member of `GameInstance` (one per run). No new bus code; if the class
  currently lives behind a scene-only seam, hoist the TYPE to a neutral
  partition (it has core-only deps by design) without changing the scene's
  usage.
- Drained on the instance tick (same top-level drain point that ticks the
  game script - after TickScript, same cascade bound N=8 + runaway log +
  subscription-order delivery as the scene bus).
- The **Game script's `on<Event>` inbox harvests against the RUN bus**, not
  a scene bus (the Game tier is scene-agnostic; behaviors/Level keep their
  scene bus). Same derived-from-handlers subscription form; same bridge
  pattern (one native subscriber fanning to `ScriptObject::Invoke`),
  installed per instance.
- **[SUPERSEDED by messaging.md 2026-08-22: scene and run now SHARE one bus per run
  scope, so there is nothing to relay across - a behavior emit reaches the Game tier
  directly. The Level re-emit below remains only as an optional translation boundary.]**
- **NO automatic scene->run relay.** A behavior/Level that wants the run
  tier's attention emits a scene event; the GAME script (which can hear its
  scenes if it chooses - see 1c) decides what becomes a run-level event by
  re-emitting. The orchestrator owns cross-tier policy; the engine wires
  nothing implicitly.

### 1b. The `run` facade (out-of-tree, Engine.GameInstance - the Net pattern)

Reserved name `run` (already on the reserved list). Registered via
`RegisterExtraFacadeName`, bindings installed per run context in
StartScript (exactly like SceneLoader today). v1 surface:

    run.events().emit(name)/.emit(name, payload)  // the run bus (1a) - events() is a METHOD (below)
    run.loadScene(guid) -> bool                   // absorbed from SceneLoader
    run.loadSceneAsync(guid) -> i32 ticket
    run.loadProgress(ticket) -> f64
    run.loadComplete(ticket) -> bool
    run.loadFailed(ticket) -> bool
    run.sceneReady() -> bool
    run.currentScene() -> Scene                   // bound handle (explicit query)

`emit` overload set mirrors the scene facade exactly (none/f64/String/bool/
Entity/generic Variant sink).

DECIDED SPELLING (Fable ruling 2026-08-19, run-facade-api-question.md): `run` is a STATIC service, so
its accessors are METHODS with parens - `run.events()`, `run.loadScene(...)`. The parens-less form is a
VALUE-HANDLE nicety (`scene.events` on the handle from `entity.scene`); `run.events()` returns the
`RunEvents` value handle whose own surface then mirrors `SceneEvents` verbatim. The convention: a value
handle you HOLD gets properties where they read naturally; a static service you CALL gets methods,
always. Not a stopgap - if a later polish pass wants the parens-less form, the sanctioned mechanism is a
static-computed-property feature and `events()` stays as a compatible spelling.

### 1c. SceneLoader absorption (the ruled fold-in: no parallel level-load APIs)

- `run.*` load methods route through the SAME GameInstance bindings
  SceneLoader uses today - one implementation, two names for one release.
- `SceneLoader` immediately becomes a documented thin alias; every in-repo
  script/sample/test migrates to `run.*` in the same commit; the alias is
  DELETED in this track's final phase (it is our API, nothing external
  holds it). Remove `SceneLoader` from the reserved list only when deleted;
  `run` stays reserved forever.

### 1d. Tests

- Native run-bus battery (zero script): publish/subscribe/cascade/order on
  a GameInstance; TWO instances have independent buses (the multi-instance
  correctness test).
- Game-script inbox both backends: C++ publishes on the run bus ->
  `onServerShutdown(payload)` fires on the Game class; a Game handler's
  re-emit of a scene event reaches a Level (the explicit relay pattern,
  proven in a test).
- run.* load battery both backends = the existing SceneLoader battery
  retargeted (and kept green through the alias until deletion).
- Pre-scene safety: every run.* call is legal in `launch()` before any
  scene exists (the null-scene battery pattern).

### 1e. Acceptance

A game script can boot with `defaultSceneId` EMPTY and orchestrate its whole
lifecycle through `run.*` (load with progress via `Ui`, hear run events,
query the current scene) - demonstrated by migrating the player-boot demo
script; user-verified on desktop.

---

## 2. `scene.ui` wiring (A5, per the OQ-5 hook-slot ruling)

Small and fully designed - implement as ruled:

- `ScriptRuntimeBinding` gains ONE slot:
  `Function<Variant(scene::Scene*)> sceneUiRoot` (Variant/scene terms only -
  Foundation gains NO ui dependency).
- The bound `Scene` facade's `ui` property calls it; empty slot or
  null-returning slot = null to script (headless runs and no-UI tests are
  correct by default; every facade guard rule applies).
- `draconic.engine.ui` installs the implementation (it owns UiScriptHost +
  the UISubsystem scene-root map): returns the scene's `RootView` as a
  constructor-less bound handle (unit-1 machinery).
- ACCEPTANCE IS HONEST (the ruled constraint): until item 3 lands, holding
  the root is the whole surface - the test proves `scene.ui` resolves to
  the right root per scene (two scenes, two roots) and that property access
  on it no-ops cleanly. No demo may imply per-control editing before item 3.

---

## 3. Per-control UI scripting: reflect the draconic.ui View surface

The named follow-on, now specced. This is Track A applied to `draconic.ui`:
scripts get typed view objects; `label.text = "3 / 6"` replaces the Ui
facade's id-addressed setters for anything already holding a view.

### Design

- Reflect the CORE control surface (not the toolkit) in an implementation
  unit (`UiReflectionImpl.cpp`, gcc hygiene): `View` (Visibility, IsEnabled,
  Name, hit-test flag), `Label` (text), `Button`/`ButtonBase` (text),
  `ProgressBar` (Value), `TextBox`-equivalents (text), `ViewGroup`
  (childCount, childAt(i), findByName(name) -> View). Reflect the
  `Visibility` enum. Methods take natural types (facade-numerics rule);
  every returned view is a constructor-less bound handle.
- **Views are Objects (RefPtr) - handles are OWNED, not borrows**, so no
  generation-guard question arises; but a handle can outlive its VIEW's
  attachment (popped overlay, despawned canvas): view ops on a detached
  view must remain SAFE no-ops where they cannot apply (they mutate an
  orphan tree, which renders nowhere - acceptable; document it).
- **The mutation-queue rule crosses the boundary**: any reflected method
  that DESTROYS or detaches views (`removeChild`, etc.) is NOT reflected in
  v1. Scripts mutate properties and read structure; structural mutation
  stays with the authored document + the Ui facade's deferred push/pop.
  Revisit only with a queued-action design.
- **No view CREATION from script in v1** - documents stay the authored
  artifact (the principle behind the six-op facade rule). `findByName` +
  property writes cover the HUD/menu cases.
- Emission: the components ride the blessed "additional emission roots" API
  on both backends' collectors (built for exactly this).
- The Ui facade stays six ops (standing rule) - it remains the
  document-push layer; the reflected surface is what you do with a view you
  hold (from `scene.ui`, `Ui.pushOverlay`'s... note pushOverlay returns an
  i32 handle, NOT a view - ADD `Ui.overlayRoot(handle) -> View` as the ONE
  sanctioned bridge from facade handles to the reflected surface; this does
  not count against the six ops, it is the seam between the layers).

### Tests

Both backends: read/write every reflected property on a real tree; findByName
descent; two scenes' roots via scene.ui are distinct; a detached view
no-ops safely; the Level HUD example from doc 1 rewritten to
`_hud.findByName("score").text = ...` as the living proof (and the Ui-facade
sugar path still green).

### Acceptance

The Roll Call Level drives its HUD through `scene.ui` + reflected views
with zero C++ per-control code; user visual verify on desktop.

---

## 4. `run.ui` - screen-tier root for scripts (thin, after items 2+3)

Same hook-slot pattern, run/app-scoped: a `screenUiRoot` slot returning the
UISubsystem screen-tier `RootView` as a bound handle, installed by
draconic.engine.ui, surfaced as `run.ui`. Gated ON item 3 (without reflected
views it is as inert as scene.ui's v1) - land it after, with the same
per-scene/per-tier distinctness test. The Ui facade's push/pop remains the
way overlays ENTER the screen tier; `run.ui` is how a script reaches what is
already there (e.g. the boot splash pushed by the player - a Game script can
then own its progress label).

---

## 5. `ScriptName` alias (the P-A1 cosmetic, specced)

`RigidBodyComponent.of(entity)` reads heavy; the spec always wrote
`RigidBody.of(entity)`. Add an optional script-facing alias:

- `TypeBuilder::ScriptName("RigidBody")` stores the alias on TypeInfo (new
  optional `const char* scriptName`, null = use `name`).
- Both emitters bind the CLASS under the alias when present; the native
  type name is unchanged (serialization untouched - this is script-surface
  only).
- The bare-name collision rule extends to aliases: a duplicate across
  (names + aliases) is the same loud startup error.
- Sweep: the `*Component` gameplay types gain aliases dropping the suffix.
  Tests: alias binds on both backends, collision detection fires, native
  FindByName is UNAFFECTED by aliases (wire identity never aliases).

---

## 6. Explicitly NOT in this doc

- **Dynamic bus `subscribe`**: designed to completion in doc 1 section 14
  (entity-owned, auto-unsubscribe); build on first concrete need - not
  before.
- **Character->sensor trigger events**: a PHYSICS feature (doc 1 section 17
  records the gap + candidate fix + the flip-guard test); belongs to a
  physics pass, not scripting.
- **EventSet authored effects**: stays parked (doc 1 section 4).
- **Ray hit-point accessors** (P-A1 small follow-up, needs per-scene state):
  fold into whichever physics pass takes section 17.

## Phasing

- **P2-1**: run bus + Game inbox + tests (1a, 1d-native). DONE (Opus 2026-08-19): P2-1a =
  `scene::EventBus m_runEvents` on GameInstance, drained after TickScript in the app loop, native
  battery (order/cascade/two-instance isolation) - commit 71baa1f1. P2-1b = the Game `on<Event>`
  inbox: the shared bridge (ScriptEventSubscriptions) generalized to bind an `EventBus*`; GameInstance
  harvests the Game class's handlers (threaded through a new `StartScript(...,gameHandlers)` overload;
  editor GamePage passes the cooked ScriptClass's) and dispatches at drain; inbox tests on AngelScript
  + Luau - commit 08816db5. Green clang + gcc. NO scene->run relay (as specced).
- **P2-2**: run facade + SceneLoader alias + in-repo migration (1b, 1c). OPENS with the `ScriptName`
  MECHANISM pulled forward from §5 (Fable ruling 2026-08-19, Q2(i)) so `class Run` binds lowercase as
  `run`: the TypeInfo alias field + `TypeBuilder::ScriptName` + both emitters binding the alias + the
  bare-name collision rule extended to aliases (a FinalizeTypes trap) + tests - the component-alias
  SWEEP stays in P2-5. Guardrails: `run` joins the RESERVED names (a user class named/aliased `run`
  traps at FinalizeTypes); RegisterExtraFacadeName registers the ALIAS (`run`, not `Run`); bump
  kSubsystemFacadeNameCount when `run` joins the bound surface. SceneLoader absorption: extend
  SceneLoaderScriptBinding so ONE service carries load + run-bus, but keep the service key/struct NAME
  as-is this phase (rename to the run-era name in P2-5 when the alias deletes - one rename, not two).
- **P2-3**: scene.ui slot (2).
- **P2-4**: UI View reflection + Ui.overlayRoot + the Roll Call HUD proof (3).
- **P2-5**: run.ui (4) + ScriptName sweep (5) + SceneLoader alias DELETION. SceneLoader deletion DONE
  (Opus 2026-08-19, commit 70d950f8): run.* fully absorbs level-load; struct/service renamed to the run
  era; count 33->32. (run.ui + ScriptName sweep still open.)
- **P2-6 (added 2026-08-19): button click -> script.** §3 deferred click handling to "authored
  actions" that never reached script (buttons only fire C++ OnClick). DONE (Opus 2026-08-19): the
  reflected `Button` handle gains `onClick(handler)` taking a `RefPtr<IScriptDelegate>` - it binds the
  underlying `ButtonBase::OnClick`; the captured RefPtr keeps the script function alive exactly as long
  as the button (released on pop). Safe by construction: `ScreenStack` push/pop already defer through
  the UIContext mutation queue, so a handler that transitions screens is fine. AngelScript wraps a void
  handler in the `Action` funcdef (`button.onClick(Action(fn))`); Luau passes the function directly.
  Tests: UiFacadeScriptTests both backends, both compilers. This is what makes PaperKid's script-driven
  screen flow ([[paperkid]]) actually work.
- Each phase: both compilers, batteries both backends, ASAN on new
  machinery; user visual verify closes P2-4/P2-5.
