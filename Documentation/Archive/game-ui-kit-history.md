# Game UI kit + UI scripting (`foundation.ui.gamekit`)

> ARCHIVED 2026-11-02, obsolete: its subject was replaced or removed. Non-authoritative: the
> original spec, kept as the record of what was built and why; the code and the tests are the truth.

**Status:** P1 SHIPPED 2026-08-19 (clang + gcc green; all tests pass). `foundation.ui.gamekit`
(UIScreen + ScreenStack) + `engine.ui.script` (reflected view handles + the `ui` screen-tier facade)
built; the old `Ui` facade hard-removed. Scene-tier `scene.ui` DEFERRED to a design call (scene UI rides
`UICanvasComponent` meanwhile - see the DECISION note in section 5). NOTE: the script-facing UI facade is
`ui` (a standalone facade), not `run.ui` - typed finders cannot hang off `run` without an engine.gameinstance
-> engine.ui.script dependency cycle (user decision 2026-08-19). Below is the original P1 plan + Fable's
rulings; the shipped shape follows it except for the `ui`-vs-`run.ui` spelling and the scene.ui deferral.

**P2 progress (2026-08-19; clang + gcc green):** the game-UI WIDGET SET is built + tested on
`foundation.ui.gamekit` (each a small partition sharing `WidgetsImpl.cpp`): `MenuList` (focusable
Button rows + wrap-around Up/Down directional focus via the CORE FocusManager), `Bar` (ProgressBar +
optional smooth drain), `Ticker` (a Label whose integer rolls, exact on landing), `Toast` (COPIED
from the toolkit `ToastHost`/`ToastCard`, CORE-only deps, per the "gamekit uses CORE not toolkit"
rule), and `ButtonPrompt` (`[E] Deliver`, resolving an action's binding via `foundation.input`'s
DescribeBinding - the seven binding-name helpers were relocated out of the editor's InputMapPage into a
new `foundation.input:binding_names` partition so runtime UI can reuse them). `GameUiSandbox` (a
DefaultApplication pushing a UIScreen onto the UISubsystem's screen tier) is the on-screen demo /
validation vehicle. **Transitions (Q2)** already work via `ScreenStack::PlayTransition` (Fade/Slide/
Scale) and are exercised in the sandbox; a latent re-entrancy CRASH they surfaced is fixed - animations
now tick under a new `UIContext::Phase::Animating`, so an animation onComplete's tree mutation (a
transition removing its screen on finish) routes through the mutation queue instead of running inline
and re-entering `AnimationManager::CancelForView` mid-`Update`. **Remaining P2:** transitions Q2 "feel"
tuning (durations / eases / coordinated cross-fade) + per-screen default focus. **Remaining otherwise:**
`scene.ui` (the deferred Fable design call), P3 (below), and ButtonPrompt polish (active-device
switching `[A]`<->`[E]` + pad-button glyphs - both need input-side work). See [[game-ui-kit-track]].

**One-line:** a game-UI convenience library (`foundation.ui.gamekit`, sibling of `foundation.ui.toolkit`
on CORE `foundation.ui`) providing a real **`UIScreen` + `ScreenStack`** primitive, plus a proper
**script surface** (reflected typed View handles + `scene.ui`/`run.ui` roots + `run.pushScreen`/
`popScreen`) - replacing the untested, disliked six-op `Ui` facade.

---

## 1. Why

Games need two things the engine doesn't give scripts today:

1. **Screen management** - menus / pause / game-over / HUD shown and stacked (settings over pause over
   gameplay), with input focus and Back handled, and transitions.
2. **Live control updates** - a script setting the HUD's score label, ticking the timer, toggling a
   "papers: 0" - i.e. reaching INTO an authored view and updating it.

Today scripts only get the `Ui` facade: six id-addressed ops (`pushOverlay(guid)->handle`, `popOverlay`,
`setText`, `setProgress`, `setVisible`, `onClick`) routed through a `"ui.runtime"` service
(`Code/Engine/Engine.UI/UISubsystem.cppm:70-160`). It is asset-driven + id-addressed with the view tree
deliberately kept out of script. The user finds it not-great and it is untested. PaperKid (7 screens +
a HUD) is the first real consumer, but **this is an engine feature**, not PaperKid glue.

