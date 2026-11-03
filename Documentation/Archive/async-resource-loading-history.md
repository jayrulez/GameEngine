# Async resource loading (task #123)

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

Size: L (4 phases). Modules: `Code/Draconic/Foundation/Draconic.Resource/`
(core protocol), factories in `*.Resource` libs (Texture, Model, Audio, Fonts,
Scene), `Draconic.Core` JobSystem (existing), consumers (RenderSubsystem et
al.). Web constraint applies throughout.

## Context (verified against the code)

`resource::ResourceManager` (ResourceModule.cppm ~line 199): `Bind(type, id)`
returns a CACHED `RefPtr<ResourceHandle>`; `BuildInto` runs the factory
SYNCHRONOUSLY on the calling (main) thread; factories Bind child resources
mid-build, which records dependency edges via `m_buildStack` and can grow the
handle map. Handles are heap-stable; `Proxy<T>` already tolerates
`Get() == nullptr` (a flushed handle rebuilds in place and proxies recover).
That null-tolerant handle is the natural pending state - no new handle model
is needed.

Today big products (textures, meshes, audio) hitch the main thread on decode
+ upload; on web this is worst (single thread + fetch).

## Design decisions (made; implement as stated)

1. **Two-stage factory protocol.** Extend `IResourceFactory` with an OPTIONAL
   async path (default = current sync behavior so factories migrate one at a
   time):
   - `DecodeStage(payload bytes) -> DecodedProduct` : PURE function of the
     source bytes - no ResourceManager, no GPU, no globals. Runs on a worker.
   - `FinalizeStage(DecodedProduct, ResourceManager&) -> product` : runs on
     the MAIN thread - GPU uploads, child Binds (dependency edges keep
     working because they stay on the build stack thread), registry writes.
   The manager owns IO (content-db product read) before DecodeStage.
2. **Threading seam: JobSystem.** Decode jobs go to the existing Core
   JobSystem (the heavy lane); no dedicated IO thread in v1 (content-db reads
   are memory/pak-backed and cheap relative to decode; revisit if profiling
   disagrees). The editor keeps EditorJobService for its own light-lane work -
   the runtime path must not depend on editor services.
3. **Handle/completion model: pending handle + poll, plus one callback.**
   `BindAsync<T>(id)` returns the same Proxy immediately (Get() null while
   pending). Add `ResourceHandle::State()` (Unloaded/Pending/Ready/Failed)
   and an optional `OnReady` callback on the handle (fired on main during the
   pump). NO futures, NO blocking waits on the main thread except an explicit
   `WaitAll` used by loading screens and tests.
4. **Main-thread pump with a frame budget.** `ResourceManager::Pump(budget)`
   finalizes completed decodes in FIFO order until the time budget (default
   ~2ms, caller-tunable) is spent. DefaultApplication ticks it once per frame
   (before subsystem update so this-frame spawns see ready resources).
5. **GPU upload staging.** FinalizeStage does uploads through the existing
   model-A GPU factory path on the main thread (uploads already go through
   the render queue there). Large textures MAY split into per-mip finalize
   steps under the budget in a later phase - not v1.
6. **Web: same API, cooperative decode.** No pthreads build in v1 (decided:
   SharedArrayBuffer hosting constraints). On web the JobSystem is inline
   (0 workers), so DecodeStage work runs inside Pump's budget too - decode
   must be CHUNKABLE for the worst offenders (texture rows / model buffers)
   OR accepted as an over-budget single slice (v1: accept, measure, chase the
   real offenders). Fetch-based IO on web is already async upstream of this.
7. **Cache/dedup.** Concurrent BindAsync of the same id share the ONE cached
   handle (existing map guarantees it); a sync Bind of a PENDING id upgrades
   to blocking-complete (finish decode inline, finalize, return ready) so
   existing sync callers stay correct during migration.
8. **Dependencies.** Child Binds inside FinalizeStage may themselves be async
   in a later phase; v1: child Binds stay SYNC inside finalize (correctness
   first). Scene/prefab loads therefore become "async at the top, sync
   below" - already the big win (the scene file itself decodes off-thread).

## Phases

- P1: protocol + manager machinery (BindAsync, states, Pump, WaitAll, sync
  upgrade), JobSystem wiring, NO factory migrated yet. Tests: fake factory
  with a slow DecodeStage - pending state, FIFO finalize, budget respected,
  dedup, sync-upgrade race (bind sync while pending), failure path.
- P2: Texture + Audio factories migrated (biggest wins, simplest decode
  purity). Tests per factory: async == sync product equivalence (byte/desc
  compare), plus the existing factory tests run through BindAsync.
- P3: Model/Geometry + Fonts; scene top-level async load; a loading-screen
  helper (progress = pending/total counts on the manager).
- P4: web pass - measure Pump behavior in WebScene (build stamp to verify),
  chunk the worst decode if a hitch > ~30ms shows in practice.

## Rules that bite here

- Threading: decode stage must not touch DefaultAllocator-unsafe or
  thread-affine state; assert main-thread in FinalizeStage (Core has thread-id
  helpers).
- Handle map mutation stays main-thread ONLY (the documented rehash-danger
  comment in Bind is load-bearing).
- No editor types in the runtime path (EditorJobService stays out).
- ASAN + TSAN builds (`-ASAN`/`-TSAN` suffixed Bin dirs exist) on P1/P2
  tests before landing each phase.

## Acceptance

- P2 on desktop: a scene with N large textures loads without a main-thread
  hitch > budget (measure with the profiler P-key dump; before/after in the
  PR). All existing resource/factory tests green both compilers; TSAN clean.
- User-visible check: editor + WebScene still load everything correctly
  (visual); no regression in sync callers.

---

## Implementation status + decisions (Opus build phase, 2026-08-02)

Everything below is the implementing agent's log for review; the spec above is
unchanged. Commits are on master after baseline `43beeaed` (see specs/HANDOFF.md).

