# Terrain vegetation (grass, then scattered props)

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: P0 BUILT 2026-09-21 (splat-driven grass); P1 BUILT 2026-09-22 (the VegetationMask
> resource + asset + importer + builder, Mask and SplatTimesMask placement, the Paint
> Vegetation brush with region-scoped regrow and persist; the WIND vertex variant off the frame
> clock and three material properties); P2 BUILT 2026-09-22 (Scattered layers hold authored
> instances, bucketed per chunk; the Paint Props brush stamps and erases them with the layer's
> rules, a spacing rule and physics collision rejection). SPLIT 2026-09-23: the one layer list
> became two on the same component - `proceduralLayers` (grown from a source) and `propLayers`
> (placed) over a shared base; `VegetationPlacement::Scattered` retired; data version 2 with a
> legacy reader for version 1 (Systems/renderer.md "Data"). P3 items stay their own seeds.
> Sized L; lands in phases with a green four-lane build after each. Origin: the backlog seed "grass / vegetation /
> foliage for terrain" (weekly_backlog.md, user 2026-08-26) and the Lumix parity doc,
> section 2 (vegetation is the largest gap in the terrain domain). Written after a
> full read of the terrain stack (Foundation/Heightfield + Terrain + Terrain.Resource,
> Pipeline/Terrain.Pipeline, Engine/Engine.Terrain, Editor/Editor.Terrain, their tests)
> and the instanced-mesh seam it rides (Engine.Render InstancedMeshComponent,
> MeshRenderer MultiMesh sets). Read CONVENTIONS.md first.

## Goal

Grass and small ground cover that grows out of the terrain's own data, then props
(rocks, bushes, saplings) an author scatters with a brush, both rendered through the
shipped instanced-mesh path at thousands of instances per chunk, culled and thinned by
distance, deterministic and headless-testable. Lumix is the yardstick (density-mask
procedural grass, per-quad instance buffers, distance fade, an authoring brush, prefab
scatter, impostors); the target is to reach it, not to copy it.

Non-goals for this spec: impostors as the far LOD (its own backlog seed; the
continuation of this one), GPU-driven scatter and culling (rides the occlusion /
DrawIndirect seed), streaming or paging of terrains larger than one heightfield,
trees as skinned or wind-animated hero assets, physics for grass.

## What exists (checked, cited per the Specs rule)

- **Instancing**: `InstancedMeshComponent` (Engine.Render RenderComponents.cppm) with a
  persistent per-set GPU buffer keyed by `MultiMeshRenderData::key`, uploads keyed by
  `version`, O(1) extraction, per-set frustum cull by merged bounds, per-set mesh LOD
  selection by aggregate bounds, shadows through the same caster path, and the GPU pick
  treating a set as one entity. Two facts that shape this spec: a set is NEVER evicted
  from the renderer's pool today (`MultiMeshSet::lastFrame` is written, nothing reads
  it), and there is no per-item shadow-cast flag on `RenderData` (every mesh casts;
  only the terrain has `castShadows`).
- **Terrain**: chunks of 64 quads over a heightfield of side 64k + 1 (`terrain::BuildChunks`,
  per-chunk `AABB bounds`), a quadtree with `CullNodes`, per-chunk LOD through the shared
  coverage metric (`foundation.lod`), `ExtractVisibleChunkDraws` giving the {chunk, lod}
  list per view. `Heightfield::GetHeightAt/GetNormalAt(worldX, worldZ)` sample the
  authoritative CPU grid; `QueryRay` picks the terrain under a pointer. The terrain
  entity places the grid by `chunkToWorld` (translation + Y rotation).