Design decisions already locked with the user (2026-08-19):
- **Typed finders**, not a bare `findByName -> View` (option A): `findLabel("score") -> Label` so the
  returned type actually HAS `.text`; identical on AngelScript + Luau (the backend-neutral goal). A bare
  `View` has no `.text`, so `findByName("score").text` does not hold up (compile error in AngelScript).
- **Both `run.ui` (screen tier) and `scene.ui` (scene tier)** are in scope; PaperKid uses both.
- **Three screen input modes:** modal / overlay / opaque.
- **Screens authored as UI DOCUMENT assets** in the editor (dogfoods the pipeline), not code-built.
- **The `Ui` facade is dropped/replaced.**
- Focus/gamepad nav is REUSED, not rebuilt (it already works - see §3).

---

## 2. What already exists (grounding - do NOT rebuild)

- **Two UI tiers in `engine.ui`** (`Code/Engine/Engine.UI/UISubsystem.cppm`): a SCREEN tier (`m_screenRoot`
  + an overlay layer, app-wide, survives scene swaps; `PushScreenOverlay(UIDocument)` / `(RefPtr<View>)`,
  `RemoveScreenOverlay`, `ScreenRoot()`) and a SCENE tier (a per-scene `RootView`, component-driven via
  `UICanvasComponent`; `SceneRoot(scene)`). The C++ push/pop already exists; there is just no SCRIPT path
  to a root.
- **`UIContext` + `GameTheme`** - one context per subsystem, CORE controls only (never toolkit); GameTheme
  is a CORE teal-dark stylesheet (`Code/Foundation/UI/Styling/GameTheme.cppm`), overridable at startup.
- **UI DOCUMENT assets** - `UIDocument`/`UIDocumentSource` in `foundation.ui.resource` (`.sml` markup +
  `.sss` styling), cooked, instantiated at runtime via CORE `MarkupLoader::LoadFromString`. An editor
  `UIDocumentPage` preview seam exists. So a `UIScreen` authored as a document reuses this.
- **CORE focus navigation** - `FocusManager` (`Code/Foundation/UI/Input/FocusManager.cppm`): `FocusNext/
  Prev`, `MoveFocus(FocusDirection{Up,Down,Left,Right})`, per-view explicit overrides, then spatial pick.
  Focus visuals are CORE (`ControlState::Focused` state-recolor, gated to keyboard/gamepad-acquired
  focus). Driven by keyboard (Tab / arrows / Return) in `InputManager::ProcessKeyDown`.
- **GAMEPAD nav already wired - in `engine.ui`, not CORE** (`UISubsystemImpl.cpp:1364-1435`): D-pad or
  left-stick -> `MoveFocus` (hold-repeat), South -> synthesize Return (`OnActivate`), East -> Escape
  (`OnCancel`). So controller-driven menus WORK today; gamekit rides this.
- **View reflection does NOT exist** - CORE UI types have RTTI identity (`RTTI_OBJECT`) but ZERO
  `REFLECT_MEMBERS`/`REFLECT_VALUE`, so nothing is script-bindable. This is the one genuinely new piece.
- **`ScriptRuntimeBinding`** (`Code/Foundation/Script.Facades/ScriptFacades.cppm:77-104`) has hook slots
  (`spawnPrefab`, `dispatchMessage`, `resolveResources`) but NO UI-root slot - the natural home for a
  `Function<Variant(Scene*)>`-shaped `sceneUiRoot`/`screenUiRoot` hook.

---

## 3. Architecture + layering

Three pieces, at three layers:

1. **`foundation.ui.gamekit`** (NEW, on CORE `foundation.ui`, sibling of `foundation.ui.toolkit`): the game
   WIDGETS + the `UIScreen` view type + a `ScreenStack` that operates on any `RootView`. Pure view-layer
   (no scene/render/script deps). Links `Foundation::UI` (+ `Foundation::Shell` ONLY if it reads gamepad
   directly - preferably it stays input-source-agnostic and the host feeds navigation, mirroring
   `foundation.ui.shell`). CMake template = `Code/Foundation/UI.Toolkit/CMakeLists.txt`.
