# Draconic — Engine Roadmap (ARCHIVED 2026-09-01)

> This checklist is no longer the planning surface. Everything still open moved
> to the weekly docs (Documentation/Plans/week-2026-09-05.md, "Backlog absorbed
> from roadmap.md"); the weeklies are the one place work is planned and
> recorded. Kept for the strategy narrative, the decisions log, and the done
> record as of the final sync below.

**Last synced to reality: 2026-09-01.** Since the previous sync (2026-07-29) the "decided-not-
started" pair both SHIPPED (web platform: WASM player + WebGPU + AS-on-web; shaders: .hlsl
files + bytecode cook + WGSL), and so did: terrain (complete, incl. brushes + top-K splat +
nav-bake feed), navigation (Recast/Detour + editor bake), mesh LOD (complete), export/settings
templates incl. the WEB target, asset thumbnails (13 types), physics CCD/mass + shape queries,
splines (Bezier-native + editor tool + path-follow), i18n foundation (correction 2026-10-17:
not built; only its spec existed), Luau replacing Wren as
the second backend, and the first game (PaperKid, playable in-editor). Weekly plan docs
(week-*.md) are the fine-grained ledger; this doc is the cross-track summary.

**Goal:** general-purpose engine at **Sedulous parity or better** (Sedulous ships game-ready systems
+ a usable editor; much of Draconic was seeded from it, and more will be — notably the UI framework).
**Platforms:** desktop (Win/Linux) now; **web/WASM + Android must stay viable** (Apple deferred — no
Mac hardware yet); console later behind clean abstraction boundaries.

**How to use this doc:** the source of truth for cross-session progress. Tracks below mirror the engine
structure. Check items off as they land; keep the detailed per-system design docs
(`renderer.md`, `particles-authoring.md`, …) as the deep references.

**Status legend:** `[x]` done · `[~]` partial / in progress · `[ ]` not started.