- **Splat**: `SplatWeights` (Terrain.Resource Splatmap.cppm), top-K (4) palette
  slots per texel with an implicit base layer; `WeightOfLayer(x, y, paletteIndex)` reads a
  layer's share; versioned (`BumpVersion`) so GPU caches rebuild; two sidecar streams
  ("pixels", "indices") on a `SplatmapAsset`; the brush cores `PaintTopK / EraseTopK /
  SmoothTopK` are pure and headless; the editor's Paint Splat tool wraps them with a
  stroke command (undo), `RegisterAssetEdit` write-back on Save, and a floating panel
  from `IViewportToolPanelProvider`.
- **Registration roots**: Engine.SceneSurface `kAllModules` (12 modules, each
  `AddXxxSceneManagers` + `RegisterXxxComponentReflection`); Pipeline.Registration
  `kBuilderCount = 26`, `kImporterCount = 9` (bump per builder or importer added).
- **Tests to mirror**: Foundation/Terrain.Tests (pure chunk, LOD, cull), Engine.Terrain.Tests
  (component reflection + serialize round trip, GPU caches by uid + version, renderer
  resolve on the Null device), Engine.Terrain.Backend.Tests (a lit dome through the full
  RenderFrame on Vulkan / WebGPU, pixels asserted), Editor.Terrain.Tests (a whole brush
  gesture scripted through `ViewportToolInput`, one command per stroke, the persist
  closure round trip).
- **Core**: `Random(seed)` with `NextU64 / NextFloat`; `HashBytes`. No time value reaches
  the View cbuffer today (`IblParams.zw`, `ShadowParams.yzw`, `DebugParams.yzw` are spare).
- **Shader flags**: 8 of 32 bits used; a `WIND` variant has room. Materials already
  carry cull mode (`CullModeConfig::None` = two-sided) and `BlendMode::Masked` (alpha
  test through the albedo alpha), which is what a grass card needs.
- **Nothing** vegetation-shaped exists in Raptor, the Beef port, legacy Sedulous
  (grass-named sample meshes only) or the Assiduous notes.

## Decision 1 - vegetation is a COMPONENT on the terrain entity, not a terrain asset field

`TerrainVegetationComponent` (Engine.Vegetation) sits on the entity that carries the
`TerrainComponent` and holds an unbounded `Array<VegetationLayer>`. Why a component:

- Reflection gives the inspector for free (collections + the generic list editor shipped
  with reflection P2), including the `Ref<StaticMesh>` and `Ref<Material>` pickers (each
  needs its InspectorView dispatch entry, per the standing rule).
- No cook change in P0: the terrain product (`TerrainResource`, its data version) is
  untouched, so no re-cook of existing terrains.
- Two scenes can dress one terrain asset differently (a winter and a summer level over
  one heightfield), which asset-level layers cannot express.