2. **`engine.ui.script`** (NEW, ENGINE-level - see the CORRECTION at the end overriding Fable's Q6): the
   UI SCRIPT surface as an out-of-tree facade module consuming `engine.ui`. It owns the `REFLECT_MEMBERS`
   of the CORE control surface + the typed finders (impl units, GCC hygiene), the `scene.ui`/`run.ui`
   root implementations, and `run.pushScreen`/`popScreen`. It lives at the ENGINE layer because
   `scene.ui`/`run.ui` are TIER roots and only `engine.ui` knows the tiers; reflecting `foundation.ui`
   view types from the engine layer is fine (the module only needs to see the types).
3. **`engine.ui` host wiring**: instantiates the `ScreenStack` on its `ScreenRoot`, keeps the existing
   gamepad pump, and exposes `ScreenRoot`/`SceneRoot` for `engine.ui.script` to back the hooks with.

**Layering tension to resolve (OPEN Q1):** a `ScreenStack` that only attaches/detaches/visibility-toggles
`UIScreen`s on a `RootView` and sets focus is CORE-doable, so it can live in `foundation.ui.gamekit`. But
two of its jobs pull toward `engine.ui`: **modal input-capture** (shielding input below - engine.ui has
`OverlayLayerWantsInput`) and **transitions** if they reuse the property-animation runtime (engine dep).
Recommendation: keep the `ScreenStack` + `UIScreen` in `foundation.ui.gamekit` operating on a `RootView`,
with input-capture expressed as a CORE hit-test/shield flag on `UIScreen` (CORE already shields via
hit-testable children), and transitions done as gamekit BUILT-IN tweens on the screen's transform/opacity
(CORE) in v1 - so gamekit stays CORE-clean; property-animation-driven transitions become a later
engine-level enhancement (Q2).

---

## 4. `foundation.ui.gamekit` - the primitives

### 4.1 `UIScreen`
A `ViewGroup` subclass = one screen/page. Authored as a `UIDocument` (`.sml`), loaded via `MarkupLoader`.
- **Input mode (3):** `Modal` (captures input; dims/blocks below), `Overlay` (pass-through - e.g. the
  HUD), `Opaque` (fully hides below - a full-screen menu).
- **Lifecycle:** `OnEnter` / `OnExit` / `OnShown` / `OnHidden` (native virtuals; also routed to script -
  Q3/§5.3).
- **Declared transition:** an in/out transition descriptor (fade/slide/scale + duration), played by the
  stack. v1 = built-in tween; Q2 = property-animation later.
- **Default focus:** the widget focused on enter (so a pad user lands on something).

### 4.2 `ScreenStack`
Operates on a `RootView` (the screen tier's, wired by `engine.ui`; a scene's for scene-attached screens).
- `push(screen)` / `pop()` / `replace(screen)` / `clear()` / `top()`.
- The TOP screen owns focus + input per its mode; screens below are shown/hidden per the top's mode.
- **Back** = Cancel (East / Escape) pops, unless the top consumes it.
- Focus is saved/restored across push/pop (CORE `FocusManager` already supports save/restore for popups).
- Transitions on push/pop.

### 4.3 Widgets (phased; thin over CORE + its focus nav)
`MenuList` (a CORE list + focus, game-styled), `Bar` (fill + optional smooth drain), `Ticker` (animated
number), `Toast` (transient notice), `ButtonPrompt` (device-aware glyph - "[A] Deliver", wired to the
input device), and settings rows (slider / stepper / toggle / key-rebind, the last tying to the input
rebind system). MenuList/Bar first (PaperKid); the rest later.

---

## 5. The script surface

### 5.1 Reflected typed View handles (the new binding)
`REFLECT_MEMBERS` the CORE control surface (constructor-less bound handles; Objects = OWNED, so no
generation-guard question; ops on a detached view are safe no-ops):
- `View`: `Visibility` (enum, also reflected), `IsEnabled`, `Name`.
- `Label`: `text`. `Button`/`ButtonBase`: `text`. `ProgressBar`: `value`. `TextBox`: `text`.
- `ViewGroup`: `childCount`, `childAt(i) -> View`, `find(name) -> View`, and the TYPED finders:
  `findLabel(name) -> Label`, `findButton -> Button`, `findProgressBar -> ProgressBar`,
  `findTextBox -> TextBox`, `findGroup -> ViewGroup`, `findScreen -> UIScreen`. Each returns null on a
  missing name OR a type mismatch (loud-null, not a silent wrong-type op).
- **No create/destroy/detach from script in v1** (the mutation-queue-crossing rule): scripts read
  structure + set properties; structure stays the authored document.

### 5.2 Roots: `scene.ui` / `run.ui`
- `scene.ui` - a computed property on the `Scene` value facade returning the scene's `RootView` as a bound
  handle (null when headless / no UI).
- `run.ui` - a method on the `run` facade returning the screen-tier `RootView` (per the decided static-
  facade spelling: `run.ui()`).
- **Hook:** `ScriptRuntimeBinding` gains `Function<Variant(scene::Scene*)> sceneUiRoot` and
  `Function<Variant()> screenUiRoot` (Variant/scene terms only - Foundation stays UI-free). `engine.ui`
  installs the impl (returns `SceneRoot`/`ScreenRoot` as a reflected handle). This DELIBERATELY REVERSES
  the old "view tree never reaches script" stance - that reversal is the whole point (Q3).

### 5.3 Screen management from script
- `run.pushScreen(document)` / `run.popScreen()` / `run.replaceScreen(document)` -> the `ScreenStack`.
- A `UIScreen`'s lifecycle + its buttons: buttons fire AUTHORED ACTIONS the Game/Level tier already
  handles (the action runtime - unchanged); the script does not wire callbacks in v1. `OnEnter`/`OnExit`
  optionally surface as script hooks on the Game/Level tier (Q3).

### 5.4 Replacing the `Ui` facade
Every op maps: `pushOverlay` -> `run.pushScreen`; `popOverlay` -> `run.popScreen`; `setText` ->
`findLabel(id).text`; `setProgress` -> `findProgressBar(id).value`; `setVisible` -> `find*(id).Visibility`;
`onClick` -> authored actions (a reflected click hook is deferred). Dependents to migrate: the registrar
(`ScriptSurfaceImpl.cpp:43`, `DefaultApplicationImpl.cpp:174`), host wiring (`DefaultApplication`), and
tests (`Integration.ScriptFacades/UiFacadeScriptTests.cpp`, `Engine.UI.Tests`, `ScriptSceneTests`).
Q4: hard-remove (it is untested + unloved) vs a SceneLoader-style thin alias deleted at the end.

---

## 6. Phasing

- **P1 (core):** `UIScreen` + `ScreenStack` (3 modes) + the reflected typed-View surface + `scene.ui`/
  `run.ui` roots + `run.pushScreen/popScreen` + delete/alias the `Ui` facade and migrate its dependents.
  This runs PaperKid's screens + HUD. Tests: reflected finders both backends; a scene's vs the screen
  root resolve distinctly; a push/pop/replace stack test; the facade-parity migration green.
- **P2 (feel):** transitions (Q2) - MECHANISM DONE (Fade/Slide/Scale via
  `ScreenStack::PlayTransition`; the animation-onComplete re-entrancy crash fixed via
  `Phase::Animating`); `ButtonPrompt` DONE; `MenuList` DONE. REMAINING: transitions Q2 "feel"
  tuning + per-screen default focus.
- **P3 (convenience):** `Toast` + `Ticker` DONE (built early in the P2 batch, alongside `Bar`).
  REMAINING: one-way value bindings + formatters, settings rows, safe-area / anchoring (+ the
  ButtonPrompt polish: active-device switching + pad glyphs).

---

## 7. Open questions (for review)

1. **`ScreenStack` home** - `foundation.ui.gamekit` (RootView-operating, my rec) vs `engine.ui` (where the
   tiers + input-shield live)? Modal input-capture + transitions are the pull.
2. **Transitions** - gamekit built-in tweens (CORE, v1) vs reuse the property-animation runtime (engine
   dep)? Rec: built-in first, property-anim later.
3. **`scene.ui`/`run.ui` via `ScriptRuntimeBinding` hook slots** - confirm the reversal of "view tree out
   of script" (it is intentional; the reflected surface is the point).
4. **`Ui` facade** - hard-remove now (untested/unloved) vs thin-alias-then-delete (safer, more churn)?
5. **`UIScreen` asset** - reuse `UIDocument`/`.sml` as-is (mode/transition come from the stack call or
   markup attributes) vs a distinct `UIScreen` asset type carrying mode + transition + default-focus
   metadata? Rec: `.sml` + a small `<screen mode=.. transition=..>` root element, no new asset type.
6. **Reflected-View home** - `foundation.ui` (an impl unit) vs a thin `foundation.ui.script` lib? Rec: keep
   it out of CORE proper in a small sibling so CORE stays reflection-light and the binding is opt-in.

---

## 8. What this deliberately is NOT (v1 non-goals)

Script-built widgets / runtime UI creation (authored documents are the artifact); data-binding beyond the
one-way convenience in P3; a binary UI tree (`.sml` text stays; binary is a separate UIResource v2);
in-world 3D UI panels beyond what `UIWorldPanelComponent` already does.

---

## FABLE REVIEW (2026-08-19): APPROVED with rulings - build P1 to this

The audit is accurate (spot-checked the load-bearing claims: the two tiers,
the focus/gamepad state, zero REFLECT on CORE views, the binding hook slots).
The shape is right and the layering instincts are mostly correct. Rulings on
the six questions, then findings the spec must absorb.

### The six rulings

1. **ScreenStack home: `foundation.ui.gamekit`, as recommended** - operating
   on a RootView, with modal shielding as a CORE full-surface hit-testable
   backdrop on `UIScreen` (the popup/dialog precedent) and NO shell dep (the
   host feeds navigation; the engine gamepad pump stays where it is). One
   addition that settles the "input-capture pulls it engine-ward" worry: the
   stack reaches everything it needs through the RootView's UIContext,
   including the mutation queue (see finding A). engine.ui's only jobs are
   instantiation-on-ScreenRoot, the hooks, and the run.* registration - as
   drawn.
2. **Transitions: built-in gamekit tweens in v1, capped at the descriptor**
   (fade/slide/scale, duration, easing) so they never grow into a second
   animation system. LAYERING SHARPENING: the property-animation CURVE
   runtime is foundation-tier (foundation.propertyanimation), not engine -
   so gamekit may use its Curve for easing math in v1 with no layering
   violation. What stays "later" is clip-driven/authored transitions; the
   tween DRIVE loop is gamekit's either way.
3. **The reversal is confirmed and the guardrail is the point**: scripts
   READ structure and SET properties; structure stays the authored document
   (no create/destroy/detach in v1). That boundary is what keeps the
   view-tree exposure safe - state it in the spec as a RULE, not a v1
   deferral, and revisit only with a concrete need. The old "actions-only"
   lock is superseded by the user's 2026-08-19 decisions; memory updated.
4. **`Ui` facade: HARD-REMOVE, same commit as the replacement's parity
   tests.** The SceneLoader-style alias earned its keep because SceneLoader
   was tested, documented, and in shipped scripts. The Ui facade is none of
   the three, and every dependent is in-repo (the registrar, DefaultApp
   wiring, the three test files - the spec already lists them). An alias
   would mean maintaining two spellings of a surface nobody liked. The
   listed test files BECOME the parity suite: every old op's replacement is
   exercised in the same commit that deletes the facade.
5. **`.sml` `<screen mode=.. transition=.. defaultFocus=..>` root element,
   no new asset type** - as recommended. One serializer-family rule applies:
   unknown attributes on `<screen>` pass through the cook untouched (the
   settings-unknown-passthrough precedent), so older documents keep cooking
   as the schema grows.
6. **Reflected-View home: the thin sibling lib - and this is not a judgment
   call, it is the standing rule**: subsystem script surfaces go out-of-tree
   in their own module (the facade-pattern rule, the Net precedent). Name it
   `foundation.ui.script`; it hosts the REFLECT bodies (impl units, gcm
   hygiene) + the typed finders + the reflection registration entry point.
   foundation.ui itself gains nothing.

### Findings to absorb into the spec (A-D are build requirements)

- **(A) Dispatch safety - the one hazard the draft does not name.** The
  EXISTING Ui facade's pushOverlay/popOverlay already route through
  `UIContext::MutationQueueRef().QueueAction` (UISubsystemImpl.cpp:141/183)
  because attach/detach during UI event dispatch is the recorded UAF class
  (the ui-mutation-queue rule). The ScreenStack's public mutations (push/
  pop/replace/clear) MUST inherit exactly that: queue the structural work
  through the RootView's UIContext mutation queue. This matters even with
  v1's "scripts run at scene tick" timing, because authored ACTIONS can fire
  from inside click dispatch and land in handlers that call
  run.pushScreen/popScreen. Property SETS (label.text) stay direct - they
  are not structural.
