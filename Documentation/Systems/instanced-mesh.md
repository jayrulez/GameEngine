# Instanced Mesh (MultiMesh) Primitive

> Status: CURRENT
> Verified: 2026-08-12 @ 7e4ebae7
>
> Since 2026-10-29 (shader-blocks-cleanup phase 3) the per-instance stream called `DataOffsets`
> below is `drawOffsets` in the forward and depth passes (`render::GpuDrawOffsets`: instance index,
> bone base, last frame's bone base, fade) and `pickOffsets` in the pick pass
> (`render::GpuPickDrawOffsets`: instance index, bone base, entity generation, entity index + 1).
> Track: [[instanced-mesh]] / [[renderer-instancing]]

IMPLEMENTED - static (SS1-6) AND skinned crowds (SS7) shipped.
Static: `InstancedMeshComponent` (`engine.render`) + a persistent per-set instance buffer + a shared
DataOffsets ramp; the whole grid draws as one instanced set across depth/forward/all CSM cascades with
no per-frame fill. A/B in RenderStressTest with **M** - 120k spheres shadows-off went 47->60 fps
(CPU-bound -> GPU-bound), render CPU 20ms->1.3ms.
Skinned (SS7): `InstancedSkinning` companion (`engine.animation`) computes M SHARED pose
palettes/frame (phase-bucketed, instance i uses pose i%M) → shared bone pool → per-set DataOffsets
(bone bases) → skinned MultiMesh draw. Per-bone motion blur (ping-pong prev pool → `DataOffsets.z`).
AnimatedCrowd sample: **2350 chars @60fps shadows-on / ~5000 shadows-off**, GPU-bound, CPU ~2ms
(vs ~1683 CPU-bound per-entity). Both InstanceData + offsets buffers are N-buffered (frames-in-flight
hazard-free, static OR per-frame-dynamic). Validated clean under the Vulkan validation layer.

Relationship to Flax's "batch skinned meshes into one draw" (global bone buffer + per-instance bone
range): that = the EXISTING per-entity skinned instancing (m_boneDevice + DataOffsets.y + ResolveInstanced
batching), O(N) unique poses. §7 goes further for crowds - O(M) SHARED poses + O(1) ECS. Per-clip variety
= app-level bucketing (one InstancedSkinning per clip); cost stays O(clips×M).

Motivated by a direct benchmark against
Godot 4.7 (below). The renderer's GPU-side instancing path already has the right shape
(`StructuredBuffer<InstanceData>` + per-instance `DataOffsets`); this primitive makes that
buffer *persistent* instead of ring-allocated per frame, collapsing static instanced content
from O(N)/frame CPU to O(1)/frame.

Grounded in: [[renderer-design]], [[renderer-instancing]], the instance-sharing work
(f63ea62), the shadow-caster decoupling ([[view-frustum-culling]]), and the Godot
comparison below.

---

## 1. Motivation - the Godot benchmark

RenderStressTest (grid of instanced spheres, one shared mesh+material, one directional CSM
light, everything framed) was reproduced in Godot 4.7 (`p2` project: `main.tscn` MultiMesh,
`main_nodes.tscn` per-node). Measured on the **same RTX 2060**, shadows on:

| shadows ON, RTX 2060 | count | frame | **ms / 100k** |
|---|--:|--:|--:|
| Godot MultiMesh | 184k | 37.04ms | **20.1** |
| Godot MeshInstance3D (per-node, auto-instanced) | 96k | 18.18ms | **18.9** |
| **Draconic** (per-entity) | 144k | 40.94ms | **28.4** |

Draconic is ~40% slower per sphere with shadows on. The profile shows **why**, and it is not
the GPU:

- Draconic is **CPU-bound**: render-thread 40ms vs GPU 29.6ms. The GPU is competitive -
 extrapolating Godot's per-node path to 144k gives ~27ms vs Draconic's 29.6ms GPU.
- Godot's MultiMesh (O(1) CPU) and per-node (O(N) CPU) run at basically the same speed
 → Godot is **GPU-bound in both** - its per-frame CPU is cheap enough to hide behind the GPU.
