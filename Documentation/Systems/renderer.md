# Renderer

> Status: CURRENT
> Verified: 2026-08-12 @ f3be260f
> Track: [[renderer-design]] / [[renderer-roadmap-status]]

The renderer is built and shipping. The full plan landed: the extraction/view core, resource +
submission rebuild, RenderGraph passes (depth prepass + forward MRT), clustered lighting + PBR, CSM
shadows (cascades + spot + point + caching), IBL + sky, the post stack (TAA/GTAO/bloom/tonemap/FXAA),
skinning, reflection probes, decals, debug draw, sprites, multi-view/multi-scene, SSR, and view-frustum
culling. This document is the design record of what was built - the body below reads in its authoring
tense but describes the DELIVERED system.

## Modules

- **`foundation.render`** (`Code/Foundation/Render`) - the scene-AGNOSTIC renderer core (the `:data`
 render-data contract, `MeshRenderer`, the pass implementations); knows nothing about scenes.
- **`foundation.render.api`** - the render API/data layer shared across the boundary.
- **`engine.render`** (`Code/Engine/Engine.Render`) - `RenderSubsystem`, the Context-level driver that
 connects scenes to the core (implements `ISceneRenderer`: `Begin` / `RenderScene` xN / `End`).

Load-bearing dependencies: `foundation.rhi` (explicit - bind groups + PSOs + dynamic rendering +
barriers), `foundation.rendergraph` (automatic barrier solving, transient aliasing, topo-sort + culling),
`foundation.materials` (data-driven; `MaterialSystem` infers set-2 bind-group layouts; the PSO cache with
version-poll hot reload - see `shaders-materials-hot-reload.md`), `foundation.shaders(.system)`,
`foundation.geometry` (GPU-ready mesh streams), and `foundation.scene` (entities/transforms/components +
`SceneModule` install + `ISceneObserver` lifecycle - [[scene-composition]]).

Future optimization + extension work (the CPU->GPU-bound parity work - parallel command recording +
draw-list sort, shadow instance reuse - the GPU-driven/indirect path, and the spatial acceleration
structure) is tracked in the weekly backlog (week-2026-09-05, "absorbed from renderer-improvements"); analysis in `Documentation/Archive/renderer-improvements-history.md`. The instanced-mesh (MultiMesh)
primitive is `Documentation/Systems/instanced-mesh.md`.

_(Original design grounding, preserved: the thin-slice ([[renderer-thin-slice]]), a critical assessment
of Sedulous.Renderer, and references from PlayCanvas (composition + uniform delivery) + Babylon-Lite
(PBR depth). The spinning-cube thin slice was the throwaway that proved the foundation integrates.)_

---

## 1. Goal & non-goals

**Goal.** A clustered forward+ renderer that is *correct by construction* in its shading,
*extensible* (new renderable types and whole subsystems plug in without touching the
core), and *concurrency- and GPU-driven-ready* in its submission - so it scales past the
point where Sedulous.Renderer silently caps out.

**Scale target.** Tens of thousands of draws and hundreds of lights without falling off a
cliff or *silently dropping* work. (Sedulous is correct but tops out: single-thread
command recording, no GPU-driven path, hard caps that drop draws/shadows silently.)

**Non-goals (v1).** Deferred/visibility-buffer rendering, ray-traced GI, virtual
shadow maps, mesh shaders as the primary path. We design seams for these but don't build
them. Editor-specific rendering (gizmos/picking) is a thin pass, not a focus.

---

## 2. Principles

1. **The renderer is scene-agnostic.** Render data is *extracted and pushed*; the renderer
 never reads a scene/entity. One-way dependency: `engine.render` → `foundation.render`.
 (Already true in the slice; non-negotiable. Sedulous honors the same boundary.)
2. **Extensible by category, not by editing the core.** A renderable type contributes a
 `RenderData` subclass tagged with a `RenderCategory`, an extractor, a per-category
 `Renderer` (drawer), and optionally a `Pass`. The core sorts by category and dispatches.
 Proven by Sedulous's particles/sprites/decals/UI all plugging in from *separate libraries*.
3. **Keep the correct parts, rebuild the compromise layer.** The Sedulous shading/IBL/post
 math, the clustered-light compute, the CSM math, and the resource-pool patterns are
 genuinely good - port them close to verbatim. The submission frame loop, the
 pointer-identity batch cache, the shadow scheduler, and reflection probes are the
 compromise layer - rebuild them modern. (See §3 boundary.)
4. **Data-driven materials; custom materials need no renderer changes.** Materials declare
 properties → `MaterialSystem` infers the set-2 bind group; the renderer binds it as an
 opaque handle. A front-end/back-end shader split lets custom materials reuse the lighting
 back-end (PlayCanvas idea, serves the material-model decision already made).
5. **Concurrency- and GPU-driven-ready from the architecture, even if filled in later.**
 Double-buffered extraction (extract frame N while frame N-1 renders), parallel command
 recording (secondary/per-thread encoders), and an indirect-draw seam are designed in;
 the first implementation may be single-threaded/CPU-driven but must not *preclude* them.
6. **Barriers/transitions are the RenderGraph's job, not hand-rolled.** All passes declare
 reads/writes; `foundation.rendergraph` inserts barriers + aliases transients. No manual
 ping-pong state tracking (a documented bug-seam in Sedulous).