**P0 note (2026-09-21): built as specified, after one inspector extension.** The generic list
editor edited a `Ref<>` or `EntityRef` slot and showed a struct element only as its type label,
so an `Array<VegetationLayer>` had no per-field rows. The user chose to extend the editor rather
than change the data model: the list editor now builds a per-slot expander of the element's leaf
rows ("Layers 1: Grass", the layer's `name` titles it), every row and command addressing the
element through a `ComponentPropertyPath` (editor.md 3.5). `TerrainVegetationComponent` holds
`Array<VegetationLayer>` (each layer: name, mesh, material, the flat scatter fields, visible);
the manager also accepts the component on a child of the terrain entity (it walks the ancestry
for the `TerrainComponent`). The seed hashes (owner entity persistent id, layer index, chunk).

The alternative (layers on `TerrainAsset`, cooked into `TerrainResource` like the splat
palette) was rejected for P0 on cost: a builder change, a data version bump, and bespoke
terrain-page rows before any grass renders. It stays possible later as a "bake this
component into the asset" step if authoring ever wants it there.

## Decision 2 - where grass grows: the splat first, a painted mask second

A layer chooses its placement source:

- **Splat rule (P0)**: `splatLayer` = a palette index (or the base) plus `splatThreshold`;
  candidate density at a texel = the layer's share (`WeightOfLayer / 255`) remapped from
  `[threshold, 1]` to `[0, 1]`. Grass follows the painted grass texture with no extra
  authoring, which is the common case and why it ships first.
- **Uniform (P0)**: no source; the layer covers the whole terrain at its density (test
  fixtures, rock fields).
- **Vegetation mask (P1)**: a painted `VegetationMaskAsset` (one u8 density plane per
  layer, the splat sidecar pattern) referenced by the component; a layer names its
  plane. Painted with a Paint Vegetation brush that reuses the splat brush's stroke and
  panel machinery. A layer may combine mask and splat (multiplied) so a painted mask can
  carve a road through splat-driven grass.

The Lumix "type mask inside the splatmap" was rejected: it couples two authoring
surfaces and our top-K raster has no spare channel; a separate raster with its own
brush is the same controllability with the splat pattern already proven.

## Decision 3 - deterministic per-chunk scatter, CPU, prefix-sorted for distance fade

> AMENDED 2026-09-22: the prefix is the coarse bound only. The fade the eye sees is PER
> INSTANCE in the vertex shaders (rank in the tint alpha, the layer's window on a private view
> slot; renderer.md "Per-instance fade"): the per-chunk density produced straight seams
> between chunks and block pops at the window's end.

Scatter is a pure function in `foundation.vegetation` (Foundation/Vegetation, no RHI,
no scene): for one terrain chunk and one layer it produces the instance list.

- **Seed**: `HashBytes` over (terrain entity persistent id, layer index, chunk x, chunk z)
  feeding `core::Random`. The same inputs always give the same instances, frame to frame
  and on any machine; tests pin transforms against a seed.
- **Candidates**: `density` (instances per square metre) times the chunk's world footprint,
  capped by `maxInstancesPerChunk` (default 4096; a layer over budget scales its density
  down and warns once). Each candidate: a uniform XZ inside the chunk, then the placement
  source's density at that point against a random threshold (rejection sampling), then
  the slope limit (`GetNormalAt` against `maxSlopeDegrees`), then the height window
  (`heightRange`), then Y from `GetHeightAt`, a random yaw, a uniform scale in
  `scaleRange`, and optional alignment to the surface normal (`alignToNormal`). The
  transform is entity-relative (the instanced-mesh convention); the component's entity
  world matrix places it.
- **Fade order**: every instance gets a random `rank` in [0, 1); the list is sorted by
  rank. Distance fade is then a PREFIX: a chunk at distance d draws the first
  `count * DensityAt(d)` instances, with `DensityAt` = 1 inside `fadeStart`, a smooth
  falloff to 0 at `fadeEnd`. The GPU buffer holds the full list once; only the draw
  count changes with distance, no re-upload and no per-frame culling work per instance.
  Thinning is per instance and stable (an instance's rank never changes), so a chunk
  never pops as a whole.
- **Bounds**: the chunk's terrain AABB expanded by the mesh's local bounds times the
  maximum scale; this is the set's cull sphere.

CPU scatter is the portable v1 (Null device, headless tests, WebGPU without readback).
GPU-driven scatter and culling is the P3 follow-on and shares the DrawIndirect and
occlusion seeds.

## Decision 4 - one instanced set per (layer, chunk), built on demand, evicted by the renderer

The manager keeps, per terrain entity and layer, a cache of chunk sets keyed by chunk
index. A set is built the first time its chunk is both frustum-visible and within
`fadeEnd` of the camera (a per-frame build budget, default 4 chunks per frame, spreads a
cold start over frames rather than stalling one). The cache is invalidated by the
heightfield uid + version, the splat uid + version, the mask version and a hash of the
layer's parameters, so a sculpt or a paint stroke regrows only the touched chunks (the
brush's touched region maps to chunk indices).

Extraction emits ONE `MultiMeshRenderData` per (layer, chunk) whose instances are in
range: `key` = the same seed hash (stable across frames, so the renderer's persistent
buffer is reused), `transforms` borrowed from the cache for the frame (the instanced-mesh
convention: immutable for the frame), `instanceCount` = the fade prefix, `version` = the
cache build version, `worldCenter / worldRadius` = the expanded chunk bounds. Sets
out of range are simply absent from the snapshot.

Two renderer changes this needs, both small and tested in Render.Tests:

- **Eviction**: a `MultiMeshSet` not extracted for `kMultiMeshEvictFrames` (120) retires
  its buffer and bind groups through the retire queue (the frames-in-flight rule) and is
  erased from the pool. Today nothing is ever freed; per-chunk sets would otherwise pin
  every chunk's grass ever seen.
- **Shadow opt-out**: `RenderData::castShadows` (default true) honoured by the shadow
  caster list and the local-shadow tiles. Grass layers default to `castShadows = false`
  (the parity doc's per-layer rule; grass shadows are the single most expensive thing a
  grass system can do), props default to true.

Mesh LOD works unchanged: a per-chunk set's aggregate bounds are small, so the shipped
per-set coverage pick is correct here (the mesh-lod spec's ruling was about terrain-wide
sets; this spec never builds one).

## Decision 5 - wind is a vertex-shader variant driven by a view time and a material property

`ShaderFlags::Wind` (`WIND`) in forward.vs / shadow_depth.vs / pick_ids.vs: a sway offset
in world space, `sin(time * speed + hash(instance world XZ)) * strength * heightMask`,
where `heightMask` is the vertex's normalised height in the mesh's local bounds (roots
stay put, tips move). `time` rides a spare View cbuffer lane (`IblParams.z`, plumbed from
the frame delta accumulator); `strength / speed` are material properties, so a card
material opts in and a rock never sways. Applies to any instanced or single mesh with
the property set, not only vegetation. P1, not P0: P0 proves the pipeline with static
grass.

## Decision 6 - props scatter into instanced sets, not entities

The P2 scatter brush paints instances of a chosen prop layer (mesh + material + rules)
into a `TerrainVegetationComponent` layer of kind `Scattered`: the brush appends
authored instances (persisted on the component like `InstancedMeshComponent::instances`,
entity-relative) instead of generating them from a density source; the same per-chunk
set machinery renders them (chunk = the instance's XZ). Collision rejection at paint
time uses `PhysicsWorld::ShapeOverlap` against the scene's current bodies when a
physics world exists, else a bounds test against already-scattered instances.

Prefab scatter (one entity per placed prefab, Lumix-style) is deliberately NOT this: it
is the existing prefab spawn plus a placement gesture, and at prop counts it defeats the
instancing. If an author needs per-prop entities (a chest, a lamp), the prefab tool is
the answer; a rock field is a scatter layer.

## Data model

```
enum class VegetationPlacement : u8 { Uniform, Splat, Mask, Scattered };

struct VegetationLayer
{
    String name;                                   // inspector label
    Ref<geometry::StaticMesh> mesh;                // the instanced mesh (a card, a tuft, a rock)
    Ref<materials::Material> material;             // optional override (else the mesh's)
    VegetationPlacement placement = Scattered; // a NEW layer is manual (ruling 2026-09-22)
    u32 splatLayer = 0;                            // Splat: palette index; kBase = the base layer
    f32 splatThreshold = 0.25f;                    // Splat: share below which nothing grows
    u32 maskPlane = 0;                             // Mask: plane index in the mask asset
    f32 density = 2.0f;                            // instances per square metre (Uniform/Splat/Mask)
    Float2 scaleRange{0.8f, 1.2f};
    f32 maxSlopeDegrees = 35.0f;
    Float2 heightRange{-1.0e6f, 1.0e6f};           // world Y window
    bool alignToNormal = false;
    f32 fadeStart = 40.0f;                         // metres: full density inside
    f32 fadeEnd = 80.0f;                           // metres: nothing beyond
    bool castShadows = false;
    u32 maxInstancesPerChunk = 4096;
    Array<Float4x4> scattered;                     // Scattered: the authored instances
};

struct TerrainVegetationComponent                  // on the TerrainComponent's entity
{
    Array<VegetationLayer> layers;
    Ref<VegetationMask> mask;                      // P1; nil = no mask planes
    u32 version;                                   // bumped on any authored change (cache key)
    bool visible = true;
};
```

`VegetationMask` (P1, Foundation/Vegetation.Resource): width x height u8 planes, one per
`planeCount`, sidecar stream "densities", versioned like `SplatWeights`; `VegetationMaskAsset`
+ builder + a PNG importer (one plane per channel) in Pipeline/Vegetation.Pipeline; the
factory and registration mirror Splatmap's.

Serialization follows the component convention (one layout, the current data version;
a wire change bumps it and the scene is re-saved). `scattered` transforms travel as the
component's own array (the InstancedMeshComponent precedent), never a sidecar in P2; if
a scene's scatter grows past the inline budget, that is the bulk-data rule's trigger to
move it to a sidecar, priced then.

## Runtime architecture

- **Foundation/Vegetation** (`foundation.vegetation`, Foundation::Core + Heightfield +
  Terrain.Resource): `VegetationLayer`, `ScatterChunk(seed, chunk, heightfield, splat,
  mask, layer, out)`, `DensityAtDistance(d, fadeStart, fadeEnd)`, `FadePrefix(count, density)`,
  `ChunkSeed(...)`, `ChunksTouchedBy(region)`. Pure; every rule unit-tested here.
- **Foundation/Vegetation.Resource** (P1): `VegetationMask` + factory + registration.
- **Engine/Engine.Vegetation** (`engine.vegetation`, links Engine.Terrain + Engine.Render):
  `TerrainVegetationComponent` + `TerrainVegetationComponentManager` (a
  `SerializableComponentManager`, an `IRenderDataProvider`), `VegetationSubsystem` (the
  TerrainSubsystem pattern: registers the provider per scene at SystemsReady, tears GPU
  state down at Destroying), `AddVegetationSceneManagers` +
  `RegisterVegetationComponentReflection` as the 13th `SceneModule`. The manager finds its
  terrain through the entity's `TerrainComponent` (the resource's heightfield + splat) and
  the per-view camera from the extraction context (distance for the fade prefix; one
  snapshot per scene per frame today, so the fade is computed against the FIRST view of
  the scene - the same limit the terrain's chunk LOD has; per-view prefixes are a
  follow-on when a second view needs them).
- **Foundation/Render**: `RenderData::castShadows`; MultiMesh set eviction; `WIND` flag +
  time lane (P1).
- **Pipeline/Vegetation.Pipeline** (P1): the mask asset, builder, importer; registration
  counts bumped.
- **Editor/Editor.Vegetation** (P1 brush, P2 scatter brush): `VegetationPaintTool`
  (`vegetation.paint`) and `VegetationScatterTool` (`vegetation.scatter`) as
  `IViewportTool`s from an `IViewportToolProvider`, panels from
  `IViewportToolPanelProvider` (Float placement like the terrain brushes), strokes as one
  command each, mask write-back through `RegisterAssetEdit`; `RegisterVegetationViewportTools`
  + `RegisterVegetationToolPanels` called where the terrain ones are. P0 has no editor code
  beyond the reflection-driven inspector and the two `Ref<>` picker dispatch entries.

## Phases

**P0 - procedural grass from the splat (static). BUILT 2026-09-21.** As specified (the
Decision 1 data model, once the inspector's list editor learned struct elements), the
snapshot's first-view origin as the fade
distance (`ExtractedScene::ViewOrigin`, set by RenderSubsystem before the providers run;
distance gates the build, the renderer frustum-culls the sets per view), the fade order as
the generator's own uniformly random sequence (equivalent to a rank sort, no sort), and one
renderer fix the WebGPU probe forced: a MultiMesh set's region capacity rounds to 16 so its
per-frame region offsets meet every backend's storage-buffer alignment.
Foundation/Vegetation with tests; Engine.Vegetation component + manager + subsystem +
module registration; RenderData::castShadows + shadow list gating; MultiMesh eviction;
inspector picker entries; TerrainPlayground gains a grass layer over its generated dome
(the visual smoke test); a Vulkan + WebGPU pixel probe. Acceptance: a terrain with a
grass splat layer shows grass where that layer is painted, none elsewhere, thinning to
nothing at `fadeEnd`; a headless scatter is byte-identical for a seed; a set out of range
for 120 frames has no GPU buffer; grass casts no shadow by default and a rock layer does;
four lanes green; `scatter-off` (no component) renders byte-identical to today.

**P1 - painted mask + wind.** BUILT 2026-09-22. Wind as specified (Decision 5) with two
refinements: the height mask needs no mesh bounds - a third material property, `WindHeight`
(the local y at which the sway is full; roots at y <= 0 stay), sits in the cbuffer's last
spare lane alongside `WindStrength` / `WindSpeed` at 24 / 28, and the variant is selected by
the material's `WindStrength` default being above zero rather than an authored flag, so no
material page toggle is needed and every existing material is byte-identical. The time is the
SCENE's clock (EnvironmentSystem, from the scene's composed dt), not the app's: a paused scene's
grass stands still, Simulate sways the editing scene; the frame clock stands in for probes. The mask half as
specified, with one addition: the
placement enum gained `SplatTimesMask` (the "splat multiplied in" combination as its own mode,
so a plain Mask layer needs no splat and a carved road is an erase stroke); the mask reference
sits on the component, the plane index on the layer; the brush maps its texel rect to the
heightfield grid through `TerrainVegetationComponentManager::InvalidateFootprint`.
VegetationMask resource + asset + importer + builder; the Paint Vegetation brush (paint,
erase, smooth over a plane; radius, strength, spacing, airbrush; one command per stroke;
mask write-back on Save) and its panel; mask-driven placement with the splat multiplied
in; `WIND` variant + view time + material properties, honoured by the depth and pick
variants too. Acceptance: a stroke regrows only the touched chunks; the persist closure
round-trips through a re-cook; wind moves tips and not roots (a vertex probe), and a
material without the property is bit-identical to P0.

**P2 - prop scatter brush.** BUILT 2026-09-22 as specified: `VegetationLayer::instances`
(terrain-local, serialized with the layer, no inspector row - the brush is the editor) feed the
same per-chunk sets (each instance maps to one chunk by its XZ; a content hash re-buckets the
layer); `foundation.vegetation::ScatterStamp` is the pure stamp (density x disc area candidates
under the layer's rules, a spacing rule against existing props, a blocked query) and
`EraseInstancesInDisc` the eraser; `VegetationScatterTool` ("vegetation.scatter", Paint Props)
wires the physics world's `ShapeOverlap` (the terrain's own body excluded) when the scene has
one, else the spacing rule alone; one command per stroke.
Layer kind `Scattered`; the brush places instances with density, jitter, scale and slope
rules and collision rejection; erase removes instances under the brush; undo per stroke;
the instances persist on the component. Acceptance: a scripted stroke places a
deterministic count inside the footprint, none on a rejected slope or inside an existing
body; a scene round-trips the scattered array.

**P3 - deferred (own seeds):** octahedral impostors as the far LOD (the backlog seed);
GPU-driven scatter and culling over DrawIndirect; per-view fade prefixes; grass on the
CDLOD-morphed surface when the morph lands (today grass samples the true heightfield
height; a morphed chunk surface can sink below a blade's root by the morph amount at
distance, which the fade hides at the distances the morph applies).

## Files to touch (P0)

New: `Code/Foundation/Vegetation/{CMakeLists.txt, VegetationModule.cppm, VegetationLayer.cppm,
Scatter.cppm, ScatterImpl.cpp}`, `Code/Foundation/Vegetation.Tests/`, `Code/Engine/Engine.Vegetation/
{CMakeLists.txt, VegetationModule.cppm, VegetationComponents.cppm, VegetationComponentsImpl.cpp,
VegetationSubsystem.cppm}`, `Code/Engine/Engine.Vegetation.Tests/`,
`Code/Engine/Engine.Vegetation.Backend.Tests/` (the terrain probe's shape).
Edited: root `CMakeLists.txt` (subdirectories + the test-suite block), `Engine.SceneSurface`
(`kAllModules` + the module count tripwire), `Foundation/Render/RenderData.cppm`
(`castShadows`), `PipelineImpl.cpp` (caster list gating), `MeshRenderer.cppm/Impl`
(eviction), `Editor.Scene/InspectorView` (two `Ref<>` dispatch entries),
`Samples/TerrainPlayground`, `Documentation/Systems/renderer.md` (a vegetation section),
`Documentation/Guides/terrain-authoring.md` (a "6. Vegetation" section), the weekly.

## Gotchas

- `MultiMeshRenderData::transforms` is BORROWED for the frame: the chunk cache must not
  reallocate while a snapshot references it. Build new sets into fresh arrays and swap
  them in between frames (the extraction runs on the job system; treat the cache as
  immutable from extraction start to `EndRendering`).
- The chunk seed must include the layer INDEX and the entity's persistent id, never a
  pointer or a frame counter (determinism, and the renderer keys its persistent buffer on
  it).
- A sculpt stroke moves heights under existing grass: invalidate by the heightfield
  version, but only the touched chunks (`HeightfieldRegion` -> chunk indices), or every
  brush frame regrows the whole terrain.
- The fade prefix means the GPU buffer always holds the full chunk list; `instanceCount`
  varies per frame while `version` does not. The renderer already treats count and
  version separately (count is per frame, capacity from the first upload); keep it so.
- `maxInstancesPerChunk` bounds memory per set (4096 x 144 bytes = 590 KB); with
  eviction the pool stays at the working set. State the budget in the Systems doc.
- Grass cards want `CullModeConfig::None` + `BlendMode::Masked`; the alpha test runs in
  the depth prepass, the shadow pass (if enabled) and the pick pass already.
- WebGPU: instanced draws and Masked materials are on the web path already; nothing new
  to validate beyond the probe running on both backends.
- `ExtractedScene` is one snapshot per scene per frame; a fade computed against the
  first view is the stated P0 limit (see Runtime architecture).

## Tests (the spec's contract)

Foundation/Vegetation.Tests: seed -> identical transforms twice and across layer/chunk
seed changes; density scales candidate count; splat rule places only where the layer's
share clears the threshold; slope and height windows reject; alignToNormal tilts by the
sampled normal; fade prefix: full inside fadeStart, zero beyond fadeEnd, monotone
between; ChunksTouchedBy maps a region to the right chunk indices; the per-chunk cap
scales density and reports it.
Engine/Engine.Vegetation.Tests: reflection + serialize round trip; the manager builds a
set only for in-range visible chunks and emits one MultiMeshRenderData per (layer,
chunk) with the fade prefix as its count; a heightfield or splat version bump regrows
only the touched chunks; the build budget spreads a cold start; a scene without the
component extracts nothing extra.
Render.Tests: a MultiMesh set unseen for kMultiMeshEvictFrames releases its buffer
through the retire queue; castShadows = false items are absent from the caster list.
Engine.Vegetation.Backend.Tests: a splat-driven grass layer over the dome renders green
on the painted half and not on the other, on Vulkan and WebGPU; a far camera shows the
fade (fewer lit grass texels).
Editor tests arrive with P1 (brush gesture, persist round trip) and P2 (scatter stroke).