- Draconic's per-frame CPU cost at 144k: Extract 4.4ms + AddView/sort 6.47ms +
 `shadow.resolve` 8.96ms (per-cascade instance fill) + depth-prepass record 6.14ms +
 forward record 2.15ms. All O(N), some done up to 5× per frame (camera + 4 cascades).

**Key finding:** Draconic's GPU is fine; the gap is per-frame CPU exceeding GPU time. Two
ways to close it: (a) make the per-frame CPU pipeline cheaper (parallelize sort, stop
rebuilding shadow instance data per cascade), or (b) give static instanced content a path
where per-frame CPU is O(1) - *this document*. They are complementary; (b) is the bigger win
for static crowds and is what Godot's MultiMesh does.

Godot's mechanism (verified in `/home/robert/Dev/CPP/godot`): a MultiMesh is ONE scene
object with ONE merged AABB, culled once per view/cascade, drawn as ONE instanced draw, with
per-instance transforms in a **persistent GPU storage buffer uploaded only on change**
(dirty-region tracking, `mesh_storage.cpp:1822/2289`). The SAME buffer is bound for the
camera pass and all 4 shadow splits - never rebuilt (`render_forward_clustered.cpp:4463`).
Godot does **not** cache directional shadows (all splits re-render every frame,
`renderer_scene_cull.cpp:3619-3621`); the win is purely O(1) per-frame CPU per object.

**Bonus edge:** Godot's MultiMesh has no per-instance skeleton (crowds need Vertex Animation
Textures). Draconic's instanced path already carries a per-instance bone base
(`DataOffsets.y`), so a skinned variant is a near-free extension - a real feature edge.

---

## 2. Goal & non-goals

**Goal.** A first-class instanced-mesh primitive whose per-frame CPU cost is O(1) in the
instance count for static content: N per-instance transforms live in a persistent GPU
buffer, the set is culled as one AABB, and it is drawn once per pass with all passes
(depth, forward, every shadow cascade) reading the same buffer. Godot-parity for static
shadowed crowds, while the existing per-entity path stays for genuinely dynamic scenes.

**Non-goals (v1).** Per-instance frustum culling (the set culls as one AABB - GPU clips the
rest, same as Godot); GPU-driven culling/compaction; skinned crowds (designed for, built
later - §7 - now shipped); LOD selection per instance. The ones that remain deferred are now
tracked in **[renderer-improvements-history.md](../Archive/renderer-improvements-history.md) §2** ("Remaining MultiMesh
optimizations") and §3/§4 (GPU-driven, spatial accel).

---

## 3. Principle - reuse the seams, add one buffer

The renderer already instances: `MeshRenderData` → `ResolveInstanced` fills a per-frame
`m_instanceRing` range with `InstanceData{world, prevWorld, tint}` and a `DataOffsets`
ramp, then emits one `DrawIndexed(count)`. The shader reads `Instances[DataOffsets.x]`
(never `SV_InstanceID` - see [[renderer-instancing]]).

A MultiMesh changes exactly one thing: **the `InstanceData` (and `DataOffsets` ramp) buffer
is persistent - allocated once, updated only on change - instead of ring-allocated and
re-filled every frame.** Everything downstream of that (the shader, the cull, the shadow
path) is unchanged. The primitive is "the same instanced draw, but the instance buffer
outlives the frame."

---

## 4. ECS layer - `InstancedMeshComponent` + manager

A new component manager alongside `MeshComponentManager`, following the same value-pool
pattern ([[scene-ecs-port]]):

```
InstancedMeshComponent {
 RefPtr<StaticMesh> mesh // shared by every instance
 Array<Ref<Material>> materials // one per submesh slot, as MeshComponent's (slot 0 the whole mesh)
 Array<Mat4> instances // CPU source of truth (or compact TRS to halve size)
 Array<Color> tints // optional per-instance tint (else one shared)
 DirtyRegions dirty // block bitset (à la Godot MULTIMESH_DIRTY_REGION_SIZE)
 u32 count, capacity
 AABB localBounds // merged bounds of all instances; recomputed only on change
 u32 gpuSetId = 0 // opaque handle into the renderer's persistent buffer pool
}
```

API mirrors Godot's MultiMesh, and crucially keeps the scene **renderer-agnostic** (Scene-
is-data; `gpuSetId` is opaque, the renderer owns the GPU memory):