- **(B) Handle ownership, pinned:** reflected View handles marshal in
  Variant's OBJECT mode, which OWNS a RefPtr - so a script holding a handle
  keeps the view ALIVE (detached, invisible, harmless), and "safe no-op on
  detached" holds by construction. Write that into §5.1 as the semantic, and
  add the regression test: pop a screen while a script holds one of its
  labels; setting .text afterward neither crashes nor resurrects anything,
  and the view releases when the script drops the handle.
- **(C) Tripwires, same-commit:** the Ui facade leaving + run.ui/scene.ui
  arriving changes the bound surface - Engine.ScriptSurface's
  kSubsystemFacadeNameCount adjusts in the SAME commit (the tripwire that
  has caught two of these already). The new foundation.ui.script
  registration joins RegisterAllScriptFacades (the composition root), not
  ad-hoc host calls.
- **(D) Typed finder semantics, pinned:** deep search from the receiver,
  first match in tree order, null on missing OR type mismatch (as drafted).
  Also pin that finders and childAt return handles per (B) - never raw
  non-owning views.
- (E) Widget tick (Bar drain / Ticker): P3 widgets need a per-frame update
  source; do not invent a widget-tick seam ad hoc in P1 - it is a P3 design
  point, note it there.
- (F) The spec should name the run-facade spelling dependency explicitly:
  run.ui()/run.pushScreen() ride the P2-2 run facade that just landed -
  gamekit P1 sequences AFTER it (it has: 4491b22e).