7. **No silent drops.** Every cap degrades gracefully (LRU/importance) and logs; never
 render-black-and-move-on (a class of bug Sedulous's comments admit it hit).
8. **Views are isolated; the renderer renders a *set of views* per frame, not "the frame."**
 Multiple scenes side-by-side (editor docked pages), the same scene from multiple cameras,
 and derived views (shadows, probe faces) are all the same mechanism. The ONLY state shared
 across views is the immutable per-scene extraction + shared GPU resources; everything
 mutable-per-frame is per-view. No view can trash another. (§9 is the full design; this fixes
 Sedulous's multi-scene-stomping class of bug by construction.)

---

## 3. The keep-vs-rebuild boundary (the heart of this design)

Earned from the critical assessment, not asserted. "Port" = translate Beef→C++23/Raptor
idioms close to verbatim. "Rebuild" = take the idea, write a modern implementation.

| Subsystem | Verdict | Why |
|---|---|---|
| **PBR/IBL/post shaders** (`forward.frag`, cluster, shadow, TAA, bloom, tonemap, prefilter/irradiance/BRDF-LUT) | **PORT** | Correct + modern: height-correlated Smith, energy-conserving, geometric specular AA, correct split-sum, ghosting-aware TAA, COD-grade bloom. Rewriting reproduces it. |
| **Clustered-light compute** (log-Z froxels, view-space AABB, GPU cull) | **PORT** (fix 32-cap + dead `CLUSTER_MAX_LIGHTS`) | Real GPU compute, correct math + NDC/Y-flip handling. |
| **CSM math** (practical split, sphere-fit cascades, cube-face seams) | **PORT** (ADD light-space texel snapping) | Fundamentals right; only missing the shimmer fix. |
| **GPU resource layer** (ring/dynamic UBO allocators, pooled bone/skinned-vertex storage, generation-handle deferred deletion, deferred bind-group destroy) | **PORT (pattern)** | Production-shaped; this is what prototypes lack. |
| **RenderGraph** | **ALREADY OURS** | `foundation.rendergraph` already ported + tested. |
| **Extraction data model** (`RenderData` + radix sort on inline 64-bit keys + category dispatch + provider boundary) | **PORT (model), REBUILD (frame-level)** | Model is excellent; single-buffering is the rebuild. |
| **Skinning** (GPU, pooled bones, prev-frame for motion vectors) | **PORT** | Complete + perf-aware. |
| **Submission frame loop** (extraction sync with sim, single-buffered, single-thread recording, no GPU-driven) | **REBUILD** | The core scale weakness - double-buffer + parallelize + indirect seam. |
| **Batch cache** (pointer-identity keys + retry loop) | **REBUILD** | Brittle under pool pointer reuse; use stable IDs. |
| **Shadow scheduler + atlas** (full re-render every frame, fixed 22-slot atlas, silent shadow drop) | **REBUILD** | Add static-shadow caching, importance/screen-size atlas, graceful fallback. |
| **Reflection probes** (nearest hard-swap, no parallax/blend) | **REBUILD** | Least-finished; add parallax-corrected box/sphere + blend weights. |

---

## 4. Architecture & module layout

Two libraries, one-way dependency, mirroring the slice but grown:

```
foundation.render (SCENE-AGNOSTIC renderer core)
 :data - RenderData base + RenderCategory + ExtractedScene (per-scene snapshot)
 :views - RenderView (the isolation boundary) + RenderViewPool + view-list assembly
 :extract_ctx - RenderContext, FrameArena, per-worker arenas, double-buffer
 :resources - GpuResourceManager (mesh/texture pools, bone pool), PerFrameResources (ring UBOs)
 :pipeline - PassList (the pass-list TEMPLATE applied per view) + PipelinePass + Renderer
 (per-category drawer) registry + the per-frame Frame (one RenderGraph, all views)
 :passes - DepthPrepass, ForwardOpaque, ForwardTransparent, Sky, Decal, Post, Overlay
 :lighting - ClusterSystem (per-view grids), LightBuffer
 :shadows - ShadowSystem, ShadowAtlas, CSM matrices, ShadowScheduler
 :ibl - IBLSystem (prefilter/irradiance/BRDF LUT), ProbeSystem
 :post - TAA, SSAO, Bloom, FXAA, Tonemap
 :skinning - BoneMatrixPool, SkinnedVertexPool
 deps: foundation.rhi, foundation.rendergraph, foundation.materials(+pso), foundation.shaders(.system),
 foundation.geometry, foundation.core

engine.render (SCENE-COUPLED integration)
 :components - MeshComponent, CameraComponent, LightComponent, ReflectionProbeComponent + managers
 :scene_system - RenderSceneSystem (per-scene SceneSystem; the parallel to Sedulous's
 RenderSceneModule): holds the scene's ONE Environment (skybox, IBL source,
 ambient, fog) + per-scene render config; coordinates the providers
 :extract - providers: turn ComponentManagers + RenderSceneSystem → RenderData pushed in
 :subsystem - RenderSubsystem (runtime::Subsystem + scene::ISceneObserver): owns the renderer,
 injects the managers + the RenderSceneSystem, registers Renderers + Passes,
 drives extract→render
 deps: foundation.render, foundation.scene, foundation.runtime, foundation.materials, foundation.geometry

 Source-of-truth split: things that are MANY-per-scene are components (mesh, camera, light,
 reflection probe); things that are ONE-per-scene live on the RenderSceneSystem (environment
 / sky / IBL / ambient / fog). If multi-environment-per-scene ever becomes real it promotes to
 a component, but 1-per-scene is the norm.
```

Other renderable subsystems (a future `foundation.particles`, the existing `foundation.vg` for UI)
depend on `foundation.render` only for the extension points (`RenderData`/`RenderCategory`/
`Renderer`/`PipelinePass`) - never on the scene. They register with the `RenderSubsystem`.

---

## 5. Extraction model

The contract between "the world" and "the GPU". Ported model, rebuilt frame-level.

- **`RenderData`** - base for a unit of renderable work, allocated from a per-frame
 **frame arena** (bump allocator), trivially destructible, valid one frame. Subclasses:
 `MeshRenderData`, `LightRenderData`, `DecalRenderData`, `SpriteRenderData`,
 (later) `ParticleRenderData`. Carries a `RenderCategory` + a precomputed 64-bit sort key.
- **`RenderCategory`** - a `u16` tag (Opaque, Masked, Transparent, Sky, Decal, Light,
 ReflectionProbe, GUI, Particle, …). The dispatch key.
- **Providers** (`engine.render`) read ComponentManagers and write `RenderData`
 into the frame's `ExtractedRenderData`. The renderer core defines the boundary
 (`IRenderDataProvider`-equivalent) and never sees a component.
- **Sort** - LSD **radix sort** over `{u64 key, RenderData*}` pairs, O(N), keys computed
 *inline during extraction* (material/PSO id in the high bits for opaque state-change
 minimization; view-space depth in the low bits, front-to-back opaque / back-to-front
 transparent - the PlayCanvas sort-key idea, also what Sedulous does).

**Extraction is per-SCENE, once per frame (not per-view).** Each active scene extracts once
into an immutable `ExtractedScene` (world-space renderables + lights + environment). Every
view of that scene - N cameras, shadow views, probe faces - shares that one snapshot read-only
and does its own per-view cull+sort against it. So "same scene from 5 cameras" extracts once
and renders 5 views; different scenes get different `ExtractedScene`s. Read-only-during-render
is what makes the snapshot safe to share across views *and* across worker threads. (§9.)

**The rebuild - double-buffered, decoupled extraction.** Sedulous extracts synchronously
inside the render call (sim↔render lockstep). We hold **two** `ExtractedScene` sets per scene
and extract frame N while the GPU submits frame N-1, so simulation and rendering overlap.
Extraction is parallel *within and across* providers and scenes via the job system (per-worker
arenas, single-threaded merge - Sedulous does the within-provider part well; we extend it
across providers, scenes, and shadow views).

---

## 6. Extensibility: categories + Renderers + Passes (the particles model)

The single most important architecture to adopt - validated by Sedulous's particles being
a separate 5k-LOC library that contributes draws with **zero** renderer-core changes. A
renderable type adds four pluggable pieces:

1. a **`RenderData` subclass** tagged with a `RenderCategory` (arena-allocated);
2. an **extractor** that fills it during extraction;
3. a **`Renderer`** (per-category drawer) registered with the `RenderSubsystem`
 (`GetSupportedCategories() → [Particle]`, records the draws for its category);
4. optionally a **`PipelinePass`** added to the pipeline to schedule it in the graph.

The core `Pipeline` sorts `RenderData` by category and dispatches each to its registered
`Renderer`. Meshes, sprites, decals, particles, and world-space UI all extend the renderer
identically, from outside. **This is the keeper architecture regardless of how much shading
code we port.** Our current flat `ExtractedView`/`Renderable` is the slice's simplification;
it gets replaced by `ExtractedRenderData` + categories.

### 6.1 Category definition: closed constants now, open registry later

`RenderCategory` is a `u16`, but the **set of categories is currently closed** - a static
list of named constants (`RenderCategories::Opaque … Particle`, `Count`-sized), ported
verbatim from Sedulous's `RenderCategories` static class. Sorting is likewise a central
`GetSortFunc`-style switch on the value. Adding a category means editing the core list + the
sort switch; a plugin **cannot** introduce one. This is deliberate for now: it's simpler, and
the ported passes + `Count`-sized arrays already assume a compile-time-constant, dense set.

The open alternative is **ezEngine's model**: categories are *registered* at runtime - 
`RegisterCategory(name, sortKeyFunc) → Category` into a global table, looked up by hashed
name, with the **sort function stored per-category** (no central switch). Built-ins are just
pre-registered entries; plugins add their own without touching core. The cost is indirection:
the category value is no longer a compile-time constant (no `switch`, no static array sizing
by `Count`, name-hash lookups).

**Decision: keep the closed model** until a third-party renderer needs to introduce a
category without editing core. That's the trigger to migrate to the registry; the four
pluggable pieces above (RenderData/extractor/Renderer/Pass) already point that direction, so
the move is additive - replace the constant block + sort switch with a registry, leave the
extension points unchanged.

---

## 7. Frame loop & concurrency

**Threading model: a stackless work-stealing task/job system on a fixed worker pool - 
NOT fibers. (Locked: WASM is a target.)** Fibers are off the table because stackful switching
on Emscripten/WASM needs Asyncify (heavy code bloat + slowdown) or the experimental Wasm
stack-switching/JSPI proposals (not production-universal) - even though desktop/mobile support
fibers fine (boost.context-style, not the deprecated `ucontext`). The task graph is also simply
the better fit: the renderer's parallelism is broad *data-parallel fan-out* (ParallelFor over
draws/components/shadow views) plus a few dependent stages, not the deep irregular
task-with-mid-task-waits graph fibers exist to serve. Normal threads work everywhere - WASM
via Web Workers + SharedArrayBuffer with three constraints we must honor in the design: a
**pre-sized worker pool**, the **main thread never blocks** (no indefinite joins/waits), and
cross-origin isolation. We evolve the existing `foundation.core` `JobSystem` (worker pool +
`WaitForAll`) into a work-stealing scheduler with `ParallelFor` + task dependencies. (Sedulous
went *fully* single-threaded for WASM, which is over-conservative - threads are fine on WASM;
only fibers and blocking-the-main-thread aren't.)

The rebuilt submission half (Sedulous's weakest area).

- **Frames-in-flight ring** (2-3): per-frame command pools, ring UBOs, deferred-destroy
 queues. (`foundation.runtime` GraphicsDevice already provides the frame ring + deferred
 destruction substrate.)
- **Double-buffered extraction** (§5): extract N ∥ submit N-1.
- **Parallel command recording** - passes that record many draws (forward opaque, shadows)
 split their draw list across worker threads recording into **per-thread encoders / secondary
 command buffers**, joined in pass order. Designed in from day one; the first cut may record
 single-threaded but the pass interface takes a thread-range. (Sedulous is 100% single-thread
 here - the hard scale wall.)
- **Deferred destruction** - generation-handle resources + a delay-N-frames free queue;
 bind groups destroyed at next BeginFrame, never while a command buffer references them.
- **No per-frame `WaitIdle`** on the hot path (only resize/shutdown). The slice already
 learned this (depth transition + shutdown WaitIdle).

---

## 8. GPU resource layer

Port Sedulous's patterns (the best part of it), on our RHI:

- **Ring/dynamic UBO allocators** - per-view (set 0) and per-object (set 2) data written into
 256-byte-aligned slots of a mapped `CpuToGpu` ring, bound with dynamic offsets. (Our slice's
 `ForwardRenderer` does a crude grow-the-UBO; this replaces it with a proper ring - the one
 thing the slice does worse than Sedulous.)
- **Pools** - a single large `BoneMatrixPool` (host-staging → device-local mirror copied once
 per frame), a skinned-vertex pool, mesh vertex/index sub-allocation. No per-mesh buffers.
- **`MeshGpuCache`** - keep (slice already has it), upgrade to sub-allocation from a pool.
- **PSO cache** - already ours (`foundation.materials.pso`, keyed by config × shader-variant ×
 RT-signature, version-poll hot reload). The renderer feeds it the assembled pipeline layout.
- **Descriptor lifetime** - persistent per-view/per-material bind groups updated in place where
 possible; avoid Sedulous's per-frame full frame-bind-group recreation.
- **Stable batch identity** - batches keyed by *stable IDs* (mesh resource id, material id),
 NOT pointer casts. Kills Sedulous's pointer-reuse hazard + the retry loop outright.

---

## 8.1 The depth convention: reverse-Z (2026-09-19)

ONE convention for every projection the engine builds and every depth it reads: the near plane
maps to NDC depth 1, the far plane to 0, a cleared (background) pixel reads 0, and the NEARER
surface is the LARGER value. Depth buffers are `Depth32Float`; with a float buffer this spends
the float's precision where a perspective divide starves it (far away), so the resolvable depth
step grows ~linearly with distance instead of quadratically (two surfaces 0.02 apart at 900
units are ~2500 ulps apart; under standard-Z they were a fraction of one ulp and z-fought).

It lives in three places, kept in step:
- `core::projection` (Float4x4.cppm): `kReverseZ`, `kNdcDepthNear`/`kNdcDepthFar`, `IsNearer`,
  `IsBackground`, `LinearizeDepth`, `DepthAtDistance`. `PerspectiveFovRH` and `OrthographicRH`
  build it; `BoundingFrustum::SetMatrix` extracts the near/far planes accordingly.
- `rhi::depth` (Foundation/RHI/Depth.cppm): `Nearer()` / `NearerOrEqual()` / `Farther()` are the
  compare functions (Greater / GreaterEqual / Less), `ClearValue()` the far plane,
  `BiasAwayFromViewer(units)` / `SlopeBiasAwayFromViewer(scale)` the rasterizer bias with its
  sign (a shadow caster is pushed away from the light = to a SMALLER depth). Every depth-stencil
  state, sampler compare and depth clear in the engine names one of these - `PipelineConfig`,
  the render-graph defaults, mesh/terrain/sprite/particle/debug/sky/MSAA passes, the shadow
  samplers, the samples.
- `Data/Shaders/depth.hlsli`: `kDepthNear`/`kDepthFar`, `IsBackgroundDepth`, `IsNearerDepth`,
  `FarthestDepth`/`NearerOf` (closest-in-neighborhood searches), `BiasTowardViewer` /
  `BiasClipTowardViewer` (shadow receiver bias, debug-line pull), `LinearizeDepth`. The sky
  sits AT `kDepthFar` with `NearerOrEqual`, so only a cleared pixel passes.

What is convention-free and stayed as it was: everything that reconstructs position through an
inverse projection (GTAO/SSAO/SSR/SSGI/decals via `InvProj`/`InvViewProj`), the soft-particle
depth from projection terms, the cluster slices (view-space), LOD (projection[1][1]), the editor
mouse ray (from the FOV). Rules: never a literal 0/1 depth, never a literal Less/Greater, never
a raw bias sign - name the helper, so the convention can be read in one place and audited by
grep. Proven at the pixel by `Render.Backend.Tests/ReverseZProbeTests` on Vulkan + WebGPU
(and the cooked WGSL path): nearer wins with the background untouched, the 0.02-at-900 face is
solid, and a sun's shadow reads lit beside the caster and dark behind it (cascade projection,
caster bias sign, sampler compare and receiver bias all agreeing). `MathTests` pins the matrix
and the frustum planes. Re-cook the shader pack after touching the shaders.

## 9. Views, scenes & the frame graph

The renderer renders a **set of views** per frame. A view is the unit of work: *render one
scene's extracted data, from one camera, into one target.* This single abstraction makes
multi-scene side-by-side (editor docked pages), multi-camera (split-screen / picture-in-picture
/ the same scene from N angles), and derived views (shadow cascades, probe faces) all the same
mechanism - and `RenderView` is the **isolation boundary** that guarantees views don't trash
each other. This is the explicit replacement for Sedulous's `Pipeline`/`ShadowPipeline`/
`ProbePipeline` god-objects.

**`RenderView`** (first-class data, pooled per frame from a `RenderViewPool`):
- `camera` (view+proj+frustum), `target` (an offscreen render target, or a backbuffer region),
 `scene` (→ the `ExtractedScene` it draws), `settings` (post config, layer mask, clear, viewport).
- per-view **transient** state, allocated from pools/rings *keyed by the view*, never shared:
 the culled+sorted draw list, the view-uniform ring slot (camera matrices), the **cluster grid**
 (clusters are view-space - per-view by construction), the shadow allocations, and the view's
 HDR/depth/normal/motion targets. **RenderView owns all per-frame-mutable render state.**

**`ExtractedScene`** (per-scene, once per frame, immutable - §5): world-space renderables +
lights + environment. Views of the same scene share it read-only. N cameras of one scene = one
extraction.

**The isolation rule (fixes Sedulous's multi-scene stomping class of bug by construction):**
the only state shared across views is (a) the immutable `ExtractedScene`(s) and (b) shared GPU
resources in `GpuResourceManager` (meshes/materials/textures - uploaded once, used everywhere).
*Everything* mutable-per-frame - culled lists, view uniforms, cluster grids, shadow atlas slots,
HDR/depth targets - is per-`RenderView`, from pools/rings. No mutable singleton is shared across
views. (Sedulous's bug was one cluster system shared across scenes; here clusters live on the
view.)

**One frame graph composes ALL views.** There is a single `foundation.rendergraph::RenderGraph` per
frame (a per-frame `Frame` object owns it). Each view - primary (cameras) and derived (shadow
cascades, probe faces) - contributes its **pass-group** into that one graph, targeting its own
resources. The graph schedules everything, inserts barriers, and **aliases transient targets
across views** (view A's HDR memory reused for view B if lifetimes don't overlap). There is **no
per-view driver object running its own mini-frame**: the "pipeline" is a **`PassList` template**
applied per view, the Shadow/Probe/Lighting **Systems contribute passes** for a view, and the
graph drives execution + barriers + aliasing. This is the decision that keeps the renderer from
re-growing the three-god-object shape.

Per-view pass-group (a `PassList` instance per view, each pass a graph node declaring its
targets/reads so barriers + aliasing are automatic):
```
ClusterBuild (compute, per-view) → [Shadow depth passes for this view's lights] →
DepthPrepass → [Decals] → ForwardOpaque (MRT: HDR, normals, motionvec) →
Sky → ForwardTransparent → Post (SSAO→Bloom→TAA→Tonemap→FXAA) → resolve to view target
```
MRT layout (color0 HDR, color1 packed normals, color2 motion vectors) follows Sedulous (feeds
SSAO/TAA). The graph owns the HDR/depth/normal/motion targets as transients with proper barriers
 - **no hand-rolled ping-pong** (the manual SceneDepth state tracking is exactly the seam we let
the graph own). Shadow/probe captures are pass-groups in the *same* graph, not separate frames.

**View sources, assembled per frame:**
- *Primary views* = `CameraComponent`s across all active scenes (each camera names its target)
 **plus** explicit registrations (an editor docked page registers `view = {scene, camera, page
 target}`).
- *Derived views* are spawned by Systems for a primary view: `ShadowSystem` spawns shadow
 cascade/atlas views for the primary view's lights; `ProbeSystem` spawns probe-capture views
 (amortized - a few faces/frame). All are `RenderView`s over the same `ExtractedScene`, into
 different targets (shadow atlas, probe cubemap).

**Targets & presentation - decoupled.** Views render into **offscreen render targets**; window
presentation is a separate composite step: a window samples/blits the offscreen targets it
shows (editor: each docked panel samples its view's texture; game: the single view blits to the
backbuffer). A single-view-to-backbuffer **fast path** skips the offscreen RT. This is what makes
editor side-by-side, split-screen, and PiP fall out of one model, and it cleanly separates
"render N views" (one graph) from "present M windows" (the runtime's per-window job).

**View-independent shadow sharing (optimization, later):** point/spot shadow maps are light-space
(view-independent), so two views of the *same scene* can share them keyed by (scene, light);
directional CSM cascades fit the camera frustum and stay per-view. v1 is per-view-correct; sharing
view-independent shadows is a later optimization, never a correctness requirement.

---

### 9.x GPU picking (2026-09-21)

Which entity is under a pixel (or inside a rect) of a view, answered by the GPU - `PickSystem`
(`foundation.render:picking`), owned by the RenderSubsystem, driven by the RenderFrame.

- **Request / poll, keyed by viewport.** `RenderSubsystem::RequestPick(viewportKey, x, y, w, h)`
  returns an id; `TryTakePickResult(id, result)` answers a few frames later with unique
  `PickHit{entityIndex, generation}` hits (an exact `EntityHandle`, not just an index). The key is
  the same opaque `viewportKey` the `RenderScene` call carries (`ViewSettings::viewportKey`), so
  only that viewport's view answers - split-screen views, the camera-preview inset and thumbnails
  never do. A request whose view does not render within `kExpireFrames` answers `rendered=false`;
  `CancelPicks(key)` when a viewport closes.
- **An on-request pass, rect-sized.** A view with pending requests re-emits its draw list through
  each renderer's `ResolvePickIds` (the depth-only caster path with an id-writing fragment) into an
  `RG32Uint` target the size of the rect, with its own depth (nearest wins). The camera projection
  is CROPPED so the rect fills the target (`CropProjectionToRect`): a click renders a 1x1 target,
  vertex work only, and the terrain's chunk cull runs against the crop. Declared before the TAA
  jitter mutates the camera (a 1x1 crop must see the unjittered pixel grid). x = entity index + 1
  (0 = the clear = nothing), y = generation.
- **Readback without a stall.** A graph copy pass copies the target into a `GpuToCpu` buffer
  (256-byte rows, the WebGPU/D3D12 pitch); the slot retires when the device ring has LEFT and
  RETURNED to the submitting frame index (the ThumbnailStage rule), then maps and decodes. Never
  WaitIdle.
- **Ids per draw kind.** Single objects: the `Object` cbuffer's two pad words (`PickIndex`,
  `PickGeneration`). Instanced runs: the instance-stepped `DataOffsets.zw` - the pick pass fills
  its own ramp, never the forward's. MultiMesh sets: one entity for all instances, so the id rides
  a per-set `PickView` slot (the shadow-view ring; `kMaxPickMultiMeshSets` per pass). Terrain: a
  `PickView` prefix in its view slot. Masked materials keep their cutout; a two-sided material
  picks both faces (cull from the material). Sprites/particles do not implement it (the editor's
  CPU origin pick covers their entities).
- **Producers tag, the readback decodes.** `RenderData::entityId` (base, every producer) holds
  the one `EntityTag` layout (index low, generation high); `engine::render::PackEntity` and the
  terrain extract write it.
- **Rings.** `Renderer::SetPickPasses(n)` before `PrepareFrame` sizes the object/instance/offset
  rings for the extra re-emits, and the shadow-view ring for `n * (1 + kMaxPickMultiMeshSets)`
  extra slots; that ring's bind range is the PickView size (80) so WebGPU's declared-size check
  passes for both layouts.
- **Shaders.** `pick_ids.vs/ps` (variants SKINNED INSTANCED ALPHA_TEST / ALPHA_TEST),
  `terrain_pick.vs/ps`; in the cooked pack.
- **Proof.** `Render.Tests` (tag, clamp, crop, decode, the request state machine on the Null
  device), `Render.Backend.Tests` PickProbe (near over far by index and generation, three
  instances answering distinctly, a corner answering nothing, a whole-view rect answering every
  entity once, two requests one frame; Vulkan + WebGPU + the cooked WGSL path),
  `Engine.Render.Tests` (subsystem API), `Editor.Scene.Tests` (the select tool's async pick).
- **Editor.** The select tool asks the GPU for the pointer pixel on click and applies the answer
  when it lands; the CPU origin pick is the fallback for a GPU miss (lights, cameras, empties).
  A drag is a marquee: one rect request, every entity drawn inside it (editor.md 3.6).
- Legacy Sedulous `PickPass.bf` rendered every mesh at full resolution into RGBA8 and copied one
  pixel after a fixed two-frame wait; this keeps its shape and drops the full-screen cost, the
  8-bit ids and the fixed wait.

### 9.y Terrain vegetation (2026-09-21)

Grass and props over a terrain are per-chunk INSTANCED SETS through the MeshRenderer's
MultiMesh path - no renderer of their own. Spec: `Documentation/Specs/terrain-vegetation.md`.

- **Data.** `TerrainVegetationComponent` (Engine.Vegetation) on the terrain entity (or a child
  of it; the manager walks the ancestry for the `TerrainComponent`): TWO lists since
  2026-09-23 (user: "the same component, but with multiple lists"). `proceduralLayers`
  (`ProceduralVegetationLayer`): placement Uniform / Splat / Mask / SplatTimesMask, splat layer
  + threshold, mask plane, density. `propLayers` (`PropVegetationLayer`): the authored
  `instances` the Paint Props brush places. Both over `VegetationLayerBase`: name, mesh, its
  `materials` (one per mesh slot, as a mesh component's: one covers the whole mesh, more route
  each submesh to its slot's through the MultiMesh path's `submeshMaterials`; none = the
  default), scale, slope and height rules, alignToNormal, fade start / end,
  castShadows, maxInstancesPerChunk, visible. A new procedural layer defaults to Mask (it grows
  nothing until the component's mask has paint on its plane; the 2026-09-22 "no unasked-for
  scatter" ruling kept); a new prop layer holds nothing. The inspector edits each layer in its
  own expander under its list (editor.md 3.5). Wire: data version 3 (2026-10-05: a material
  per slot; a version-2 layer's one optional `material` reads as the list's one entry), with a
  legacy reader for the one-list version 1 (`TypeBuilder::ReadsDataVersionsFrom(1)`: `placement` 3 = Scattered
  becomes a prop layer, the rest procedural; the retired enumerator's value is not reused). The
  manager keys its caches and chunk seeds by SLOT (the list index, the prop list flagged in the
  top bit, `kPropSlotBit`), so the two lists never collide on a persistent-buffer key.
- **Scatter.** `foundation.vegetation::ScatterChunk` is a pure function of (seed, chunk,
  heightfield, splat, layer): the seed hashes the owner entity's persistent id, the layer index
  and the chunk index, so the same inputs give byte-identical terrain-local instances on any machine. The
  output ORDER is a uniformly random sequence, so distance fade is a draw-count PREFIX
  (`DensityAtDistance` x count via `FadePrefix`): the GPU buffer holds the full chunk once,
  only `instanceCount` moves with distance, no re-upload, no per-instance culling, no chunk
  pop. `maxInstancesPerChunk` (4096 x 144 B = 590 KB per set) bounds memory: it counts PLACED
  instances (the loop stops when the chunk is full, and warns once), never the candidates, so
  a mask patch covering a slice of a chunk still grows at the layer's density (2026-09-23; the
  candidate count keeps its own CPU ceiling, `kMaxCandidatesPerChunk`).
- **Extraction.** `VegetationLayerComponentManager` (an `IRenderDataProvider`, registered per
  scene by `VegetationSubsystem`) emits ONE `MultiMeshRenderData` per (layer, chunk) within
  `fadeEnd` of the snapshot's first-view origin (`ExtractedScene::ViewOrigin`, set by
  RenderSubsystem before the providers run; a headless extraction thins nothing): `key` = the
  chunk seed (the renderer's persistent-buffer slot), `transforms` borrowed from the cache for
  the frame, `instanceCount` = the fade prefix, `uploadCount` = the whole set (uploaded once;
  the prefix moves with the camera without a re-upload), `version` = the scatter build
  (re-upload only on change), `castShadows` and the `fadeStart` / `fadeEnd` window from the
  layer. The renderer keeps a per-region (frame-in-flight) version + written count and rewrites
  a region only when it is behind the item, so a region never draws a tail it was not written
  with.
- **Per-instance fade (2026-09-22).** The prefix alone faded a whole chunk at one density (the
  chunk's nearest distance), so neighbouring chunks met at a straight seam and a chunk's last
  instances vanished as a block. The prefix stays the coarse bound; the dissolve itself is per
  instance in the vertex shaders (`instance_fade.hlsli`, forward / shadow-depth / pick under
  INSTANCED): the renderer writes each instance's RANK ((i + 0.5) / uploadCount, its place in
  the set's random order) into its `Tint.a` at upload when the item carries a window, and the
  shader collapses an instance to its origin (zero area) when its rank is above the density at
  ITS OWN camera distance, shrinking over the last eighth of the window first. The window rides
  the pass's view block through a PRIVATE slot per faded set (`ShadowParams.zw` on the forward
  view; `ShadowWind.yz` + `ShadowCamera` on the 96-byte shadow view; `FadeStart` + `PickCamera`
  on the 96-byte pick view), budgeted by `kMaxFadedSetSlots` (a set past it draws unfaded, never
  dropped). Non-faded sets and every other draw are untouched (window 0). Probed at the texel
  on Vulkan + WebGPU (`InstanceFadeProbeTests`). Sets are built on demand under a per-extraction budget (4 chunks, `SetBuildBudget`),
  invalidated by the heightfield and splat uid + version, the entity world matrix (recompose,
  no rescatter) and the layer's scatter hash; `InvalidateRegion` (the editor brushes, P1)
  regrows only the touched chunks. An invalidated set that was already built keeps drawing
  its previous instances until its rebuild's turn comes (a dropped frame under a brush is a
  flicker); only a never-built set waits unseen. A prop layer re-buckets its authored
  instances outside the budget (a copy, not a scatter), so a prop stroke lands whole. Frustum visibility is NOT known at extraction (one snapshot
  per scene); distance gates the build, the renderer culls the sets per view.
- **Renderer prerequisites (this phase).** `RenderData::castShadows` (default true) gates the
  sun-cascade caster list, so grass casts nothing unless asked; `MeshRenderer` evicts a
  MultiMesh set unseen for `kMultiMeshEvictFrames` (120) through the retire queue (before
  this nothing was ever freed); a set's per-frame region capacity is rounded to 16 instances
  (`MultiMeshRegionCapacity`) so region offsets meet every backend's storage-buffer offset
  alignment (an odd count - 941 tufts - bound region 1 at an unaligned byte and WebGPU refused
  the bind group; Vulkan had accepted it).
- **Tests.** `Vegetation.Tests` (seed determinism, density -> count and the cap, the splat /
  slope / height rules, normal alignment, bounds growth, fade math, `ChunksTouchedBy`),
  `Engine.Vegetation.Tests` (reflection + wire, one set per painted chunk with the fade prefix,
  out-of-range absent, region-scoped regrow, the build budget, nothing without a layer),
  `Render.Tests` (eviction, castShadows gate, region alignment),
  `Engine.Vegetation.Backend.Tests` (a splat-driven grass layer draws on the painted half only
  and thins with distance; Vulkan + WebGPU). `TerrainPlayground` grows a grass layer over the
  x < 0 half of its dome with HUD density / fade / shadow controls.
- **Painted mask (P1, 2026-09-22).** `VegetationMask` (Foundation/Vegetation.Resource): u8
  density planes over the terrain footprint, one plane per layer that chooses Mask placement
  (`SplatTimesMask` multiplies the splat share in, so erasing the mask carves a road through
  splat grass). The component references it (`Ref<VegetationMask>`); the scatter samples the
  plane like the splat. Cooked as `VegetationMaskAsset` -> metadata + one "densities" sidecar
  (Pipeline/Vegetation.Pipeline; a PNG imports as one plane per channel). The Paint Vegetation
  brush (Editor/Editor.Vegetation, tool id `vegetation.paint`) paints, erases and smooths a
  plane with the splat brush's stroke model, one command per stroke, the source written back on
  Save; every stamp hands the manager the footprint rect it touched
  (`InvalidateFootprint`), so only those chunks regrow while painting.
- **Wind (P1, 2026-09-22).** `ShaderFlags::Wind` (`WIND`, declared in the forward, shadow-depth
  and pick vertex shaders' `variants:` lines, so the cooked pack carries the lattice): a
  world-space sway in `wind.hlsli`, `sin(time * WindSpeed + hash(world XZ)) * WindStrength *
  mask^2` with `mask = saturate(localY / WindHeight)` (roots at local y <= 0 stay, tips move),
  applied to the current and the previous-frame position so motion vectors follow it. The three
  parameters are MATERIAL properties in the forward set-2 cbuffer's spare lanes (offsets 24, 28
  and 60 - the block stays 64 bytes, `CreatePBR` declares them at 0 / 0 / 1), so a material
  without them reads zeros; the renderer selects the variant only when a material's
  `WindStrength` default is above zero (`MeshRenderer::MaterialWantsWind`), binding set 2 to the
  depth and pick passes for it as it does for masked casters, so every other material is
  byte-identical to before. Time: the SCENE's clock - `EnvironmentSystem` accumulates the
  scene's own dt (the context, group and scene time scales the scene manager composes) and
  only while the scene simulates (`Scene::SimulationEnabled`: the editing scene ticks every
  frame with simulation off, Simulate turns it on), so a paused or slowed scene's grass pauses
  or slows with it, the frozen editing scene's stands still, and Simulate sways it; the environment extraction stamps it on the snapshot
  (`ExtractedScene::SetTime`), and every record context reads its view's (or its casters')
  scene clock, falling back to `RenderFrame::SetTime` (the app's run clock via
  `RenderSubsystem::SetTime`) only for a snapshot no scene stamped (the probes). It rides
  `IblParams.zw` (this and last frame's seconds), `ShadowView.ShadowWind.x` (the ShadowView
  cbuffer grew to 80 bytes) and `PickView.WindTime`.
  `Render.Backend.Tests` WindProbe: a segmented card's tips shift between two frame times while
  its root rows stay, and a still material renders byte-identical; Vulkan + WebGPU.
- **Props (P2, 2026-09-22).** A layer with Scattered placement draws its authored
  `instances` (terrain-local matrices on the layer, serialized with it) through the same
  per-chunk sets: each instance maps to one chunk by its XZ, and a content hash of the array
  re-buckets the layer on any change (a stroke, an undo). The Paint Props brush (Editor/
  Editor.Vegetation, tool id `vegetation.scatter`) stamps `foundation.vegetation::ScatterStamp`
  (density x disc area candidates under the layer's slope / height / scale / alignment rules,
  a spacing rule against existing props, and a collision query: the scene's physics world's
  `ShapeOverlap` with the terrain's own body excluded when a world exists) or erases the props
  under the disc, one command per stroke; stamps seed from the stroke so a scripted stroke is
  deterministic. Both vegetation brushes share one footprint pick (`editor.vegetation:pick`).
- **Deferred.** Impostors and GPU-driven scatter, per-view fade prefixes (P3).

## 10. Materials & shaders

- **Data-driven** - `MaterialSystem` infers the set-2 bind group from declared properties
 (already built); the renderer binds it opaquely. `PipelineConfig` drives render state;
 `ShaderFlags` drive compile-time variants via DXC; the PSO cache keys on both.
- **Front-end/back-end shader split** (PlayCanvas idea, serves the "custom materials without
 renderer changes" decision) - surface chunks *produce* `albedo/normal/roughness/metallic`;
 shared lighting chunks *consume* them. Custom materials swap the front end, reuse the
 clustered-lighting + IBL + shadow back end. Realized as shared `.hlsli` includes + the
 variant system, not a runtime string preprocessor (ours is compile-time, strictly better
 than PlayCanvas's runtime assembly).
- **Forward shader** - port Sedulous's `forward.frag.hlsl` (the assessed-correct PBR loop) as
 the standard surface→lighting path; adapt bindings to our set convention + TEXCOORDn vertex
 inputs (slice learned this) + `row_major` matrices (slice learned this).
- **A mesh's own material properties (2026-10-08).** A material's properties are shared by every
 mesh that uses it (the mesh renderer keeps one `MaterialInstance` per material). A mesh can set
 some for itself alone: `MeshComponent::SetMaterialProperty(slot, name, value, size)` (scripts:
 `SceneRender.setMaterialFloat` / `setMaterialFloat4` / `clearMaterialProperty`), runtime state
 that is not saved. Extraction lends them to the draw (`MeshRenderData::overrides`, with a version
 that changes with them). Such a mesh never batches; drawn alone, each slot it overrides gets an
 instance of its own, kept per (entity, slot), its values reset to the material's and the overrides
 applied again whenever the version changes, and released a few frames after no draw uses it.
 Values are as the material authors them (a colour sRGB, an HDR colour's intensity in w), so the
 one encode to the GPU stays where it is. Depth and shadow passes draw with the shared material.
 `MaterialInstance` refuses a value wider than its property.

---

## 11. Lighting & shadows

**Clustered, designed for MANY lights (port the math, scale the data path).** Port the
compute cluster build (16×16 tiles, log-Z slices, view-space AABB, GPU sphere-cull) and the
forward light loop, but design the data path for *many many lights* from the start:
- **Storage buffers throughout** (lights, per-cluster index lists) - drop the WebGL
 float-texture packing entirely; 32-bit light indices, per-cluster `(offset, count)` into a
 *compacted* global index list (not Sedulous's fixed 32-slot-per-cluster reservation).
- **Two-phase GPU culling** so cost isn't O(clusters × allLights): a coarse pass culls the
 global light set to the view frustum (and bins by depth) → only candidate lights reach the
 per-cluster assignment. This is where the GPU-driven seam (§7) pays off - light culling is a
 compute job, not a per-cluster loop over every light.
- Parameterized per-cluster cap with real overflow handling (priority/spill, never silent
 truncation); delete the dead `CLUSTER_MAX_LIGHTS`.

**Shadows (port math, rebuild scheduler + atlas).**
- Port: CSM practical-split + sphere-fit cascades, smooth cascade blend, cube-face point
 shadows with anti-seam FOV, the bias scheme. **Add light-space texel snapping** (the missing
 shimmer fix).
- Rebuild the **atlas** (importance/screen-size-driven slot sizing, LRU, **graceful fallback**
 instead of silent shadow disappearance) and the **scheduler** (static/dynamic split with
 **static-shadow caching** - render static casters once, re-render only dynamic - instead of
 Sedulous's full re-render-every-frame). This is the biggest perf win available.
- Filtering: start with PCF (port), seam in PCSS/contact-hardening later.

---

## 12. IBL, probes, post, skinning

- **IBL** - port split-sum (GGX prefilter, irradiance convolution, pre-baked BRDF LUT). Bump
 the conservative 256² env later; add roughness-aware specular occlusion as a refinement.
- **Reflection probes** - **rebuild**: parallax-corrected box/sphere projection + blend weights
 (Sedulous's `InfluenceRadius` hook exists but is unused), instead of nearest-probe hard-swap.
- **Post** - port the stack (TAA, SSAO, Bloom, FXAA) shaders; they're assessed correct. Adopt
 the refinements from the start since we're doing the most-correct thing: **YCoCg TAA
 neighborhood clamp** (better chroma-edge stability than RGB) and **blue-noise SSAO** (no
 hash banding).
- **Color management & tonemapping - the most-correct path, not ACES-approx.** Strict linear
 working space throughout (HDR render targets), exposure before tonemap, and a real filmic
 operator: **AgX** (Sobotka) as the default - it preserves hue and handles bright saturated
 colors without the hue-shift/desaturation artifacts of the Narkowicz ACES fit Sedulous uses;
 proper OETF for the display (sRGB now, with a clean seam for Display-P3 / HDR10 scRGB output
 later). Tonemap stays the last *color* step before FXAA's LDR luma pass. (This upgrades the
 one place the assessment flagged Sedulous as "the cheap approximation.")
- **Skinning** - port the GPU-skinning + pooled-bone design (incl. prev-frame bones for motion
 vectors). `foundation.geometry`'s `SkinnedMesh` (static stream + parallel skin stream) feeds it.

---

## 13. Phasing (each milestone shippable + tested on Null RHI, verified on Vulkan via Sandbox)

**All phases below are DELIVERED** (0-9, plus SSR and view-frustum culling added after). The
one item explicitly deferred - the spatial acceleration structure - moved to
[renderer-improvements-history.md](../Archive/renderer-improvements-history.md); culling is still linear scans today.
View-frustum culling is ON by default since 2026-09-24 (`RenderSubsystem` / `RenderFrame`
`m_viewCulling`; it shipped off and only the samples ever switched it on, so the editor and the
player drew everything). Per view, `ViewPostOverride::disableCulling` (the editor's Post menu:
"No frustum culling (draw everything)") clears `ViewSettings::frustumCull` for an A/B; the frame
gates by both. Occlusion culling does not exist (backlog: Hi-Z from the depth prepass).

0. **(done)** Thin slice: extraction boundary + forward draw of a cube.
1. **Extraction + view core**: replace `ExtractedView`/`Renderable` with per-scene
 `ExtractedScene` + `RenderData`/`RenderCategory` + radix sort + provider boundary + the
 `Renderer`/`Pass` registry, **and `RenderView` + the per-frame `Frame`/one-graph composition**
 (even the single camera is "one view" - the isolation boundary exists from day one). Mesh
 path, single-view→backbuffer fast path. (Establishes the keeper architecture + view isolation.)
2. **Resource + submission rebuild**: ring UBOs, mesh/bone pools, stable-id batching +
 instancing, frames-in-flight, double-buffered extraction, parallel-record seam.
3. **Pipeline + RenderGraph passes**: depth prepass + forward opaque/transparent + MRT via the
 graph (automatic barriers). Material set-2 binding (data-driven materials drive shading).
4. **Lighting**: clustered compute + light buffer + the ported forward PBR loop.
5. **Shadows**: CSM (with texel snap) + the rebuilt atlas/scheduler (static caching).
6. **IBL + sky**, then **post** (TAA/SSAO/bloom/tonemap/FXAA), then **skinning**. **(done)**
 - **spatial acceleration structure** (BVH/octree over `MeshRenderData` world bounds) - DEFERRED,
 moved to [renderer-improvements-history.md](../Archive/renderer-improvements-history.md). Culling is linear scans today; the
 world center+radius on `MeshRenderData` are the ready-made index, and it drops in behind the
 existing `BuildDrawList` / `BuildShadowCasterList` cull seam when a profile demands it.
7. **Reflection probes (rebuilt)**, **decals**, **debug draw**. **(done)**
8. **Multi-view / multi-scene**: offscreen view targets + window composite, the view-list
 assembled from cameras across active scenes + explicit (editor) registrations. Proves
 side-by-side scenes and same-scene-multiple-cameras with no cross-view trashing. (The
 `RenderView` isolation built in phase 1 makes this additive, not a rearchitecture.)
9. **Extension proof**: a second renderable subsystem (sprites or particles) plugging in with no
 core change.

---

## 14. Decisions (resolved) + remaining open question

1. **GPU-driven path** - RESOLVED: **design the seam in now, build later.** Batches produce
 indirect-draw args; a compute cull job is droppable-in; the submission interface assumes it.
2. **Parallel command recording** - RESOLVED: **design in now.** Pass/record interfaces take a
 thread-range from day one; the first implementation may be single-threaded but the seam is
 load-bearing in the API.
3. **Port fidelity** - RESOLVED: the §3 keep/rebuild table stands (port shaders near-verbatim;
 rebuild shadow scheduler + probes + batch cache; keep our graph/materials/variants).
4. **Color/tonemapping** - RESOLVED: **most-correct path** - strict linear workspace, **AgX**
 tonemapper (hue-preserving, vs the ACES Narkowicz fit), YCoCg TAA clamp, blue-noise SSAO,
 clean seam for Display-P3 / HDR output. (See §12.)
5. **Lighting scale** - RESOLVED: **many many lights** - storage-buffer light data, compacted
 per-cluster index lists, two-phase GPU light culling (not O(clusters × allLights)). (See §11.)
6. **Transparency** - RESOLVED: sorted forward-blended v1; OIT later.
7. **Environment** - RESOLVED: **NOT a component** - it is one-per-scene, so it lives on the
 per-scene **RenderSceneSystem** (the Sedulous `RenderSceneModule` parallel). Many-per-scene
 render inputs stay components: `MeshComponent`, `CameraComponent`, `LightComponent`,
 `ReflectionProbeComponent`. (See §4.)

**WASM is a target (resolved).** Concurrency = stackless task graph, no fibers (§7). Sequencing:
the renderer is built **native-first**, then Emscripten bring-up proceeds **from core upward**
once the renderer lands. That makes WASM/WebGPU portability a *design constraint now* (§16) so
the bring-up is a port, not a rewrite.

---

## 16. WASM / WebGPU portability constraints (design-now, build-native-first)

The renderer is built and validated on Vulkan first, but WASM/WebGPU is a confirmed target
with bring-up to follow (core-upward). To keep that a *port*, the renderer must stay inside the
WebGPU-mappable subset from day one:

- **RHI feature subset.** Use only what WebGPU exposes: bind groups, render/compute pipelines,
 render passes (our dynamic-rendering usage maps to WebGPU passes), **dynamic-offset uniform
 buffers**, **storage buffers**, **compute**, **indirect draw**. Avoid geometry/tessellation
 shaders, and any bindless beyond WebGPU binding-array limits, in the core path. (Our RHI is
 already WebGPU-shaped, so this is mostly "don't reach for Vulkan-only extensions.")
- **Bind-group budget ≤ 4.** WebGPU guarantees only 4 bind groups. The set-frequency convention
 (view / material / object / pass) must fit in 4 - design the frequencies up front. (Sedulous's
 set0-frame / set2-material / set3-object fits; keep within that envelope.)
- **Shaders must cross-compile.** Author HLSL → SPIR-V via DXC for native; the same shaders must
 be WGSL-translatable (SPIR-V→WGSL via Tint, or a WGSL emit path the shader system gains at
 bring-up). Stay in the HLSL subset that cross-compiles - no constructs without a WGSL mapping.
- **Concurrency (§7):** pre-sized worker pool, main thread never blocks, no mid-frame thread
 spawn. The task graph is built to these from the start.
- **No synchronous GPU readback on the main thread** - picking/screenshot/occlusion results go
 through async readback (frames-in-flight latency), never a stall.
- **wasm32 address space** - 4 GB cap, 32-bit pointers by default; the large pools (bone/vertex)
 are fine but keep allocations bounded and avoid pointer-size assumptions.

These are cheap to honor while building native and ruinous to retrofit - hence design-now.

## 15. Bottom line

Not a wholesale port, not from-scratch. **Lift the assessed-correct, hard-to-get-right parts
(shading/IBL/post shaders, clustered compute, CSM math, resource-pool patterns) and the
extensibility model (category + RenderData + Renderer + Pass); rebuild the scale/concurrency
layer (frame loop, batching, shadow scheduler, probes) modern; keep our scene-agnostic
extraction boundary (cleaner than Sedulous's) and our already-superior compile-time materials
+ shader variants + PSO cache + RenderGraph.** The result is Sedulous's correctness without its
ceiling.