### Landed + pushed
- **P1** `3a772a49` (core) + `d424768f` (wiring). BindAsync/Pump/WaitAll/State/
  OnReady, two-stage protocol, sync-upgrade, dtor drain. 9 tests, clang+gcc+
  ASAN+TSAN green.
- **P2** `a33087f7` Texture, `05454c22` Audio.
- **P3a** `24e8c7b3` Mesh (Static+Skinned), `f792b3a5` Fonts.
- Each migrated factory ships an async==sync equivalence test AND a 10-12-way
  concurrent-decode test; all pass under TSAN (the empirical proof of the
  decode-on-a-worker thread-safety argument, per factory).

### Decisions made during the build
1. **`ReapPending` frees through `JobSystem::Wait`, not a `counter.Value()==0`
   check.** TSAN caught a UAF: the JobSystem sets the Counter's count to 0
   INSIDE its Counter lock (RunJob), so a 0 is observable while the signaling
   worker is still touching the Counter. `Wait` has the documented lifetime
   fence (acquires the Counter lock after count hits 0); for a finalized record
   it is effectively non-blocking. This is load-bearing - do not "optimize" it
   back to a value check.
2. **DefaultApplication pumps only the manager it OWNS (`m_ownedResources`),
   not `Resources()`.** When embedded in the editor it BORROWS the editor's
   manager and the editor pumps that manager itself ([:770](ApplicationImpl));
   using ownership avoids double-pumping the shared manager per frame. (Found
   via a reviewer question - the editor drives the embedded app's OnUpdate.)
3. **Decode-on-a-worker is safe - verified, not assumed.** NativeFileSystem and
   the pak both open INDEPENDENT per-call streams (no shared handle/seek);
   `TypeRegistry::FindByName` + `SerializableRegistry::Create` are `const`
   read-only over maps populated at startup (no registration during load). Each
   migrated factory force-inits its product/intermediate `StaticType()` on main
   (or relies on `RegisterX`/`AddFactory` already doing so).
4. **Pure-CPU products use an identity `FinalizeStage`.** Audio/Mesh/Fonts
   products hold no GPU objects (the renderer uploads their CPU images/buffers
   separately), so the whole build runs on the worker and finalize just returns
   the decoded object. Only Texture does real main-thread GPU work in finalize.
5. **Model composite (P3) SKIPPED - deliberately, low value in v1.** Traced:
   NOTHING binds `ModelResource` at runtime. Models reach a scene via the editor
   ModelPrefab path, which reads the MANIFEST and spawns entities whose
   components bind the individual mesh/texture/material resources DIRECTLY -
   those leaf factories are already async. The composite's own decode is a light
   manifest (Guid lists + node tree) and its children resolve via SYNC child
   Binds (spec decision 8), so migrating it moves a tiny read off-thread while
   the real cost stays sync, on a path nothing hits at load time. Revisit when
   child Binds become async.

6. **Scene async landed as "async BELOW", not "async at the top" (`2bbea702`).**
   Rationale: the spec's "async at the top" assumes a scene is a composite
   RESOURCE (Bind<Scene> via a factory) whose DecodeStage parses the scene file
   off-thread. But scenes are NOT loaded via the manager - they load via
   `LoadScene(instance, scene)` (direct deserialize) + `ResolveSceneResources`.
   And for a scene the heavy cost is the RESOURCES it references (meshes/textures),
   not the light entity/component structure. So I made the RESOURCE binds async:
   `ResourceManager::SetAsyncBinds` + `AsyncBindScope` (RAII) flips a mode that
   `Ref<T>::Bind` routes on, so `ResolveSceneResources` (unchanged) binds the
   whole scene's resource set via BindAsync; `AsyncLoadBatch` drives a loading
   screen (Progress/Step/WaitComplete). Scene FILE decode staying on the main
   thread is deliberate - it is lighter than its resources and Scene
   deserialization (component managers, the scene-composition install/observer path -
   ISceneAware in the original text, replaced 2026-08-19) is not worker-safe without
   more work. Making the scene itself an async composite resource is the deferred
   "async at the top" variant if a profile ever shows the structure parse hurts.
7. **Consumer wiring is opt-in / left to integration.** The capability +
   AsyncLoadBatch are landed + tested, but no default scene-load path was flipped
   to async - that changes runtime behavior (resources pop in over frames) and
   wants a game-UI loading screen, so a specific consumer (GameInstance
   level-load) should opt in deliberately. Default scene load is unchanged.

### Status 2026-09-23: the settle-reload stall (the Bistro prefab open)
A settling child reloads its dependents through the recorded edge - and that reload ran in
the caller's mode. In the editor (sync binds outside the page's `AsyncBindScope`) a
material's reload sync-bound its other pending textures; `CompletePending` then waited out
the decode and ran an unbounded `Pump`, finalizing every decoded entry - nested through the
reloads those finalizes triggered - for one 8 s stall on the UI thread. Fixed twice over:
the dependents' reload runs with async binds on (pending slots stay skipped; each settle
reloads again), and `CompletePending` finalizes only its own id (the rest keep their FIFO
turn). `Pump` also reports each burst once when the pending set drains (`async loads
settled: N in flight at the peak, M ms`, peak sampled at bind time) and any single finalize
past 50 ms by type and id (Debug: a shader compile or pipeline creation is expected there).
Resource.Tests cover the reload mode, the single-id upgrade and both log lines.

### Remaining
- Wire a real consumer (GameInstance async level-load + a game-UI loading screen)
  when desired - the mechanism is ready (`AsyncBindScope` + `AsyncLoadBatch`).
- Model composite (skipped, decision 5) - revisit only when child Binds go async.
- **P4 web pass**: not started (measure Pump in WebScene; chunk worst decode only
  if a real >~30ms hitch shows).

---

## Scene / level load: findings + async wiring options (FOR FABLE REVIEW)

Investigated on the user's questions before wiring anything. Nothing changed yet;
this is the decision writeup so review can pick the approach.