Phasing as drafted is approved with P1 amended by (A)-(D). Battery rule as
always; the parity suite + both-backend finder tests are the P1 gate.

### CORRECTION (Fable, 2026-08-19, user catch): the CORE UI animation system exists - USE IT

Both the draft and my review missed `foundation.ui`'s OWN animation layer:
`Code/Foundation/UI/Animation/` - `Animation` + `FloatAnimation`/
`Float2Animation`/`ColorAnimation`, `Storyboard` (composition), `ViewAnimator`
(FadeTo/FadeIn/... factories with `EasingFunction`), and `AnimationManager`
(owned by the UIContext, `ctx.Animations()->Add(...)`, safe against
mid-update adds, target-tracked). Ported from Sedulous.UI; currently has ZERO
consumers outside foundation.ui - which is why neither audit surfaced it, and
which this work now fixes by becoming its first real consumer.

**Ruling 2 is REVISED accordingly:**
- Gamekit transitions are built ON `ViewAnimator` + `AnimationManager` +
  `Storyboard` - there are NO new gamekit tweens, and the
  foundation.propertyanimation-curves-for-easing note is WITHDRAWN
  (`EasingFunction` already exists at the right layer). The screen transition
  descriptor (fade/slide/scale + duration + easing) maps to ViewAnimator
  factories composed in a Storyboard; the ScreenStack adds them to the
  RootView's context AnimationManager. "Property-animation later" still means
  clip-driven authored transitions only.