- `SetInstanceTransform(i, m)` - writes the CPU cache, marks one dirty region, no GPU work.
- `SetInstanceTint(i, c)` - same.
- `SetBuffer(Span<Mat4>)` - bulk set + a single dirty flush (fast path for static content).
- `Reserve(n) / SetCount(n)` - grow/shrink; a resize triggers a full re-upload + AABB rebuild.

A fully static set is written once and never dirties again → after the first frame its
per-frame CPU cost is nil. Moving individual instances marks regions; only those upload.

---

## 5. The one new piece - a persistent instance buffer pool

This is the only genuinely new infrastructure. `m_instanceRing` is *transient* (per-frame,
frames-in-flight). A MultiMesh needs a **long-lived** `Storage` buffer per set (or a sub-
allocation from a shared arena), owned by the `MeshRenderer`, keyed by `gpuSetId`:

- **Allocate/grow** on first extract or a `SetCount` resize; upload all `InstanceData`.
- **Per frame**, apply only the dirty regions (a small `WriteBuffer`), or nothing if static.
- A persistent **`DataOffsets` ramp** `[0,1,…,N-1]` built once (still needed - we avoid
 `SV_InstanceID` - but static now).

### Frames-in-flight (the one real hazard)

The GPU may read last frame's buffer while this frame writes dirty regions. Options:

- **Static sets** (never dirty after init) → a single **immutable** buffer, no per-frame
 copy, no hazard. This is the common case and the whole point - handle it first.
- **Dynamic sets** (dirty regions each frame) → either N-buffer it (one copy per frame-in-
 flight, upload dirty to the current copy) or single buffer + upload-before-passes with a
 barrier. Memory: 150k × 144B ≈ 21MB per set (× frames-in-flight if dynamic) - acceptable.

Version the bind group by the buffer's generation, not its pointer (a reallocation on
resize must invalidate cached bind groups - see [[bind-group-cache-versioning]]).

---

## 6. Wiring through the existing pipeline

### 6.1 Extraction - O(1) per set

`ExtractInstancedMeshesInto` walks the manager and emits **one** RenderData per component -
no per-instance copy:

```
MultiMeshRenderData : MeshRenderData {
 u32 gpuSetId
 u32 instanceCount
 // worldCenter/worldRadius (base fields) = the MERGED AABB as a sphere -> single-AABB cull
}
```

It also carries which dirty regions to flush this frame (or "none"). This single line is
what removes the ~15ms of per-frame Extract + sort + fill for the set.

### 6.2 Cull / draw list - unchanged

`RenderView::BuildDrawList` treats `MultiMeshRenderData` like any `RenderData`: one
`DrawItem`, sorted by `worldCenter`, culled by the merged sphere. One frustum test, not
150k. Same for `m_shadowCasters` - one caster entry. View-frustum culling then gives the
open-world win: the whole set culls as a unit when off-screen.

### 6.3 Resolve / draw - the shadow win falls out for free

A MultiMesh resolves to **one** `ResolvedDraw` with **no fill loop** (contrast
`ResolveInstanced` / `ResolveDepthInstanced`, which fill `m_instanceRing` per item):

- bind the persistent `InstanceData` buffer as set 1 (its own buffer, one bind),
- bind the persistent `DataOffsets` ramp,
- `DrawIndexed(instanceCount)`.

**The shader is unchanged** - it already reads `Instances[DataOffsets.x]`. And the *same*
persistent buffer is bound for the depth prepass, the forward pass, **and all 4 shadow
cascades** - exactly Godot's mechanism, no per-cascade rebuild. For MultiMesh content,
`shadow.resolve` and the per-pass instance fill go to ~0.

---

## 7. Skinned crowds - a later, distinct primitive (SHIPPED)