### How the game loads a scene at runtime (verified)
There is NO separate "level load" primitive and NO runtime `LoadScene` caller
outside this pattern. The canonical boot sequence is
`PlayerApplication::OnStartup` (Draconic.Engine.Player/PlayerApplication.h ~159)
and the play-in-editor `GamePageImpl` mirrors it exactly:
1. resolve the scene INSTANCE: `--scene` override -> `Settings().defaultSceneId`
   (the AUTHORITATIVE startup-scene Guid, set in the editor's Project Settings /
   SettingsDialog) -> `defaultScene` path fallback;
2. `Instance().CreateScene(name)` on the GameInstance's SceneManager;
3. `scene::LoadScene(instance, scene)` - deserialize entities/transforms/components
   from the instance's "scene" data stream (SerializeScene read);
4. `scene::ResolveSceneResources(scene, Resources())` - the SYNC §8 pass that binds
   every component's `resource::Ref<T>` (meshes/textures/materials/...);
5. `ResolveScenePrefabs(...)` + a second `ResolveSceneResources` for nested prefab
   instances;
6. `EnsureCamera(); scene.Start(); scene.SetSimulationEnabled(true)`.
Runtime scene CONTENT also grows via `SpawnPrefab` - both `Scene.spawn(prefab,...)`
(script, the ONLY scripting door to resource loading) and the net-replicated spawn,
each followed by a sync `ResolveSceneResources` (DefaultApplicationImpl ~224 / ~521).

### How rendering handles multiple scenes (verified)
The app renders **every ACTIVE scene**, not one: `DefaultApplicationImpl` ~478
loops `gi.Scenes().ActiveScenes()` and calls `RenderScene` per scene (layered;
RenderSubsystem is "RenderScene per active scene"). `SceneManager` holds
`m_scenes` (owned) + `m_active` (rendered list) + `m_current` (the `Scene.spawn`
target). So "load another scene" today = `CreateScene` + `LoadScene` +
`ResolveSceneResources` for the new id, which ADDS it to the active/rendered set
(additive/overlay). To SWITCH you also `DestroyScene(old)`. There is no
high-level runtime `ChangeScene(guid)` / additive-load API yet - a caller does the
four steps by hand.

### Where async slots in
The single insertion point is step 4 (and the prefab step 5, and the spawn
resolves): wrap `ResolveSceneResources` in an `AsyncBindScope` so the whole
scene's resource set decodes on workers, drive `AsyncLoadBatch` as a loading
screen, and only `scene.Start()` / reveal once complete. Already-cached resources
resolve instantly (BindAsync dedups); only first-time loads go async. The scene
STRUCTURE parse (LoadScene) stays on the main thread (light; not worker-safe).

### Options (pick one)
1. **New async level-load API (recommended).** Add a runtime helper e.g.
   `LoadSceneAsync(instance, sceneManager, resources) -> AsyncLoadBatch` that does
   CreateScene + LoadScene + `{AsyncBindScope}` ResolveSceneResources, and a
   `Scene.loadLevelAsync(guid)` + progress query script binding. The level appears
   only when the batch completes (WaitComplete / progress bar), so no entity is
   ever observed with null resources - SAFE. Also finally gives scripts a real
   level-load + progress API (today they only have `Scene.spawn`). Per-spawn
   behavior stays unchanged. Adds a small API surface + one game-UI loading screen.
2. **Make `Scene.spawn` (and net-spawn) async by default.** Wrap the two
   DefaultApplicationImpl spawn resolves in `AsyncBindScope`. Smallest change,
   directly script-reachable, kills spawn hitches on heavy prefabs. RISK: an
   entity is returned with null resource proxies on the spawn frame; rendering
   pop-in is harmless, but physics/collision or gameplay that reads a resource the
   same frame it spawns (spawn-then-measure-bounds, a collider whose cooked shape
   is not ready) would see null. Engine-wide behavior change; cannot be validated
   headless.