- Finding (E) is RESOLVED early: `AnimationManager` IS the per-frame update
  source the P3 widgets (Bar drain, Ticker) need - no widget-tick seam to
  design.
- New P1 duty: gamekit's transition tests double as the animation layer's
  first consumer-level coverage (FadeTo/Storyboard driven through a
  UIContext tick), since nothing exercises it end-to-end today.

### CORRECTION (user, 2026-08-19): Q6 layer is `engine.ui.script`, not `foundation.ui.script`

Fable's Q6 rule was right (subsystem script surface = its own out-of-tree module, facade-pattern) but
put it at the wrong LAYER. `foundation.ui` knows View/Label/FocusManager - it has NO concept of the
screen/scene TIERS; only `engine.ui` does (`ScreenRoot`/`SceneRoot`). `scene.ui`/`run.ui` are tier-root
accessors, so the tier-aware half is necessarily engine-level. And the reflected View handles have no
consumer but game UI scripting, which always needs a root to `findLabel` on - so splitting the view
reflection down to foundation buys zero reuse and fragments one surface across two layers. Like
`foundation.net`'s facade is foundation because Net is foundation, the UI script facade is
**`engine.ui.script`** because the UI tiers are engine. Same rule, correct layer.

Net: the whole UI script surface (View reflection + typed finders + `scene.ui`/`run.ui` + `run.pushScreen`)
lives in a new out-of-tree **`engine.ui.script`** consuming `engine.ui`. Every "foundation.ui.script"
mention above (§3 item 2, finding C) reads `engine.ui.script`.