> **Status: shipped** as the `InstancedSkinning` companion component (recommendation 2 below),
> built after the static primitive was proven. Adds M shared pose palettes/frame, per-clip
> app-level bucketing, per-instance tint, per-bone motion blur (ping-pong prev pool →
> `DataOffsets.z`), and an optional runtime mesh-merge. AnimatedCrowd sample; ~5-6k characters
> shadows-off, GPU-bound. The design reasoning that led here is preserved below.

The render backend already unifies static and skinned instancing (per-instance bone base in
`DataOffsets.y`, shared bone pool). So skinned crowds are one render path with the static
case. **But the ECS/cost model differs enough to keep them separate:**

- **Static instanced** → O(1)/frame (persistent buffer). The v1 win.
- **Skinned instanced** → O(unique poses)/frame - every animated instance needs bone
 matrices computed + uploaded each frame. It is NOT O(1). Affordable only with a **shared
 pose pool**: M unique poses among N instances → compute M, each instance's `DataOffsets.y`
 points to its pose.

Because `MeshComponent` unifies static+skinned (one component, mesh flag) only *works* since
a single mesh is cheap either way - and instancing breaks that symmetry (static = O(1),
skinned = O(poses)) - the recommendation is:

1. Ship `InstancedMeshComponent` (static) first - lean, O(1), closes the Godot gap.
2. Add skinning as a **companion component** - `InstancedSkinning` on the same entity
 (composition, not a parallel `InstancedSkinnedMeshComponent`) - holding the pose pool +
 per-instance animation state (clip/time/phase), driven by the existing animation stack
 ([[skinning-animation-plan]]). The renderer emits the skinned variant when both are
 present. This keeps the static path pristine and makes the differing cost model explicit.

Do not build either skinned option until the static primitive is proven.

---

## 8. Reused vs new

| Layer | Status |
|---|---|
| Shader (`Instances[DataOffsets.x]`, bone base) | **reused, unchanged** |
| Cull / `BuildDrawList` (single-AABB) | **reused** |
| Shadow-caster decoupling (`m_shadowCasters`, per-cascade cull) | **reused** - one caster entry, buffer shared across cascades |
| `ResolveInstanced` / `ResolveDepthInstanced` | branch for the no-fill persistent path |
| `InstancedMeshComponent` + manager | **new** (ECS) |
| Persistent instance buffer pool + dirty upload + FiF sync | **new** (the real work) |
| `MultiMeshRenderData` + extraction path | **new**, small |

---

## 9. Phasing - all shipped

1. ✅ **Persistent buffer pool** (renderer): allocate/grow, bind-group versioning by generation.
2. ✅ **`InstancedMeshComponent` + manager** (ECS): CPU cache, version bump, API.
3. ✅ **Extraction + `MultiMeshRenderData`**: one RenderData per set, merged AABB recomputed
 only on version change.
4. ✅ **Resolve branch**: bind-persistent + one draw, no fill; wired into depth/forward/shadow.
5. ✅ **Dynamic path**: frames-in-flight sync. Shipped N-buffered from the start (per-region
 InstanceData bind groups **and** byte-offset `DataOffsets` regions), dirty-for-FiF-frames
 upload - not the static-only shortcut the plan allowed.
6. ✅ **Sample**: RenderStressTest **M**-toggle (per-entity ↔ one MultiMesh); re-measured -
 120k spheres shadows-off 47→60 fps, render CPU 20→1.3ms (CPU-bound → GPU-bound).
7. ✅ **`InstancedSkinning` companion + shared pose pool** (§7): per-clip bucketing, per-
 instance tint, per-bone motion blur, optional runtime mesh-merge. AnimatedCrowd sample.