**Definition of done (every track):** an item is only `[x]` when it lands with **adequate tests**
(doctest suites in the owning module's `Tests/` dir, matching the existing coverage bar). No feature is
"done" without tests. GPU/visual work that can't be fully unit-tested still gets whatever is
testable (data/serialization/logic round-trips) plus a sample that exercises it.

---

## Strategy

Rendering was done first on purpose — it's the hardest system to get right, and it had to be. Particles
followed as an extensibility test of the renderer; animation followed because animated meshes needed
it; the runtime/editor **triad seam** was built early because it shapes everything downstream. That
focus is why the engine is deep on tech/content and thin on gameplay runtime — **by design, not by
neglect.**

Near-term direction (agreed): **solidify the infrastructure and start the tooling side before building
out the gameplay subsystems.** Fill holes from **Core** upward. The subsystem architecture (Sedulous-
style, proven) is trusted, so there's no rush to implement audio/physics/AI — the leverage right now is
in Core hardening, the content pipeline, prefabs, portability groundwork, and the editor.

### Recommended near-term sequence
0. ~~**UI framework**~~ ✅ **DONE** — faithful Sedulous.UI port (draconic.ui, proven on-screen) + a
   fresh eepp-derived draconic.gui. This was the prerequisite for the editor's UI.
1. **Editor infrastructure** ← **ACTIVE BIG TRACK** (started 2026-07-10) — the scene/level editor
   *application*, built on draconic.ui. **Substantially built**: shell + multi-scene pages
   (one-bracket rendering), scene hierarchy with full command coverage, reflection-driven inspector,
   gizmos (one-undo-per-drag), material editor page, asset browser (inline rename + group delete),
   model importer (authored names, textures, tangents, MR bake), three-phase responsive cook, and
   scoped cook with dependency closure — all committed. **Prefabs ✅, play-in-editor ✅, and the
   whole scripting/subsystem stack ✅ since.** Now: export hardening + an integrated sample toward
   the **MVP-to-Export** milestone below (which is effectively feature-complete).
2. **Core / content-pipeline hardening** (editor prereqs, backfill as the editor needs them) —
   ~~content-DB text format~~ ✅, offline cooker, prefabs (+ model→prefab), fonts→triad, texture-cook
   completeness, scene-serialization robustness. *(hot-reload deferred — not needed pre-editor.)*
3. **Portability groundwork** - ✅ web SHIPPED (Emscripten + WebGPU wgpu-native sidecar; the
   3D scene, web player, and AngelScript all run in-browser off the naga WGSL cook). Remaining:
   web networking/Firefox/export polish, then Android.
4. **Gameplay-runtime subsystems** — ✅ audio (miniaudio, P1-P3), ✅ physics (Jolt, P1-P3 +
   CCD/mass + shape queries), ✅ input action-mapping, ✅ game-UI subsystem, ✅ navigation
   (Recast/Detour), ✅ terrain (complete; follow-ups in terrain-backlog.md).
5. ~~**Gameplay / scripting**~~ ✅ **SHIPPED far beyond the original line** — two CERTIFIED
   backends (AngelScript + Luau; Wren retired), coroutines, the full entity-behavior tier (harvested
   properties both languages, lifecycle, events, Scene facade, delegates, introspection),
   ScriptPage, and an AngelScript step debugger (P1). See docs/design/scripting.md §9 for
   status + follow-ups and docs/design/script-debugger.md for the debugger track.

---

## Track: General (build / test / process)
- [x] CMake presets — clang Debug, clang RelWithDebInfo, gcc Debug (+ ctest presets)
- [x] **Release/Shipping presets** (2026-07-29) - clang/gcc `-release` and `-shipping`
      (`DRACONIC_SHIPPING_BUILD` → asserts + Trace/Debug/Info logs compile out via
      dead-ternary macros, `-s` strip, `-Shipping` output suffix). First sizes: Player 7.9MB,
      Editor 11.1MB, Cook 7.0MB, Export 8.2MB (vs 49-83MB Debug).
- [x] Sanitizer build dirs (asan/tsan)
- [x] ~2,700+ doctest cases across 99 `Tests/` dirs (421 test .cpp files) - as of 2026-07-29
- [x] Asserts/fatals print their own backtrace (`sys::WriteBacktrace`, System-backend per
      platform; Linux execinfo, Win32 CaptureStackBackTrace pending validation)
- [x] clang-format / clang-tidy / clangd config
- [ ] Local convenience test-runner (one command → all ctest presets)  *(no hosted/paid CI for now)*
- [ ] Keep design docs current as tracks land (ongoing)

## Track: Core
**Foundation**
- [x] Containers, allocators (linear/stack/pool/frame/tracking), IO/streams, string/UTF-8/format, log
- [x] Reflection + RTTI (properties, instance/static methods, base chains, enum + value types)
- [x] **StringHash + TypeDomain** (2026-07-29) - constexpr FNV-1a string identity (`:string_hash`);
      TypeRegistry tags registrations with an OPEN string-named domain (core names only the
      default "Runtime"; editor sites pass `TypeDomain(u8"Editor")`; re-registration widens to
      Runtime, never narrows). Feeds the script API browser's editor-only markers + a future
      export lint.
- [x] Serialization — one `ISerializer`, **binary + XML** backends, polymorphic load
  *(JSON not needed — XML covers human-readable, binary covers cooked)*
- [x] JobSystem — work-stealing, task-graph deps, `ParallelFor`, caller-participation (WASM-safe)
- [x] Threading primitives (Win32 + Linux backends)
- [x] Time/Clock/Stopwatch types — `Duration`/`TimePoint`/`Clock`/`Stopwatch` in `:time`
      (`System/Time.cppm`) over the tick primitives; 10 test cases; DesktopRunner + SampleApp migrated
      off `std::chrono`
- [~] **SIMD math** (packed vs SIMD split) — *perf; phased, packed-default / SIMD-opt-in:*
  - [x] **Phase 1 — packed baseline**: renamed `Vector2/3/4`→`Float2/3/4`, `Matrix3/4`→`Float3x3/4x4`
        engine-wide (packed, unaligned, shader-layout-matching; the default everywhere). Case-sensitive
        `\b`-anchored rename — provably didn't touch shader HLSL (lowercase `floatN`). Green on
        clang+gcc, all 58 suites pass. `Quaternion`/`Color`/`Plane`/`AABB`/`Transform` keep their names.
  - [x] **Phase 2 — SIMD types**: `alignas(16)` `Vector2/3/4` + `Matrix4` (`:simd`/`:simd_vector`/
        `:simd_matrix`), SSE2 backend + scalar fallback (ARM/WASM), explicit `Float*`↔`Vector*`
        conversions, full API mirror. Equivalence tests vs the packed reference — **both SSE and the
        forced-scalar path pass**. Additive/unused → zero behavior change. (NEON/WASM-SIMD specializations
        deferred; scalar fallback is correct there today.)
  - [~] **Phase 3 — guards done; migration profile-gated**:
    - [x] `static_assert(sizeof/alignof)` guards on GPU-facing structs (vertex streams + `alignof`;
          `ObjectData`/`InstanceData`/`ShadowViewData` cbuffer mirrors) so a stray aligned SIMD type in
          a vertex/cbuffer trips at compile time. Many structs already had size guards.
    - [ ] Hot-path migration — **measured, deferred**. A perf probe (RelWithDebInfo) showed the naive
          swap (SIMD multiply but data stays in packed `Float4x4`) is worthless (~1.05x — per-op
          load/store eats it, and optimized scalar auto-vectorizes). The real win (~1.8x on isolated
          matmul) needs data kept in SIMD form across ops (a data-layout change), which is only worth
          it in a loop *profiling proves* dominates a frame. Not done speculatively; revisit with a
          real frame profile.
- [ ] Concurrent / lock-free containers (only the JobSystem's internal work-stealing deque exists
      today) — *as needed*
- [ ] Allocators: general-purpose heap (TLSF/buddy) + debug guard-page/quarantine allocator (only the
      tracking allocator exists today) — *as needed*
- [ ] Unicode: normalization + collation (only basic decode/encode helpers today) — *as needed*
- [ ] Pak: compression (zstd/lz4; only `kCompressionNone` today) + runtime write/watch — *later*

**Content & asset pipeline** *(core infrastructure — the near-term focus)*
- [x] Content DB — GUID identity, `.rasset` envelope + sidecar streams, deterministic GUIDs
- [x] General asset-name dedup - `Group::UniqueInstanceName`/`UniqueGroupName` ("Base.2"
      convention), every creator/duplicate/import site swept onto it (2026-07-29); importer
      reuse-taken-name paths deliberately kept (reimport idempotency)
- [x] ResourceManager — `Proxy<T>`, auto dependency edges, transitive reload (tested)
- [x] Editor asset base — `Asset`/`IAssetBuilder`/`DefaultAssetBuilder` source→product cook
- [x] Consistent triads: Texture, Geometry, Image, Materials, Particles, Animation, Shaders
- [x] **Content DB text/XML format** — human-readable editor DB via a pluggable `SerializerFactory`
      on `ContentDatabase(mount, factory)`: editor DB opens with `XmlSerializerFactory` (readable/
      diffable), cooked stays `BinarySerializerFactory`; same `SerializerContext`/`ISerializer`
      contract, both round-trip-tested.
- [x] **Hot-reload wiring** — `NativeFileSystem` implements `IWatchableFileSystem`/`Poll`; the editor's
      `CookService` polls the source watcher (`AsWatchable()`), recooks the dirty asset, and the editor
      app drives `ResourceManager::Reload` (transitive reload machinery already tested) → live in-editor.
- [x] **Offline cooker tool** — `DraconicCook` CLI (`Code/Tools/Cook`): headless, opens the project,
      registers every builder, plans + executes the incremental cook (`--rebuild` / `--dry-run`;
      exit code = failed cooks). Editor + cook exes assemble the full pipeline.
- [x] **Fonts → proper triad** — SHIPPED (all 6 phases): FontAsset triad through the content
      DB/ResourceManager, project templates carry fonts, embedded editor font, MSDF UI rendering on
- [~] Texture cook completeness — cubemap cook, **cook-time mip generation** (AppendMipChain),
      and **BC block compression** (bc7enc BC7 + rgbcx BC1/3/4/5, policy-driven) all SHIPPED.
      Still missing: **BC6H for HDR + the normal-map format fix** (spec ready:
      texture-compression-hdr-and-normals.md, after texture-page-ux) and **KTX2/basis import**
      (ties to the WASM/Android transcode path)
- [x] Skinned-model full PBR maps — skinned meshes share the static forward path; the `Skinned` flag
      is vertex-stage only (bone skinning + skin stream), and set 2 binds the same **data-driven
      material** (all declared textures: albedo/normal/MR/occlusion/emissive). Not albedo-only.
- [x] Scene-serialization robustness — **v2 length-prefixed component + system-settings records**:
      readers SKIP unknown component types instead of aborting (tested: "unknown component types SKIP;
      later records still load"), prefab override records drop unknown types, count-guards
      (`kMaxPrefabRecordEntries`) against corrupt streams. *(Field-level version migration is still
      minimal — add per-component versioning as formats evolve.)*

## Track: Rendering
- [x] Clustered forward PBR, IBL + analytic sky, SSR
- [x] Shadows — CSM + spot + point, with caching
- [x] Decals, sprites, static instancing (MultiMesh), multi-view, color management
- [x] **Reflection probes** (parallax-corrected, cluster-assigned — *implemented*; may have minor TODOs)
- [x] View-frustum culling, CPU + GPU profiling, immediate debug draw
- [x] Task-graph render pipeline (WASM-target-driven, no fibers)
- [x] Post-FX stack (TAA, GTAO, bloom, FXAA, tonemap)
- [~] **Post-processing config** - per-scene authored, per-view applied (post-processing-config.md):
      phases 1 + 2a + 2b + 3 SHIPPED; PAUSED pending on-screen verify; phases 4/5
      (grading/DoF/volumes) not started
- [x] GPU skinning
- [~] DX12 backend — complete except 1 TODO (blit pipeline needs `D3DCompile`, `DxDevice.cppm:666`)
- [ ] **GPU memory sub-allocation** — today the RHI does **one allocation per resource** (Vulkan:
      `vkAllocateMemory` per texture/buffer in `VkTexture`/`VkBuffer`; DX12: committed resources). This
      trends toward the driver's `maxMemoryAllocationCount` ceiling and wastes memory on alignment
      padding. It already bit us once: a transient-pool leak under viewport-resize churn exhausted VRAM
      (fixed by draining the pool, but the per-resource-allocation fragility remains). Plan:
  - [ ] **Vulkan → VMA** (AMD Vulkan Memory Allocator) — sub-allocate images/buffers from pooled
        `VkDeviceMemory` blocks behind the existing `Create*`/`Destroy*` seam (no call-site changes)
  - [ ] **DX12 → D3D12MA** (D3D12 Memory Allocator) — the equivalent: placed resources from pooled heaps
        instead of committed resources, same RHI seam
  - [ ] Keep the `[VkTexture] vkAllocateMemory FAILED` / RenderGraph transient-alloc warnings as the
        regression tripwire
- [~] Spatial acceleration — brute-force sphere view-frustum culling SHIPPED (default-off,
      per-cascade shadow culls decoupled); a real spatial structure (BVH/octree) not started
- [x] **Move shaders out of C++ code — SHIPPED** (P1 .hlsl files + provider seam + hot reload,
      P2 cooked per-backend bytecode, P3 WGSL cook via naga for web). Original design notes:
      supersedes the old sketch here). HLSL stays the authored source; every inline string bank
      lifts to VFS-mounted engine `.hlsl` files. Two resource tiers: engine builtins stay
      name-addressed through an `IShaderSourceProvider` seam (renderer stays resource-agnostic);
      project shaders are Guid resources (MaterialSource already encodes both). Dual mode:
      dev = runtime DXC (hot-reload), export = cooked per-backend bytecode so dists drop the DXC
      sidecar. Variants declared in-source (`// draconic:variants A B`), request canonicalized
      against the declared mask, power-set cooked - dist misses impossible by construction.
      SPIR-V → WGSL via an external cook tool (feeds the web track). Implementation not started;
      the editor Shader page comes LAST (CodeEditView prerequisite now exists).

## Track: UI (framework)
- [x] VG 2D vector-graphics stack (Path/Fills/Tessellation/GPU renderer/SVG) — the primitive layer
- [x] Dear ImGui integration (tooling/debug)
- [x] **Retained-mode UI toolkit — seeded from Sedulous.UI** (draconic.ui: ~31 controls + all
      subsystems + toolkit ~40 controls; DarkTheme, text input, VFS/shell adapters, docking via
      `DockManager`/`DockablePanel`). **Proven on-screen** in UISandbox. VG is the render adapter.
- [x] Fresh eepp-derived **draconic.gui** (full CSS + theming + docking-capable widgets) — retained
      alongside as the alternative framework.
- [x] **CodeEditView** (2026-07-29, code-editor.md) - purpose-built code editor in ui.toolkit:
      CodeDocument (delta undo, markers, find, compound edits) + virtualized CodeEditView
      (gutter/breakpoints, find/replace bar, self-drawn completion popup), line-state lexer
      model + registry (toolkit ships NO language tables - the backends' lexers + API
      completion live in `Script/{Language}/Editor/`), diagnostics, hover values, execution
      arrow. All 4 phases shipped + user-verified.
- [x] Runtime integration — draconic.ui on the multi-window runtime (UIHost + ui.application docking
      host, OS floats/drag/redock, multi-window input routing); at Sedulous parity or better. *(see
      Subsystems: UI)*

## Track: Subsystems
*(runtime integration of each system into the Context/Scene, Sedulous-style)*

**Scene** — [x] ECS (gen-guarded pool, sparse-set components, phase systems), transform hierarchy
(dirty cascade + motion vectors), `ISceneAware` injection, **whole-scene serialization (tested)**
- [x] **Prefabs / entity templates** — authored template + instantiate-with-overrides (COMPLETE,
  nesting/order/placement, text XML sources) — user-confirmed 2026-07-17
- [x] **Model import → save as prefab** (hierarchy-preserving import cook)
- [x] **GameInstance** - "a running game" as a first-class object (owns the run host, a
      SceneManager scene-group, per-instance NetworkManager + ActionRuntime + input isolation);
      SceneSubsystem reduced to a PURE REGISTRY (no default manager - every owner registers its
      own SceneManager). Track complete (2026-07-22, 2231caf + follow-ups).
- [ ] Scene streaming / partitioning — *later*

**Render** — [x] integration seam done (register-renderer / register-provider, per-view draw)

**Particles** — [x] CPU sim + full authoring triad (Sedulous parity+); [ ] GPU-compute sim — *deferred*

**Animation** — [x] skeleton/clip/pose/sampler/player + state machine (blend trees, bone masks,
layers), triad + subsystem; skinned crowds proven

**Input** — [x] event stream + devices (keyboard/mouse/gamepad/touch); [x] **action/mapping/binding
layer** (P1+P2: runtime/asset/interactions/rebind/InputMapPage/script facade) — rest of P3 blocked on
game-ui consumers

**Audio** — [x] **SHIPPED** (miniaudio; P1-P3 + niceties): 4 buses, pooled voices + steal, per-scene
groups, 3D spatialization + distance LPF, SoundCue (weighted no-repeat), Freeverb + listener zones,
multi-listener, BusLayout-as-data, audition + SoundCue editor pages, Audio script facade. Grain banks
deferred (documented growth path). See audio.md

**Physics** — [x] **SHIPPED** (Jolt; P1-P3): cooked shapes + plane, collision assets + import,
rigid bodies, raycasts, CharacterVirtual, joints w/ motors, 32 collision groups + inspector matrix,
contact events → script behaviors (composition-root bridge). See physics.md

**Networking** - [~] **draconic.net P0-P2 SHIPPED, demo confirmed on-screen 2026-07-22** (two
editor tabs host/join, cube replicating over loopback UDP). P0 wire + reliable-UDP + UDP/TCP
sockets; P1 session/clock/RPC + NetworkSubsystem + Net facade on BOTH script backends +
DefaultApplication game-wiring; P2 replication core (draconic.net.replication: reflection field
codec → snapshot → per-peer delta → prefab spawn + late-join → interpolation → relevancy/
fog-of-war). First target genre RTS (genres kept open via the IReplicationModel seam).
- [x] StateReplication + interpolation wired through the run scope (GameInstance carries its
      NetworkManager; replication rides the fixed/render tick); net extraction spec ready (P1-P4)
- [ ] Deferred: per-field bitmask delta, receive-RPC-into-script, project-settings → NetworkStartup
- [ ] Win32 socket backend needs Windows validation

**AI / Navigation** — [~] navmesh/pathfinding COMPLETE (Recast/Detour: zones + editor bake +
agents/crowd + FindPath, wasm-clean; terrain surfaces feed the bake since 2026-08-31); behavior
trees not started

**Terrain** — [x] COMPLETE — heightfield triad, chunked geo-mip LOD, top-K splat painting +
sculpt brushes, per-layer PBR/height/mask, nav-bake integration. Streaming/paging + the other
follow-ups live in terrain-backlog.md

**UI (subsystem)** — [x] draconic.ui wired into the multi-window runtime (UIHost + ui.application
docking host + input routing); dockable 3D viewport (`draconic.ui.viewport`) hosted per scene page
- [x] **Game-UI subsystem** (P1+P2): per-scene UI roots, screen/scene overlay roles, world-space
  UI panels (RT canvas → sprite), actions-only consumption, GameTheme. See game-ui.md

## Track: Portability
*(web plan DECIDED 2026-07-28 - docs/design/web-platform.md; browser milestone gated on the
shaders track's WGSL path)*
- **Web / Emscripten**
  - [x] **P0: emcc + CMake + C++23-modules spike - PASSED 2026-07-29**: draconic.core +
        full doctest suite green under node as wasm32 (215 cases / 8,073 assertions);
        `cmake --preset wasm`; incremental-climb gate in the root CMakeLists; portability
        fixes in web-platform.md. The track is UNBLOCKED.
  - [x] **WebGPU RHI backend - COMPLETE, user-verified 2026-07-30**: native-first
        wgpu-native sidecar against `webgpu.h`; runs the RHI sample battery (26/30 + 4
        graceful skips), the FULL renderer (Sandbox), the UI stack (UISandbox), and
        the EDITOR - zero validation errors, visually confirmed vs Vulkan. All
        executables take --webgpu/--vulkan/--dx12 (shared SelectBackendFromArguments).
  - [x] Runtime compiles + runs under WASM/emscripten — the 3D scene, the web player, and
        AngelScript-on-web all work in-browser (WebScene target; naga WGSL cook)
  - [~] Web shell/runner — single-threaded browser main loop working; remaining: web
        networking (WebSocket client), Firefox validation, export integration
- **Android**
  - [ ] Android shell backend (Vulkan already covers the GPU side)
  - [ ] Touch/input + build
- **Apple** — deferred (no Mac hardware)
- **Console** — keep abstraction boundaries clean now; implement later behind NDA SDKs

## Track: Gameplay
- [x] **Scripting → ECS — SHIPPED** (the biggest track; see scripting.md §9 + script-debugger.md).
      Game-script layer (the `Game` class in DraconicPlayer/PIE) AND the full **entity-behavior**
      tier: ScriptComponent, cooked ScriptClass + per-language cook/harvest, lifecycle + simulation
      gating, hot reload, property overrides + inspector. **Backend-NEUTRAL, proven**: AngelScript +
      Luau both CERTIFIED (registry + conformance battery + capability flags; Wren retired). Coroutines
      (both backends), `entity.send`, physics/UI events, `Scene.spawn`/`find`, Input/Audio/Physics
      facades, delegates + API introspection, ScriptPage editor, AngelScript step debugger (P1).
      Follow-ups (non-blocking) in scripting.md §9.
- [x] **Multiplayer foundations** - draconic.net P0-P2 + per-instance NetworkManager on
      GameInstance + Net script facade (see Subsystems: Networking; replication wiring into the
      tick is the open item)
- [~] Save-game / settings-config / localization — settings + export templates SHIPPED
      (uniform CLI+editor export incl. the WEB target); i18n foundation layer SHIPPED
      (v2 ruling: foundation-only, no subsystem). Save-game still open
      (correction 2026-10-17: the i18n layer was never built; this sync counted its v2 spec as
      shipped. The revised spec is Specs/i18n.md.)
- [~] Gameplay sample — **PaperKid** (P1 playable in-editor) exercises input-actions +
      physics + audio + script-behaviors + prefabs + game-UI; networking is the one subsystem
      it does not touch (the net demo scripts remain that seed)

## Milestone: MVP-to-Export (agreed 2026-07-12)

**Goal: from the current state, reach "export something playable" with the CURRENT subsystems
plus scripting; fill in other subsystems (audio/physics/...) after.**

**Progress (updated 2026-07-20):** the critical-path tools EXIST under `Code/Tools/` — `DraconicCook`
(1), `DraconicExport` (2), and `DraconicPlayer` (3). **Scripting (3) is done far beyond MVP** — the
game-script layer AND the full entity-behavior tier on two certified backends (see the Gameplay
track). **Play-in-editor (4) SHIPPED** — GameEditorPage hosts a fresh player run (P1+P2, embedded
host + per-scene time). **Prefabs SHIPPED.** Remaining for a shippable MVP: audit export depth/
hardening + a real end-to-end export smoke, and the optional native-module seam (5). The
subsequent tracks (audio/physics/input/game-UI) that were "fill in after" are ALSO already
shipped — the MVP is effectively feature-complete; the gap is export hardening + an integrated
sample, not missing subsystems.

Key survey finding:
Sedulous never loaded DLLs - its "game module" is a statically-linked IApplication in a shared
game lib consumed by per-project App/Editor executables (TowerDefense pattern), with the
lifecycle contract Configure/OnStartup = edit-time (register subsystems + reflected types) and
OnLaunch/OnExit = per game run only. We adopt that contract AND go one further with a generic
player, which Sedulous lacked.

Critical path (order):
1. **DraconicPlayer** - generic runner: engine subsystems + mount cooked DB + load the project's
   default scene + run with simulation enabled. Zero native game code required; with scripting,
   player + scripts + cooked content IS the game.
2. **Export CLI** - clean shipping cook (DraconicCook shares the pipeline DB) -> stage player exe
   + Content.pak + built-in assets into dist/<platform>/. PAK FROM DAY ONE (refined 2026-07-12):
   draconic.vfs.pak already ships PakFileSystem (read+enumerate, exactly what ContentDatabase
   needs at runtime) + PakBuilder (offline writer, tested; format reserves compression - codecs
   deferred). Loose-dir mount stays as the debug option. NOTE: packaging is what will eventually
   force the shaders-out-of-C++ sub-track (runtime DXC -> precompiled); acceptable for MVP.
3. **Scripting (MVP-required)** - TWO layers (refined 2026-07-12). (a) The GAME SCRIPT: a
   project-level coordinating script (manifest-referenced, e.g. game.wren) = the SCRIPTED
   counterpart of the native IApplication lifecycle (launch/update/exit) - orchestrates ABOVE
   scenes (which scene to load, game states, transitions). Hosted identically by DraconicPlayer
   and PIE (Play = fresh script context + fresh scene from cooked content; Stop = context
   teardown, total cleanup). Run-only for MVP (no edit-time Configure hooks - reload/sandboxing
   questions deferred; script-defined editor-visible types arrive with behaviors). A native
   module later replaces the script at the same seam. (b) ENTITY BEHAVIORS: per-entity scripts
   via a script component, ticked under simulation - separate, later. Builds on the proven
   reflection->Script->Wren prototype (IScriptManager/IScriptContext: CreateInstance + Call).
4. **Play-in-editor (8b)** - GameEditorPage singleton tab hosting the PLAYER behavior (fresh
   run from cooked content - distinct from Simulate's in-place snapshot/restore); input via
   InputSurface/InputRouter gated on the tab; preview-resolution modes; later the same page
   launches a native module's OnLaunch(EditorApplicationHost) when that story lands.
5. Native game module = static-link per-project targets (<Game>.Core lib + generated App/
   Editor exes) WHEN a project needs native code; manifest nativeModule field reserved.
   DLL loading/hot-reload = phase-never unless demand appears (gameplay iteration = scripts).

~~Deferred until after MVP: prefabs~~ ✅ prefabs DONE; ~~other subsystems~~ ✅ audio/physics/
input/game-UI DONE (and networking since). Still deferred: editor follow-ups 7-8 (thumbnails,
drag-from-assets, importer chooser, scatter tooling), portability packaging (WASM/Android ride
their shells).
**STATUS 2026-09-01: everything the 2026-07-29 "next planned" list named has SHIPPED**
(export templates incl. WEB, reflection P2, web platform, shaders, AI/nav, terrain) except
post-processing phases 4/5 (phases 1-3 built, PAUSED pending verify). Current planning lives
in the weekly docs (Documentation/Plans/week-*.md; the 2026-09-05 doc carries the triaged
project seeds) - this milestone is effectively feature-complete, and the remaining MVP gap is
unchanged: export-depth hardening + one integrated showcase sample (PaperKid covers most of it).

## Track: Editor  *(ACTIVE — the editor app on draconic.ui; see docs/design/editor.md)*
- [x] Editor app shell + multi-scene pages (one-bracket rendering across scenes)
- [x] Reflection-driven inspector (leverages full runtime reflection; attribute conventions +
      component annotations)
- [x] Scene hierarchy panel with full command coverage + gizmos (one-undo-per-drag) + dockable viewport
- [x] Material editor page (per-asset preview choice, unlit preset, responsive re-cook)
- [~] Asset browser over the content DB — browse + import + inline rename + group delete + scoped/
      group cook done; **thumbnails SHIPPED** (CPU lane: texture/image/font/audio/heightfield/
      splatmap; GPU stage: mesh/material/prefab/scene/particle/collision/skeleton/model).
      Drag-from-assets + importer chooser still deferred
- [x] Model importer — hierarchy-preserving import cook (authored names, full texture wiring, usage
      color spaces, MR bake, generated tangents)
- [x] Cook pipeline — thread-safe three-phase cook (worker plan → main pre-create → worker build),
      cook-gated structural mutations with toasts, scoped cook + dependency closure, per-item release
- [x] Model-import → **save as prefab** workflow (prefabs shipped)
- [x] Play-in-editor (GameEditorPage hosting a fresh player run — P1+P2, embedded host + per-scene
      time; Simulate 8a pre-existed) + **script debugger** (breakpoint gutter + debugger panel)
- [x] **Script debugger editor story** (2026-07-29, script-debugger.md P1.5) - execution arrow +
      scroll-to-line in ScriptPage, hover-values while paused (EditorContext::ScriptValueProbe),
      LIVE breakpoint add/remove mid-run (diff-sync to the AngelScript debugger), pause-gated
      script tick (suspension is not a fault). Next: P2 profiler / P3 remote.
- [x] **ScriptPage autocomplete** - shipped via CodeEditView: completion over the backend's
      ACTUAL bound API (IScriptManager::DescribeBoundApi through a throwaway manager), plus
      markup-element/attribute completion on UIDocument pages
- [x] **ScriptPage API browser** - shipped (code-editor track COMPLETE): openable side panel
      (API toggle in the status row) with a filter box over a class > members tree of the bound
      API; completion and the browser now share ONE ScriptApiSurface (same data, built once);
      double-click inserts at the cursor. Editor-only bindings are MARKED " [editor]" in both
      the browser and completion: TypeRegistry gained open string-hash TypeDomains (core
      StringHash added; core names only the default "Runtime"; */Editor/* registration sites
      pass TypeDomain(u8"Editor"); widen-only re-registration rule), and ScriptApiType
      carries its TypeId. Future payoff: an export-time lint for player-bound scripts
      referencing editor-only API
- [x] **Bespoke per-asset pages** - the full set shipped: Texture, Mesh, Image, Heightfield,
      Terrain, Font, Material, AnimationClip/Graph, Skeleton, ParticleEffect, CollisionShape,
      InputMap, AudioClip/SoundCue/BusLayout, and the generic reflected AssetFormPage
      (recipe in editor-bespoke-pages)
- ~~Depends on: content-DB text format, offline cooker, prefabs~~ ✅ all shipped

---

## Decisions

- **Web platform (2026-07-28, web-platform.md).** Emscripten + WebGPU. The WebGPU RHI backend is
  built **native-first against wgpu-native** (`webgpu.h`), validated on desktop with the existing
  RHI tests/samples before any browser build; web v1 is single-threaded; the browser milestone is
  gated on the shaders track's WGSL cook. **P0 = emcc + CMake + C++23-modules spike** - if modules
  don't fly under emcc, the plan reshapes. (Supersedes the older open "here-first vs Sedulous-
  first" question - here-first stands. *Resolved.*)
- **Shaders (2026-07-28, shaders.md).** HLSL stays the single authored source, lifted out of C++
  string banks into VFS-mounted engine `.hlsl` files. Renderer stays resource-agnostic via
  name-addressed builtins (`IShaderSourceProvider`); project shaders are Guid resources. Dev =
  runtime DXC, export = per-backend cooked bytecode (dists drop the DXC sidecar). Variants are
  in-source declarations, canonicalized + power-set cooked. WGSL via an external SPIR-V cook tool.
  *(Resolved - implementation not started.)*
- **No JSON serializer.** XML (human-readable) + binary (cooked) are sufficient; the content-DB text
  format need is served by XML. *(Resolved.)*
- **No hosted/paid CI for now.** Rely on local ctest presets + sanitizer builds; optionally add a local
  run-all script. *(Resolved.)*
- **Subsystems are not rushed.** Infrastructure + tooling first; the proven subsystem architecture means
  audio/physics/AI/terrain can follow once the foundation and editor are solid. *(Resolved.)*