### CORRECTION 2 (user, 2026-08-19): the root binding is an ENGINE per-context service, NOT `ScriptRuntimeBinding`

The draft (§5.2) routed `scene.ui`/`run.ui` through NEW `sceneUiRoot`/`screenUiRoot` slots on the
foundation `ScriptRuntimeBinding`. Wrong precedent. `ScriptRuntimeBinding` is the FOUNDATION behavior
facades' binding (`Entity`/`Scene`/`Time`/`Random`), holding the run time/RNG + hooks for the few engine
capabilities THOSE facades reach (`spawnPrefab`, `dispatchMessage`, `resolveResources`). Every SUBSYSTEM
facade instead installs its OWN per-context service (`context.SetService(key, &binding)`, resolved via
`GetService(key)`): `Ui` = `"ui.runtime"`, `SceneLoader` = `"sceneloader.runtime"`, Audio, Input. That is
the out-of-tree pattern the UI surface follows: **`engine.ui.script` installs its OWN service binding**
(screen-tier root + a scene->root resolver + the `ScreenStack`); `run.ui()`/`run.pushScreen()` resolve
it. Foundation `ScriptRuntimeBinding` is NOT touched - no `sceneUiRoot`/`screenUiRoot` slots.

OPEN (user to pick) - scene UI is INHERENTLY SCENE-SCOPED (a run owns a SceneManager GROUP; multiple
scenes can be live, so a no-arg "current scene" is undefined - the caller must name WHICH scene). Two
scene-scoped shapes:
- **(a) `scene.ui` on the foundation `Scene` handle** - ergonomic (`self.scene.ui.findLabel(...)`), via a
  `sceneUiRoot` `ScriptRuntimeBinding` hook that is the EXACT parallel to `scene.spawn`/`spawnPrefab`
  (already a foundation `Scene`-facade method reaching an engine capability through a hook - this is the
  RIGHT use, distinct from routing the whole binding there). One UI-agnostic slot.
- **(a') `run.sceneUi(scene)`** - an engine method you pass the scene handle to; engine service, foundation
  fully untouched, but clunkier (`run.sceneUi(self.scene)`).
The SCREEN tier (`run.ui()`/`run.pushScreen`) uses the engine service either way. Recommendation: (a) -
scene-scoped, ergonomic, the `scene.spawn` twin. (Earlier "(b) `run.sceneUi()` = current scene" was WRONG
- ambiguous with multiple live scenes.)

### DECISION (user, 2026-08-19): ship `run.ui` now; DEFER the scene.ui script root

P1 builds **only the SCREEN tier script surface** (`run.ui()` / `run.pushScreen` / `popScreen` via the
`engine.ui.script` per-context service) plus `UIScreen` + `ScreenStack` + the reflected typed-View finders.
The **scene.ui script root (a vs a') is DEFERRED** to a design call with Fable on the weekend - the
precedent finding (no installed scene-scoped hook exists outside foundation; the only hook-less precedent
is `ComponentOf`, which works because the scene OWNS its component managers, whereas the scene does NOT own
its UI root - engine.ui does, keyed by scene) is enough to want a live review before committing foundation
surface either way.

**Scene UI in the meantime = `UICanvasComponent`** (`engine.ui`, `UISubsystem.cppm:216`), which already
does the right thing: an authored `UIDocument` on an entity, instantiated into a `root` tree hosted under
the scene root and drawn in the **scene-overlay pass** (`CanvasRenderMode::ScreenOverlay`); spawn/despawn =
open/close; serialized + resource-resolved like any component. This is the clean, overlay-routed path -
scripts do NOT poke the scene UI root, which the user flagged as probably-undesirable anyway. So P1 needs
no scene.ui decision to be useful: HUD/menus that live in a scene ride a `UICanvasComponent`; the run
(screen tier) rides `run.ui`. Reflected script access to a canvas's tree, if wanted, folds into the
weekend scene.ui call.