3. **Opt-in async spawn.** Keep `Scene.spawn` sync; add a separate
   `Scene.spawnAsync` (+ C++ path) a caller opts into where pop-in is acceptable.
   Zero risk to current behavior; more API surface; does not by itself give a
   level-load-with-loading-screen (that still wants option 1's batch).

Recommendation: **option 1** - it is the only one that both delivers the
loading-screen level-load AND makes async loading usable from scripting, with no
null-on-spawn-frame risk. Option 2's engine-wide spawn-semantics change is the
one I would not make without the user's explicit call (hence this writeup).

---

## Scene-load OWNERSHIP + loading-screen UI: user's open questions (FOR FABLE)

The user raised these while reviewing the options above; they likely shape the
scripting track too, so they want Fable's read before we build. Verified facts are
inline so the questions are grounded; the questions themselves are the USER's.

**Verified context**
- GameInstance already OWNS scenes (`CreateScene`/`DestroyScene`/`Scenes()`/
  `SetScene`, GameInstance.cppm ~47-116) and its `CreateScene` is the proper entry
  ("so the re-bind [of behaviors to the run host] happens"). But the LOAD
  ORCHESTRATION (resolve defaultSceneId -> CreateScene -> LoadScene ->
  ResolveSceneResources -> ResolveScenePrefabs -> Start) lives in
  `PlayerApplication::OnStartup` AND `GamePageImpl` - duplicated, in the
  APPLICATION layer, not the instance.
- There ARE two overlay/UI tiers (Render.Api/RenderApi.cppm): `ISceneOverlay`
  (~135) = SCENE tier, composited per-scene ("the canvas"); `IScreenOverlay`
  (~145) = SCREEN / window-space tier, drawn above all scenes (examples: profiler
  HUDs, editor overlays), via `IScreenRenderer`.

**Q1 - Should scene-load orchestration move from DefaultApplication to
GameInstance?** The user's intuition: it feels wrong that DefaultApplication loads
the scene when GameInstance is the user-controllable unit. Proposal to weigh:
a `GameInstance::LoadScene(instance/guid, resources)` (+ `LoadSceneAsync ->
AsyncLoadBatch`) that owns the CreateScene+LoadScene+Resolve+Prefabs+Start
sequence, so PlayerApplication and GamePageImpl both call it (de-dupes the two
copies) and scripts get one obvious entry. Does the run-host / SceneAware wiring
make this clean, or is there a reason the app currently owns it?

**Q2 - Scripting reach.** The user wants scripting "very usable"; today scripts
have only `Scene.spawn`. If scene load lives on GameInstance (Q1), expose it to
script: `Game.loadScene(guid)` / `loadSceneAsync(guid)` + a progress query. This
is the scripting-track overlap - decide the API shape here so the two tracks agree.

**Q3 - Two loading-screen contexts?** The user sees potentially two:
(a) APPLICATION-startup loading (DefaultApplication loading engine/default content
BEFORE any GameInstance exists), and (b) GAME-INSTANCE level-load (a running game
kicks off scene loading and updates its own progress bar). Are both real? If so,
who owns each loading screen, and does (b) reuse `AsyncLoadBatch` while (a) needs
its own pre-instance batch? Or is (a) unnecessary (engine/default content is
tiny / already sync at boot)?

**Q4 - Which UI tier for the loading screen?** Given the two tiers above, a loading
screen wants the SCREEN tier (`IScreenOverlay` / `IScreenRenderer`) - it must be
visible in window space while NO scene is active/loaded yet, so a scene-tier
(`ISceneOverlay`, canvas) overlay would not work for the initial load. Confirm:
(i) loading UI = screen tier; (ii) a game-instance-owned loading screen registers
as an `IScreenOverlay` (who owns/registers it - the GameInstance, the game-UI
subsystem, or the app?); (iii) does the game UI subsystem already have a
screen-tier root a game script could drive, or is that new surface?

Implementation note: the async-load MECHANISM (AsyncBindScope + AsyncLoadBatch)
is done + tested and is agnostic to all of the above - it slots under whichever
ownership + tier Fable picks. These questions are about WHERE the load lives and
WHICH UI tier shows progress, not about the async plumbing.

---

## Fable review + decisions (2026-08-02)

Reviewed the writeup above; verified the load-bearing claims in the code
(PlayerApplication.h:159-186 vs GamePageImpl.cpp:274-311 duplication is real
and identical in sequence; SceneManager::CreateScene AUTO-ACTIVATES the scene
- SceneManager.cppm ~110 pushes to m_active immediately; GameInstance already
owns Scenes()/CreateScene-with-rebind/SetScene-keeps-replication/StartScript/
per-instance ActionRuntime; the Scene.spawn facade pattern is a binding struct
with host-installed function pointers - ScriptFacades.cppm ~65). Resource +
GameInstance suites green at HEAD on both compilers. The landed P1-P3
decisions (Wait-fence reap, owned-manager pump, identity finalize, model
composite skip, "async below") are all sound - no changes requested.

### Option pick: OPTION 1. Do not do option 2.

Changing `Scene.spawn` semantics engine-wide (option 2) is exactly the class
of silent behavior change we do not make: a cooked collider or spawn-frame
bounds read seeing a null product is a gameplay correctness bug that no
headless test will catch. Option 3 is deferred surface - add `spawnAsync`
only when a real game asks for it.

### Q1: YES - move orchestration onto GameInstance. This is the refactor.

The sequence operates ONLY on instance-owned state, and GameInstance's own
comments already anticipate it (`SetScene` "keep replication on the current
scene across level loads"). The app layer keeps POLICY, the instance owns
MECHANISM:

- App resolves WHAT to load (--scene override -> Settings().defaultSceneId ->
  path fallback) and hands the resolved content instance to the API.
- `GameInstance::LoadScene(instance, resources)` (sync) and
  `GameInstance::LoadSceneAsync(instance, resources) -> SceneLoadHandle` own
  CreateScene + LoadScene + Resolve + Prefabs + second Resolve + EnsureCamera
  + Start + SetSimulationEnabled. PlayerApplication and GamePageImpl each
  become one call - the de-dup is the proof the seam is right.
- GameInstance must NOT depend on UI or rendering (SetHeadless servers load
  scenes too). Progress is data on the handle; UI reads it from outside.

**Activation gate (the correctness detail options 1's safety rests on):**
CreateScene auto-activates, so a naive async flow would render/tick a
half-resolved scene. The async path must create the scene INACTIVE and only
activate + Start + enable simulation when the batch completes. Add
`SceneManager` de/activation API (the removal logic already exists inside
DestroyScene - factor it) or a `CreateScene(name, activate)` overload.
Specify and test the edge cases: LoadSceneAsync while one is already pending
on the same instance (v1: reject/return the existing handle - keep it
simple); DestroyScene / instance teardown with a pending batch (the existing
dtor-drain discipline: complete or cancel the batch BEFORE the scene dies -
never let a finalize touch a destroyed scene).

`SceneLoadHandle`: wraps the AsyncLoadBatch + the pending Scene*; surface =
Progress() (0..1), IsComplete(), Failed(), Scene() (null until complete is
acceptable; document it). This is the object both C++ callers and the script
facade read.

### Q2: scripting - ride the existing facade pattern, on the instance.

This answers the "scripting must be very usable" requirement and the
scripting-track overlap: same recipe as `Scene.spawn` (static facade class +
binding struct + function pointers installed by the host), with the pointers
installed per-INSTANCE so multi-instance stays correct. v1 surface on the
`Game` facade:

    Game.loadSceneAsync(guid) -> ticket (i32)
    Game.loadProgress(ticket) -> f64 (0..1)
    Game.loadComplete(ticket) -> bool
    Game.loadFailed(ticket)   -> bool
    Game.loadScene(guid)      -> bool (sync convenience for tiny scenes)

Both backends have coroutines, so the usable idiom falls out immediately:
`var t = Game.loadSceneAsync(id); while (!Game.loadComplete(t)) yield;`.
Numerics rule applies (ticket is i32 through the method path). A
`Game.switchScene(guid)` convenience (load new -> on complete destroy old +
SetScene) is a natural v2 once v1 is proven; do not build it speculatively.
Tests: the script battery gets a fake-factory-backed load with progress
polling on BOTH backends.

### Q3: build (b) only. (a) is not real today.

Engine/default content at app startup is small and loads sync before the
first frame; there is no UI alive that early to show progress on. And the
player's boot IS a level load - fold it into (b): PlayerApplication calls
LoadSceneAsync for the default scene and shows the loading screen from the
first frames. One mechanism, one owner. If a future game ships a heavy boot
preload, AsyncLoadBatch already works pre-instance (it is manager-level) -
defer until that exists.

### Q4: screen tier, owned by the game/app layer - NOT GameInstance.

(i) Confirmed: loading UI is the SCREEN tier. The surface already exists -
`UISubsystem::PushScreenOverlay(document)` / `ScreenRoot()` (the scene-less
screen tier survives scene swaps and stays topmost - there is a test named
exactly that). No new tier plumbing needed.
(ii) Ownership: the app/game layer pushes and pops it around the handle
(GameInstance stays UI-free per Q1). v1: a small default loading-screen
document in PlayerApplication (progress bar bound to handle.Progress(),
popped on complete); a game script can push its OWN document instead and
poll `Game.loadProgress` - that is the scripting story, no extra engine
surface.
(iii) Modality is a feature here: a loading screen with a hit-testable root
makes the overlay layer modal (OverlayLayerWantsInput), which blocks game
input during the load - make that deliberate in the default document, and
leave a passive (IsHitTestVisible=false) variant possible for background
streaming bars.

### Order of work

1. SceneManager activation gate + tests.
2. GameInstance::LoadScene/LoadSceneAsync + SceneLoadHandle + tests
   (including the pending-load and teardown edge cases); migrate
   PlayerApplication + GamePageImpl onto it (behavioral no-op for the sync
   path - verify with existing suites).
3. Game facade additions + script battery tests (both backends).
4. Player default loading screen (screen overlay) - the piece the user can
   SEE; hand over for visual verify with a deliberately heavy test scene.

Steps 1+2 are pure refactor + new API with sync behavior unchanged - land
them first and let the suites prove the no-op before any async path is wired
into a shipped flow.

---

## Fable: script UI access + boot splash design (user's follow-up, 2026-08-02)

Verified surface first: `UIDocument` is ALREADY a resource (UIDocumentFactory
in UI.Resource - bindable by guid); core draconic.ui HAS a ProgressBar
control; `ScriptSubsystem::ConfigureRunHost` applies facade bindings to the
default host AND every instance host; project settings already carries
`defaultSceneId` (ProjectModule.cppm ~51). Everything below composes existing
pieces.

### 1) Yes: a `Ui` facade - asset-driven, id-addressed

Scripts never see Application/UISubsystem - correct and stays that way. Like
`Scene.spawn`, the `Ui` facade is a static class over a binding struct whose
function pointers the host installs (ConfigureRunHost); the pointers land in
UISubsystem's screen tier + the app's resource manager. v1 surface,
deliberately FLAT (no view-hierarchy objects exposed to script):

    Ui.pushOverlay(documentGuid) -> i32 handle   // resolve cooked UIDocument,
                                                 // instantiate, PushScreenOverlay
    Ui.popOverlay(handle)
    Ui.setText(handle, "id", text)
    Ui.setProgress(handle, "id", f64)            // ProgressBar by id
    Ui.setVisible(handle, "id", bool)
    Ui.onClick(handle, "id", fn)                 // the menu-building primitive

Id-addressing mirrors the C++ pattern (`FindByName<Button>(u8"kiosk-btn")`),
keeps the script surface small, and makes documents the AUTHORED artifact
(editor UIDocumentPage) rather than script-built trees. Rules that apply:
mutations from event handlers go through the UI mutation queue; overlay
modality follows the hit-testable-root rule (a menu = modal, a HUD readout =
IsHitTestVisible false); facade numerics (handle i32, progress f64). Richer
object-style binding can ride the reflection track later - do not build it
into v1. With `Ui` + `Game`, a script-driven loading screen is complete:

    var screen = Ui.pushOverlay(LOADING_DOC)
    var t = Game.loadSceneAsync(LEVEL)
    while (!Game.loadComplete(t)) {
      Ui.setProgress(screen, "progress", Game.loadProgress(t))
      yield
    }
    Ui.popOverlay(screen)

### 2) Boot splash: yes, project-settings-specified, and it IS a UIDocument

Project settings gains `loadingDocumentId` (Guid, beside defaultSceneId;
SettingsDialog gets the picker). PlayerApplication boot becomes:

1. Push the splash document as a MODAL screen overlay (built-in trivial
   default when the guid is empty: dark fill + centered bar). The document
   itself loads SYNC - it is one small cooked document whose theme/font are
   already resident; that resolves the chicken-and-egg.
2. `GameInstance::LoadSceneAsync(defaultScene)`.
3. Per frame, drive CONVENTIONAL ids in the document from the handle:
   `progress` (ProgressBar) and `status` (Label) - present = driven, absent =
   skipped. That convention is the whole app<->document contract.
4. On complete: pop splash, activate + Start scene, launch the game script.

Customization = full UI-document authoring in the editor (layout, theme,
images, fonts - anything draconic.ui renders); "make it nice" is an
authoring problem, not an engine one, EXCEPT the id convention which must be
documented in the template project's default splash document. The game
script starts AFTER the first scene is ready in v1 (classic flow);
script-first boot (script drives even the initial load with its own UI) is a
one-flag variant to defer until a game wants it.

### Order-of-work impact

The Ui facade slots as step 3.5 in the plan above (after the Game facade,
before/with the player loading screen - the player splash can be pure C++
against UISubsystem first, script parity right after). Tests: facade battery
on both backends with a fake document (push/pop/setters/onClick), plus the
settings round-trip for `loadingDocumentId` (both-registrations rule).

---

## Fable: boot ordering CORRECTED (supersedes "classic flow" above, 2026-08-02)

User's clarification is architecturally right and the code confirms it: the
GameInstance script is the PER-RUN ORCHESTRATOR (launch/update/exit on the
instance's run host, survives scene switches, distinct from scene-bound
component behaviors), and NOTHING requires a scene before it runs -
TickScript already falls back to sceneScale=1 with no scene, and the
facades are null-scene-safe (Scene.spawn no-ops via binding null checks; Ui
screen tier is scene-less by design; Game.loadSceneAsync is pre-scene by
definition). Today's player starting the script AFTER the scene load
(PlayerApplication.h ~287) is an ordering accident, not a dependency.

### The contract (replaces the earlier "script starts after first scene")

1. **Game script launches FIRST** - at instance start, before any scene.
   update(dt) ticks from frame 1, scene or no scene. exit() at instance
   teardown. This is the orchestrator model made real.
2. **`defaultSceneId` presence is the boot switch - no new flag.**
   - SET: after launch(), the APP kicks GameInstance::LoadSceneAsync +
     the settings splash (conventional ids). Convenience path: a simple
     game writes zero load code.
   - EMPTY: the script owns boot entirely - pushes its own UI, loads
     whatever it wants via Game.loadSceneAsync. This IS script-first boot;
     it stops being a deferred variant and becomes a config choice.
3. **Add `Game.sceneReady() -> bool`** (and the app-driven load is
   observable through it): a script on the convenience path that needs
   scene-dependent init waits `while (!Game.sceneReady()) yield` instead of
   assuming a scene in launch(). Migration note: OUR existing demo game
   scripts assume a loaded scene at launch() because of today's ordering -
   sweep and fix them when the flip lands (they are ours, not users').
4. **GamePageImpl (play-in-editor) follows the same order** for parity -
   the editor's play tab must boot exactly like the player.

### Facade audit rider

As part of the facade work: audit every Game/Scene/Net/Input facade entry
for null-scene safety (called from launch() before any scene exists). The
pattern is already there (Scene.spawn's null checks) - make it uniform and
add a script-battery test that calls the surface pre-scene on both backends.

---

## Opus: queued follow-up - AngelScript editor-side property harvest parity (2026-08-02)

Verified during the Game-facade battery work (user asked to confirm, not trust
the comment): AngelScript property *apply* is NOT deferred - the neutral
property-apply path Invokes `<name>=` with one arg, and AngelScriptContext::Invoke
intercepts the trailing `=` and writes the same-named member field directly via
SetMemberField -> WriteTypedAddress (AngelScriptScriptImpl.cpp ~2255/~2290). So an
AS behavior that DECLARES a matching member field receives harvested values at
runtime, same as Wren.

What is genuinely missing is the OTHER direction: editor-side EXTRACTION of an AS
class's exposed properties FROM source (the Wren path harvests these into
ScriptPropertyDesc for the inspector; AS does not yet - which is why the existing
AS behavior tests carry no properties and my Game AS battery builds the Guid inline
via the reflected Guid(u64,u64) factory instead of an asset property).

QUEUED (this track): AngelScript property-harvest parity - extract exposed
properties from AS class source so AS behaviors get inspector-editable properties
(incl. asset/Guid refs) like Wren. Until then, AS behaviors take config via
constructor/inline or code, not authored properties.

---

## Opus: step 3 progress - Game.* facade wired end-to-end (2026-08-02)

Committed (master), each built + tested clang + gcc:

- c3295d4a Script/Facades: `Game.*` facade (loadSceneAsync/loadProgress/
  loadComplete/loadFailed/loadScene/sceneReady) + reflection + script battery on
  BOTH backends (Wren asset-property level id; AngelScript builds the Guid inline
  via the reflected Guid(u64,u64) factory - AS asset-property harvest is a
  separate queued item, see below). Unwired pointers are safe no-ops (loadComplete
  returns true so a poll loop never hangs).
- 7562973a GameInstance: ticket -> SceneLoadHandle registry. TrackScriptLoad ->
  1-based ticket; PumpScriptLoads (driven each frame by the app after the resource
  pump) activates a finished load, SetScene (instance bookkeeping), then runs the
  app render/sim POLICY hook; ScriptLoad{Progress,Complete,Failed}(ticket) answer
  the facade; unknown ticket -> terminal-complete (never hangs). SceneReady() backs
  Game.sceneReady(). Tests: activate+policy+SetScene+SceneReady, idempotent re-pump,
  failed-load terminal, unknown ticket safe.
- b655b676 DefaultApp: InstallInstanceLoadFacade per instance (primary + extras)
  right after ConfigureRunHost, CAPTURING the instance (orchestrator has no scene
  to route by). loadSceneAsync/loadScene resolve the cooked scene from the content
  DB with the sync path's nested-prefab provider; the others forward to the
  registry. PumpScriptLoads in OnUpdate. Policy = protected virtual
  ApplyLoadedSceneActivation (base Start+SetSimulationEnabled; player override adds
  EnsureCamera-on-scene so a script-loaded level renders).

DECOMPOSITION NOTE (deviates from Fable's 3b/3c split): the app's own STARTUP
LoadSceneAsync + the script-launches-FIRST boot reorder are NOT in the above - they
rewrite the player/editor boot path and are best done as one unit with the demo-
script sweep. Folding them into "3c: boot reorder" keeps 3b (now: facade + registry
+ per-host wiring, the reusable machinery) fully landed + tested. Remaining step-3
work: 3c boot reorder (PlayerApplication + GamePageImpl parity + our demo-script
sweep), 3d facade null-scene audit + pre-scene battery.

PRE-EXISTING RED (not mine): game-instance test "a debugger suspension in update"
fails identically with my test file stashed - StartScript returns false in that
case's setup. Flagged to the user; left untouched (debugger track).

---

## Opus: step 3c + 3d complete + a facade-name bug fixed (2026-08-02)

3c BOOT REORDER (commit 4a269537): game script launches BEFORE the scene.
PlayerApplication.OnLaunch - engine-service bindings, then LoadAndStartGameScript(),
then (if a scene resolved) sync load+activate+SetPrimaryScene. Missing default scene
is no longer fatal (script owns boot); explicit --scene that fails stays fatal.
GameEditorPage got the same order for parity (StartGameScriptFromProject before the
scene build; SetScene after; a scene-load failure after launch stops the script). Our
in-repo demo scripts needed no sweep (empty launch()); cleaned cruft from the user's
EditorProject NewBehavior3.as (net demo) at the user's request.

BUG FOUND + FIXED (commit ed577f4b): the 3a load facade named `Game` collides with the
MANDATORY `Game` orchestrator class - a hard AngelScript compile error ("Name conflict.
'Game' is an extended data type") and a Wren import clash. The orchestrator could never
call its own load facade. The 3a battery missed it (called from a behavior, not a Game
class). RENAMED Game -> SceneLoader (user's choice; methods unchanged; binding field
names unchanged so 3b wiring is comment-only). RECOMMEND Fable note this naming rule:
no facade may share the orchestrator class name `Game`.

3d NULL-SCENE AUDIT: all facades already safe (Scene.* guard currentScene, Entity.*
guard Live(), SceneLoader.* pre-scene by design, Net/Input/Log/Time/Random scene-free).
New game-instance pre-scene battery (both backends) drives a real `class Game`
orchestrator calling the surface pre-scene: compiling = the name-clash regression guard,
launch+tick-without-fault = the null-scene guard.

DEBUGGER RED UPDATE: the game-instance "debugger suspension in update" AS case that read
as a pre-existing failure was a TEST-SETUP gap - it never calls RegisterScriptFacade
Reflection and relied on process-global registration; now that the new pre-scene tests
register it earlier, the full suite is 10/10 green. The real fix is to make that test
self-sufficient (register facades in its own setup) - part of the deferred debugger dig.

REMAINING step 3: 3.5 Ui facade + 4 player boot splash (visual verify).

---

## Opus: step 3.5 Ui facade + facade LAYERING fix (user-flagged, 2026-08-02)

Ui facade (pushOverlay/popOverlay/setText/setProgress/setVisible/onClick) built with a
both-backends battery; onClick round-trips a script fn as a native IScriptDelegate.

USER FLAGGED A LAYERING VIOLATION mid-step: SceneLoader + Ui were added to the neutral
Foundation `draconic.script.facades` lib, but they are SUBSYSTEM concerns. Fixed - moved
both out-of-tree (the Net facade pattern), owned by their subsystems:
- Ui -> draconic.engine.ui (commit a519a8d0), service `ui.runtime`.
- SceneLoader -> draconic.engine.gameinstance (commit f0ae1dbd), service
  `sceneloader.runtime`; binding is a stable GameInstance member the app fills, installed
  on the run context in StartScript.
Foundation's facade lib + ScriptRuntimeBinding now hold ONLY the core surface (Entity/Log/
Time/Random/Scene). Batteries relocated to each owner's test project (raw-context, both
backends). Full clang+gcc green.

Wrote docs/design/adding-facades.md (the how-to the user asked for): the two homes, the
out-of-tree recipe, backend exposure, and the gotchas (no `Game`-named facade; guard every
method; i32/f64 numerics; null-scene safety; RefPtr<IScriptDelegate> callbacks + the AS
`double(double)` funcdef limit).

STILL PENDING for the loading screen to actually work on screen: the Ui host wiring (the
app installs a real UiScriptBinding backed by UISubsystem PushScreenOverlay/ScreenRoot +
control-by-id lookup) - today Ui.* is a registered no-op. That + the player boot splash is
step 4 (visual verify).

---

## Opus: step 3.5b Ui host wiring + step 4 boot splash COMPLETE (2026-08-02)

Ui HOST wiring (commit 9f54896e): UiScriptHost (engine.ui) backs the Ui.* facade with the
live UISubsystem screen tier - handle->overlay-view map + id-addressed control ops
(Label/Button text, ProgressBar value, view visibility, ButtonBase click -> IScriptDelegate).
DefaultApplication owns one host (screen tier is app-wide), resolves cooked UIDocuments by
guid from the resource manager, and the context configurator installs the binding on every
run context. Ui.* is now REAL (was a no-op). Headless test drives real controls + fires a
click delegate.

Step 4 boot splash (commit f0730ef1): the player streams the default scene ASYNC behind a
splash overlay. loadingDocumentId project setting (Guid, v8); nil = built-in default
(status Label + progress ProgressBar). OnLaunch pushes the splash + kicks LoadSceneAsync
(after the game script launches); OnUpdate's DriveBoot drives progress each frame + activates
the scene on completion + pops the splash. Full clang+gcc green.

>>> NEEDS USER VISUAL VERIFY: run the player on a project whose default scene has async
assets - splash shows with a filling bar, then the scene appears. <<<

DEFERRED (not blocking): editor SettingsDialog picker for loadingDocumentId (set via the
project file for now); a prettier default splash markup (styling attributes).

Task #123 async-resource-loading + the scripting/level-load/loading-screen story is now
functionally COMPLETE end to end. Remaining are the deferred/queued niceties above +
the AS-harvest / void()-delegate-funcdef / debugger-test-self-sufficiency follow-ups.

---

## Opus: debugger-test "issue" RESOLVED - it was the Game-facade clash all along (2026-08-02)

CORRECTION to the earlier notes calling the game-instance "debugger suspension in update"
AS test a pre-existing failure / test-setup gap. It was NEITHER. Root cause, now proven:

The run host's EnsureContext calls RegisterScriptFacadeReflection + RegisterReflectedTypes
(ScriptSubsystem.cppm ~173), so every game-script context registers all facade types with
the engine. The debugger test's script is `class Game {...}`. While the level-load facade
was named `Game` (steps 3a-3b), that class collided with the registered `Game` facade type
-> AngelScript "Name conflict. 'Game' is an extended data type" -> StartScript returned
false -> the whole test cascaded (14 failed assertions).

Renaming the facade Game -> SceneLoader (commit ed577f4b) removed the clash; `class Game`
compiles again and the suite went 10/10 at that commit. Verified stably green: debugger
test in isolation x3 + full suite, clang + gcc. NO further action on that test - it is a
genuine victim-then-fix of the naming bug, not a flaky/self-sufficiency problem. The
follow-up item is dropped.

---

## Fable REVIEW of the completed track (2026-08-02)

Reviewed 0602b486..e0027232 (13 commits). Suites re-run at HEAD: Scene 246,
Resource 314, GameInstance 130, Engine.UI 284, Script/Wren/AngelScript/Scene
batteries - ALL green, clang + gcc. Conventions clean (no trailers, no
dashes). Verified in code: the activation gate (create-inactive -> gated
ActivateLoadedScene), the boot reorder, the app-policy callback keeping
GameInstance render-unaware, delegate lifetime on BOTH backends (AS delegate
ref-holds its manager + null-guards; Wren has the Detach-on-VM-free protocol;
the IScriptDelegate contract "error if the owning context is gone" is
actually implemented), and the player DriveBoot sequence. The
SceneLoader rename + out-of-tree facade relocation + the debugger-test root
cause were all the right calls. Overall: high-quality track. Findings:

### 1. MUST FIX: Ui.popOverlay from an onClick handler mutates mid-dispatch

`UiScriptHost::PopOverlay` calls `UISubsystem::RemoveScreenOverlay`, which
calls `m_overlayLayer->RemoveView(view)` DIRECTLY - no deferral. A script
popping its own overlay from its onClick handler (the standard "close menu"
button - arguably the facade's primary use case) destroys the view tree
mid-event-dispatch: exactly the mutation-queue rule, and the facade's own
doc comment ("the host defers") promises otherwise. The battery missed it
because its click test does not pop from inside the handler.

Fix shape: remove the m_overlays map entry immediately (the handle dies
synchronously, repeat pops are no-ops), but route the view detach through
`UISubsystem` Context() MutationQueueRef().QueueAction (hold the RefPtr in
the queued action). Give PushOverlay the same discipline for
handler-context calls: instantiate the view + insert the map entry
synchronously (the handle must return), defer only the AddView attach.
Add a battery case that pops (and one that pushes) from INSIDE a click
handler, and fix the facade doc comment if any behavior differs.

### 2. Record as a known limitation: handle progress is manager-global

`SceneLoadHandle::IsComplete/Progress` poll `ResourceManager::PendingCount`
- the WHOLE manager - and m_total snapshots the global count. Two
overlapping loads (or any unrelated BindAsync in flight) conflate: each
handle waits on ALL pending work (the safe direction) and progress
distorts. Fine for the boot + single-level-switch reality; MUST move to
per-batch tracking (AsyncLoadBatch exists and does exactly this) before
overlapping/background loads become real. Put a comment on the handle
saying so; no code change required now.

### 3. Minor: script-load tickets never retire

`m_scriptLoads` grows monotonically (one entry per level switch, linear
scans in the polls). Retire entries once activated/failed and polled, or
sweep on scene destroy. Low priority; bounded by level switches per run.

### 4. Caution (comment, no code): pending-scene destruction

`SceneLoadHandle` holds a raw `Scene*`. Nothing destroys a PENDING inactive
scene today, but if that path ever appears (instance-level scene sweeps,
script-driven cancel), `PumpScriptLoads` would activate a dangling pointer.
Note it on TrackScriptLoad so the future author clears tracked loads when
destroying their scene.

### On the user's Ui-facade concern (agreed, and it is the right v1)

Correct read: Ui.* is a loading-screen/menu primitive, not general UI
scripting. That is the intended v1 - the UI framework is not script-friendly
yet, and growing this facade method-by-method (setColor, setLayout, ...)
would recreate the framework badly, one op at a time. The real path to
script-usable UI is the REFLECTION track (P3 runtime surface): reflect the
View/control property + method surface, and scripts get typed view objects
through the same reflected-type path as everything else. When that lands,
Ui.* stays as the convenience layer (push/pop cooked documents by guid) and
the id-addressed setters become thin sugar. RULE for the interim: the Ui
facade does not grow beyond the current six ops without a design
conversation; new needs go on the reflection track's list.

Verdict: land finding 1, note 2-4 in comments, then #123 closes pending the
user's visual verify of the boot splash.

---

## Opus: actioned Fable's review (2026-08-02)

Finding 1 (MUST FIX) LANDED - commit f8235f01. UiScriptHost push/pop now defer the overlay-
layer tree mutation through UIContext MutationQueueRef().QueueAction: PushOverlay instantiates
the view + registers the handle synchronously (handle returns; setters address the map's view
attached-or-not) and defers the attach; PopOverlay drops the map entry synchronously and
defers the detach, holding the RefPtr in the queued action. New UISubsystem::Instantiate
ScreenOverlay splits instantiate from attach. The C++ boot splash (direct UISubsystem, not
event dispatch) is unchanged. Battery: a close button pops its own overlay from the handler -
handle dies at once, tree stays attached until the drain (no mid-dispatch destroy). UI 292
green clang+gcc.

Findings 2-4 noted in code comments (no behavior change), as directed:
- 2 (handle progress manager-global) -> LIMITATION comment on SceneLoadHandle.
- 3 (m_scriptLoads grows) -> MINOR comment on the member.
- 4 (raw Scene* pending destroy) -> CAUTION comment on TrackScriptLoad.

The Ui facade doc comment ("mutations from an onClick handler follow the mutation-queue rule -
the host defers") is now TRUE, so it stands.

Per Fable's verdict, task #123 CLOSES pending the user's visual verify of the boot splash.

---

## State (appended 2026-08-03; original content above is unchanged)

**CORE SHIPPED (~90%).** P2 (texture, audio), P3 (geometry, fonts, scene resource,
SceneManager activation gate), async orchestration + SceneLoadHandle, boot splash,
and the Fable review findings all landed (a33087f7..9a1199bc). REMAINING: the P4
web-perf pass (not started - measure Pump in WebScene, chunk only on a real hitch)
+ optional extra consumer wiring. The composite-resource model stays deferred.