Decisions taken vs §10's open questions: **per-set buffer** (not a shared arena) and **full
`Mat4` InstanceData** (`World + PrevWorld + Tint`, 144B - not the 3×4 compaction the doc leaned
toward; that's now a deferred optimization in [renderer-improvements-history.md](../Archive/renderer-improvements-history.md) §2).

---

## 10. Open questions - resolved

The v1 open questions have been decided by the shipped implementation; the ones that stay open
are optimizations, now tracked in **[renderer-improvements-history.md](../Archive/renderer-improvements-history.md) §2**.

- **Compact per-instance data?** → **Shipped full `Mat4`** (`World + PrevWorld + Tint`, 144B),
 *not* the 3×4 the doc leaned toward. The ~3× smaller 3×4/TRS packing is a real future
 bandwidth/memory win → **deferred** (the renderer backlog (week-2026-09-05) / Archive/renderer-improvements-history.md §2).
- **Per-set buffer vs shared arena?** → **Per-set** (one bind per MultiMesh; few sets in
 practice). Shared-arena sub-allocation stays a future option, not needed yet.
- **Culling granularity?** → **One merged AABB** (Godot-parity). Coarse per-cell sub-AABB
 splitting for open-world-scale sets → **deferred** (the renderer backlog (week-2026-09-05) / Archive/renderer-improvements-history.md §2 / §4 spatial).
- **Motion vectors?** → Static sets carry `prevWorld = world` (zero motion). Skinned crowds
 ship real per-bone motion blur via the ping-pong prev pose pool (`DataOffsets.z`). Done.

---

## 11. Relationship to other engines

Flax describes "storing skeleton bones in a global shared GPU buffer and batching the same skinned
meshes into a single draw call" — that is exactly Draconic's **existing per-entity skinned
instancing** (shared bone pool + per-instance bone range via `DataOffsets.y` + `ResolveInstanced`
batching), where every character animates independently (**O(N)** unique poses).

`InstancedSkinning` is the step *beyond* that for crowds: **O(M) shared poses + O(1) scene**. The
trade is explicit — Flax keeps 5,000 fully-independent actors at O(N) animation; Draconic's crowd
shares M poses at O(M) to reach far higher counts. You pick per-entity for a few dozen distinct
characters and `InstancedSkinning` for thousands of same-clip agents. (Godot's `MultiMesh` is the
static analogue; it has no per-instance skeleton, so its crowds need the technique below.)

---

## 12. vs. Vertex Animation Textures (the other main technique)

The classic technique for *massive* crowds (100k+) is **Vertex Animation Textures (VAT)**: bake each
frame of the animation into a texture (a texel holds a vertex's position or offset at that frame),
and in the vertex shader sample the texture at `(vertexID, phase)` to get the deformed vertex. There
are no bones and no skinning matrices at render time — per-vertex is one or two texture fetches, and
per-instance is just a phase offset.

VAT and the shared pose pool solve the *same* problem — collapse a crowd to one draw with
per-instance addressing — but sit at opposite ends of a trade-off:

| | Shared pose pool (Draconic) | Vertex Animation Textures |
|---|---|---|
| Animation compute | **O(M) on the CPU** (M palettes/frame) | **O(0)** — fully baked, GPU-only |
| Per-vertex GPU | 4-bone matrix blend | 1–2 texture fetches (cheaper) |
| Per-instance data | bone base (`i % M`) | a phase / frame offset |
| Memory | M × boneCount matrices (~100 KB) | verts × frames × channels (often large) |
| Runtime skeleton | **yes** — any clip, blends, IK, bone sockets | **no** — animation is baked |
| Blend / mix clips | pose lerp at runtime | multiple VATs + blend, or pre-baked |
| Authoring | none (uses the rig directly) | an offline bake per clip |

The shared pose pool is deliberately the **middle ground**: it gets VAT-like **CPU scaling** (O(M),
not O(N)) while keeping a **live skeleton**. That matters — a pooled crowd character can still blend
clips, run IK, or hold a weapon socketed to a bone, none of which a baked VAT supports without extra
machinery. The price is the per-vertex bone blend (a GPU cost) instead of a texture fetch — and
since the crowd is already GPU-bound, that is a deliberate *"spend GPU to keep flexibility"* choice.

**When would VAT win?** At the extreme end — 100k+ (or high-poly) characters where the per-vertex
cost dominates and the crowd genuinely needs no runtime flexibility (identical baked loops). Because
Draconic already addresses instances by a per-instance base + phase, a VAT-style path drops into the
*same* instanced draw (bake to a texture, sample instead of blending bones) — so it's a future
option for that regime, not a fork. In short: the pose pool covers "thousands of flexible agents"
without giving up the skeleton; VAT is the escape hatch for "hundreds of thousands of baked ones."

---
