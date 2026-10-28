# Terrain holes (cut-outs for caves, wells, doorways)

> STATUS: P0 BUILT 2026-09-23 (the sample plane + the "holes" stream, HeightfieldSource 2 /
> builder 3; holed chunks' own index buffers through TerrainHoledMeshCache; Jolt's no-collision
> sample; the nav bake's block skip; QueryRay's crossing skip; vegetation's cell reject; probed
> at the texel on Vulkan + WebGPU). P1 BUILT 2026-09-23 (the `terrain.hole` brush, Cut / Fill,
> HoleStrokeCommand, a persist that writes BOTH sidecars, its panel). P2 BUILT 2026-09-23: the
> alpha-tested rim (see "P2 - the rim" below; it reverses the "rejected: discard" line of
> Decision 3 for holed chunks only). Props over a cut + the brushes' hole-plane pick BUILT
> 2026-09-23 (Decision 4, the raycast and vegetation bullets). Departures from the text
> below: the heightfield page previews only image-backed sources, so the hole overlay there has
> nothing to draw on for a painted heightfield and was left out; the brush does not call
> InvalidateRegion (like sculpt, a version bump regrows every set of a layer - the cost, not
> the correctness, of a cut under grass); a fill over solid ground pushes no command. Sized M. Origin: the backlog seed "erosion + hole cutting for
> terrain" (weekly_backlog.md, user 2026-08-26; terrain-backlog.md carries the one standing
> rule: holes must EXCLUDE their cells from the nav bake). Written after a full read of the
> terrain stack (Foundation/Heightfield + Terrain + Terrain.Resource, Engine/Engine.Terrain,
> Engine.Physics, Editor.Terrain, Editor.Navigation, Foundation/Vegetation and their tests).
> The Beef port is catching up to this engine; every decision below is language-neutral and
> names the seam it lands on so the port can follow it one for one. Read CONVENTIONS.md first.

## Goal

An author paints a hole into a terrain and the terrain stops existing there: nothing draws,
nothing casts or receives a shadow, nothing collides, no navmesh forms, no grass or prop grows,
and a ray (a brush pick, a gameplay trace) passes through to whatever sits below - a cave
mesh, a well, a basement. Everything else about the terrain stays exactly as it is today.

Not goals: soft or partial holes (a sample is solid or cut), holes as a runtime-animated
effect, and erosion (its own seed; it shares the sculpt spine but nothing here).

## What exists (checked, cited per the Specs rule)

- **Heightfield** (`Code/Foundation/Heightfield/Heightfield.cppm`): `u16` samples, row-major
  `gx + gz * S`, `S = 64k + 1`, one `uid` per object and one `Version()` bumped by every edit
  (`BumpVersion`). No per-sample flags of any kind. `QueryRay` (line 203) is THE hit test every
  brush uses: it marches the ray, finds the sign change of `SignedGap`, then bisects. Brush
  cores (`SculptRaise/Flatten/Smooth`) run over `detail::VisitBrush` and return a
  `HeightfieldRegion` (inclusive touched rect) for region-delta undo.
- **Cooked form** (`Foundation/Heightfield.Resource/HeightfieldResource.cppm`):
  `HeightfieldSource {size, worldSize, minY, maxY}` at `RTTI_DEFINE_OBJECT_VERSIONED(..., 1)`
  plus the bulk stream `kHeightStream = "heights"`; `HeightfieldAssetBuilder::Version() == 2`.
  The strict-versioning rule (`Serialize.cppm:229`) refuses any other chain - a wire change is
  a bump plus a re-cook, never a migration.
- **Chunk geometry** (`Foundation/Terrain/Terrain.cppm`): ONE shared 65x65 grid vertex buffer
  and ONE shared index buffer per LOD 0..6 (`BuildChunkGridVertices/Indices`: surface prefix
  of `ChunkLodSurfaceIndexCount(lod)` triangles, then a double-sided skirt around the four
  edges). Every chunk draws the same buffers; the vertex shader (`Data/Engine/Shaders/terrain.vs.hlsl`)
  reads `Texture2D<uint> HeightTex` (R16Uint, `Engine.Terrain/HeightTexture.cppm`, rebuilt in
  place on a version change). There is no per-chunk mesh to drop a triangle from.
- **Renderer** (`Engine.Terrain/TerrainRenderer.cppm`): `LodMesh {indexBuffer, indexCount,
  surfaceIndexCount}` per LOD built once in `Initialize`; colour draws the full count, the
  depth (shadow / prepass) and pick paths draw the skirtless `surfaceIndexCount` prefix
  through `ResolveDepthLike`. `TerrainRenderData` (`TerrainRenderData.cppm`) copies the chunk
  and quadtree arrays into the frame arena; `ExtractVisibleChunkDraws` yields `ChunkDraw
  {chunkIndex, lod}`.
- **Physics** (`Engine.Physics/PhysicsSubsystem.cppm:749`, `fillHeightfield`): every body
  creation converts the samples to world-Y floats and `PhysicsWorldImpl.cpp:316` builds a
  `JPH::HeightFieldShapeSettings` from them. Jolt's own hole sentinel,
  `HeightFieldShapeConstants::cNoCollisionValue`, is unused (only mentioned in a comment).
  Jolt's rule: any triangle with a no-collision VERTEX is not collidable.
- **Navigation** (`Editor.Navigation/NavigationBakeImpl.cpp:99`, `CollectNavigationGeometry`):
  triangulates the heightfield inside the zone at a `stride` of grid cells per nav cell and
  emits two +Y triangles per stride block. No exclusion hook.
- **Vegetation** (`Foundation/Vegetation/ScatterImpl.cpp`): `PlacementShareAt` (line 80) is
  the one gate every placement mode passes through; `ScatterChunk` (line 105) and
  `ScatterStamp` (line 271) then reject on slope and height. Set caches invalidate on the
  heightfield's `Version()` (`Engine.Vegetation/VegetationComponentsImpl.cpp:433`).
- **Brushes** (`Editor.Terrain`): `terrain.sculpt` and `terrain.splat` are scene-viewport
  `IViewportTool`s; a stroke is one `IEditorCommand` holding the region's before/after slices
  (`SculptStrokeCommand`, `TypeId "terrain.sculpt.stroke"`), `BumpVersion` for the live
  re-upload, and an asset-edit closure that writes the source instance's bulk stream
  (`SculptImpl.cpp:322`). Panels come from `editor.app:tool_panel_widgets`.
- **Precedent for a byte plane beside bulk data**: `VegetationMask` (a u8 raster with planes,
  its own stream, brush stamps, region undo) and the splat dual raster (two byte rasters under
  one uid + one version, two streams).

## Decision 1 - a hole is a SAMPLE flag, and a triangle with a hole vertex is gone

The mask marks heightfield SAMPLES (the `S x S` grid, the same raster as the heights), not
cells. The one rule every consumer applies: **a triangle is removed when any of its vertices
is a hole sample.** At LOD 0 a single hole sample removes the up to six triangles around it
(a two-cell-wide cut); a painted disc removes the disc plus a one-cell rim.

Why samples, not cells:

- **It is Jolt's model.** `cNoCollisionValue` is per sample and removes every triangle that
  touches it. A per-cell mask cannot be expressed exactly in the collision shape: marking one
  sample kills four cells, so a per-cell hole would either have no collision hole (a one-cell
  hole you cannot fall through) or a collision hole wider than the picture (you fall through
  visible ground at the rim). Per-sample makes render, collision, nav, scatter and raycast
  agree to the triangle.
- **It is the height raster.** Samples already have the `S x S` layout, the row-major index,
  the clamped `Index()`, the region-delta undo and the R16 texture upload; a second byte per
  sample rides all of it. Unreal's landscape visibility mask is per vertex for the same
  reasons; this will feel familiar to anyone who has cut a cave there.
- **Coarser LODs stay honest.** A LOD-`L` quad spans a `2^L` block of samples; the quad is
  removed when ANY sample in its block (interior included) is a hole. A hole never shrinks with
  distance and surface is never drawn over one; a distant hole may look a little larger than
  it is, which is the right direction (the skirt rule below keeps the rim closed).

Storage: `Array<u8> m_holes` on `Heightfield`, `S * S` bytes, `0` = solid, `255` = hole, no
other values (the brush writes 0 or 255; the byte, not a bit, keeps the API, the slices and
the undo shape identical to the height raster and leaves room for the resource factory to
validate the blob size the way it validates heights). `m_holeCount` caches how many are set
so every consumer has an O(1) "no holes here" fast path and the common terrain pays nothing.

API on `Heightfield`: `IsHole(gx, gz)`, `SetHole(gx, gz, bool)`, `Span<const u8> Holes()`,
`HoleCount()`, `HasHoles()`, `CellHasHole(cx, cz)` (any of the four corner samples),
`BlockHasHole(gx0, gz0, gx1, gz1)` (inclusive; the LOD and nav question). `SetHole` bumps
the version like `SetSample` does not - through the brush cores, which bump once per stroke:
`CutHoles(hf, worldX, worldZ, radius)` and `FillHoles(...)` over `detail::VisitBrush` with a
HARD edge (weight `> 0` marks; there is no falloff to a boolean), returning the
`HeightfieldRegion` touched. `Resize`/construction allocate the plane zeroed.

## Decision 2 - the hole plane is a third bulk stream on the heightfield, always present

`HeightfieldSource` gains no metadata (the plane's size is `size * size`); the cooked
instance gains the stream `kHoleStream = "holes"` beside `"heights"`, ALWAYS written (the
one-layout rule; an all-zero plane is a legal, common file). `HeightfieldSource` dataVersion
1 -> 2, `HeightfieldAssetBuilder::Version()` 2 -> 3, `HeightfieldSource::Build(heightBlob,
holeBlob, allocator)` refuses a hole blob whose size is not `S * S` the way it refuses a bad
height blob; the factory's `BuildFrom` reads both streams. `HeightfieldAsset` (a `.png`/`.r16`
file source) cooks an all-zero plane; a painted heightfield (empty `fileName`, streams are
truth) carries whatever the brush left. `ScanDependencies` declares both streams so a hole
edit re-cooks. The sculpt persist closure (`SculptImpl.cpp:322`) keeps writing only the
height stream; the hole brush's closure writes only the hole stream; the two never clobber
each other because the streams are separate.

Size: 1 MB per 1025-squared heightfield, on disk and in memory - the bulk-sidecar rule holds,
the texture (below) is 1 byte per sample. Bit-packing is a later optimisation if it ever
matters; it would change the wire (a bump), never the API.

## Decision 3 - holed chunks draw their own index buffers; the common path is untouched

The shared grid stays the path for every chunk without holes. A chunk whose sample block
(`[gridX0, gridX0 + 64] x [gridZ0, gridZ0 + 64]`, edges shared with neighbours) contains a
hole sample gets its OWN `LodMesh` set, built on the CPU by a pure function in
`foundation.terrain`:

- `BuildHoledChunkIndices(const Heightfield&, const TerrainChunk&, u32 lod, Array<u32>& out,
  u32& outSurfaceIndexCount)`: the same walk as `BuildChunkGridIndices` with each quad
  skipped when `BlockHasHole` over its stride block says so (the surface prefix), then the
  skirt walls with each edge segment skipped when either of its two edge samples is a hole
  (so a skirt never hangs under a cut rim; the neighbour chunk shares the edge samples and
  makes the same call). Headless-tested against hand-built masks.
- `TerrainChunk` gains `bool hasHoles` (set by `BuildChunks`), and `TerrainChunkCache` in
  `TerrainComponentManager` is keyed by the heightfield version as well as its uid, so a
  hole edit rebuilds chunk facts (a fully holed chunk - every sample cut - is dropped from the
  draw list outright).
- `Engine.Terrain` owns a `HoledChunkMeshCache` beside the height-texture cache: key
  `(Heightfield::uid, chunkIndex)`, value `version + LodMesh[kMaxChunkLod + 1]`, rebuilt in
  place on a version change, retired through the `GpuRetireQueue` like the textures, cleared
  in `OnPrepareShutdown` (the render-flushes-first rule). Memory is about 130 KB per holed
  chunk across all LODs; a hundred holed chunks is 13 MB, and a terrain with no holes
  allocates nothing.
- `TerrainRenderData` carries `const HoledChunkMesh* holedMeshes` + `u32 holedMeshCount`
  (chunkIndex -> LodMesh pointers, copied into the frame arena like the chunks); the renderer's
  draw loop picks `holedMeshes[i]` for a chunk that has one, else the shared `LodMesh`. Colour,
  depth (shadow casters, the prepass) and pick draws all go through the same choice, so a hole
  is a hole in shadows and under the cursor for free.

Rejected for the common path: a per-fragment `discard` reading a hole texture. It needs a
pixel shader on the depth and pick paths that have none today, defeats early-Z on every
terrain fragment, costs a texture load per fragment on every terrain forever, and still leaves
the skirt problem to solve in geometry. P2 (below) adopts it for HOLED CHUNKS ONLY, where the
cost is bounded by the rare chunk that carries a cut and the geometry rule still does the
skirts and the fully cut quads. Rejected: rebuilding the whole terrain's buffers per chunk - the shared grid
is the reason terrain draws cost what they do, and holes are rare.

## Decision 4 - collision, nav, raycast and scatter all apply the sample rule

- **Physics**: `fillHeightfield` writes `cNoCollisionValue` for a hole sample instead of the
  height. Nothing else changes; Jolt removes exactly the triangles the renderer removes. The
  ray test in `Physics.Tests` proves a ray through a cut sample reports no hit and the
  neighbouring solid quad still does.
- **Navigation**: `CollectNavigationGeometry` skips a stride block's two triangles when
  `BlockHasHole` over that block is true (the stride block, not just the corners - a small
  hole inside a coarse nav cell must still open it). This is the backlog's standing rule made
  concrete; the nav test bakes a flat zone with a cut and shows the navmesh has a gap agents
  route around.
- **Raycast**: `Heightfield::QueryRay` accepts a crossing only when the cell it lands in has no
  hole corner (`CellHasHole`); a crossing inside a hole cell is skipped and the march
  continues, so the ray exits the terrain's AABB with no hit and the caller's next candidate
  (a cave mesh, the physics world) can answer. This is what makes the brushes (sculpt, splat,
  the mask painter) land on the cave floor instead of the air where the terrain used to be,
  and it is the behaviour a gameplay trace wants. The brushes that work ON the hole - Fill,
  the prop brush erasing what stands over a cut - use `QueryRayIgnoringHoles`, the same march
  with the cut samples as surface (built 2026-09-23 after the RTHoles1 report: Fill needed a
  click from the rim and the eraser never activated over a hole). Never a gameplay query.
- **Vegetation**: `PlacementShareAt` returns 0 when `CellHasHole` at the local position, and
  `ScatterStamp` rejects a candidate the same way. A STAMPED prop placed before the cut is
  not deleted by it: `BuildSet` leaves an authored instance out of the bucket while its cell
  is cut (the rule for props too), Fill brings it back through the version bump, and the
  eraser can reach it through the hole (built 2026-09-23; chosen over evicting the instance
  from the authored list, which would make a terrain brush edit vegetation data and need its
  own undo payload). Set caches already key on the heightfield
  version, so a cut regrows the touched chunks through the existing region path
  (`InvalidateRegion` from the hole brush, like sculpt).
- **GPU picking** is geometry, covered by Decision 3. **Bounds** (`CellBounds`, the chunk
  AABB, the quadtree) keep counting hole samples' heights: a hole is an absence of surface,
  not a change of the field, and a conservative box costs nothing.

## Decision 5 - the brush is a third terrain viewport tool

`terrain.hole`, display "Cut Holes", beside `terrain.sculpt` and `terrain.splat` in
`Editor.Terrain`, registered by the same `TerrainViewportToolProvider`. One panel from
`editor.app:tool_panel_widgets`: a mode row (`SegmentedToggle`: Cut / Fill) and a radius
float; the status bar says the mode and radius ("Cut Holes [FILL] r=6.0"). Hard-edged disc,
no strength (a sample is cut or not). `UnavailableReason()` mirrors sculpt ("select an entity
with a terrain whose heightfield resolves").

Stroke = `HoleStrokeCommand` (`TypeId "terrain.hole.stroke"`) holding the region's before /
after hole slices, `Execute` writes AFTER, `Undo` writes BEFORE, both through `SetHole` then
one `BumpVersion` (the live path: height texture untouched, chunk meshes rebuilt for the
touched chunks only, vegetation regrown through `InvalidateRegion`). `EndStroke` registers the
asset-edit closure that writes the source instance's `"holes"` stream (the sculpt closure's
twin, reading and syncing the same `HeightfieldAsset`). One stroke, one undo entry, like the
others.

The heightfield asset page stays a 2D grayscale of heights (the asset-identity ruling); it
draws hole samples as a flat magenta overlay so an author can see what a cooked heightfield
carries without opening a scene. Nothing else in the editor changes.

## Data model (the whole delta)

```
Heightfield            + Array<u8> m_holes (S*S, 0|255), u32 m_holeCount
                       + IsHole/SetHole/Holes/HoleCount/HasHoles/CellHasHole/BlockHasHole
                       + CutHoles/FillHoles brush cores (hard edge, region result)
HeightfieldSource      dataVersion 1 -> 2; Build(heightBlob, holeBlob)
                       + kHoleStream = "holes" (always written)
HeightfieldAssetBuilder Version 2 -> 3; ScanDependencies declares "holes"
TerrainChunk           + bool hasHoles
foundation.terrain     + BuildHoledChunkIndices(hf, chunk, lod, out, outSurfaceCount)
engine.terrain         + HoledChunkMeshCache; TerrainRenderData.holedMeshes/holedMeshCount
                       ChunkCache keyed by (uid, version)
PhysicsSubsystem       fillHeightfield writes cNoCollisionValue for a hole sample
NavigationBake         CollectNavigationGeometry skips holed stride blocks
Heightfield::QueryRay  skips crossings in a hole cell
vegetation             PlacementShareAt / ScatterStamp reject a hole cell
Editor.Terrain         + TerrainHoleTool ("terrain.hole"), HoleStrokeCommand, HolePanelProvider
Editor.Heightfield     page overlay for hole samples
```

Nothing is added to `TerrainComponent`, `TerrainAsset`, `TerrainSource` or `SplatWeights`:
holes belong to the heightfield asset, the thing that IS the surface.

## Phases

- **P0 - the field and every consumer, headless** (Foundation + Engine, no editor): the plane,
  the stream and the version bumps; `BuildHoledChunkIndices` + the mesh cache + the render
  data path; physics, nav, raycast, vegetation. Acceptance: a cooked heightfield with a cut
  square draws a hole (pixel probe on Vulkan and WebGPU: the clear colour shows through, the
  shadow map has the hole), a ray misses through it and hits beside it, the nav bake has a gap,
  grass does not grow in it, and every existing terrain re-cooks unchanged apart from the new
  all-zero stream.
- **P1 - the brush**: tool, panel, command, persist, page overlay, `InvalidateRegion` for
  vegetation. Acceptance: a headless stroke through `ViewportToolInput` cuts, undoes and
  redoes a region; the asset instance's `"holes"` stream round-trips; the status names the
  mode.
- **P2 - the rim** (BUILT 2026-09-23, user ask after the playground's stair-stepped edge): a
  holed chunk draws the SUPERSET of quads (`BuildHoledChunkIndices(..., dropWhenAnyCut =
  false)`: a quad drops only when every sample in its stride block is cut; collision, nav and
  raycast keep the strict rule) and its three pixel shaders discard by a bilinear R8 mask of
  the hole plane (`TerrainHoleTextureCache`, `TerrainRenderData::holeView`, sampled at the
  sample centres with `HoleCoverage > 0.5`). The rim is then the 0.5 iso-line of the mask at
  every LOD, one smooth curve instead of a per-sample stair, and a coarse level no longer
  drops a 64 x 64-cell quad for one cut sample. Mechanism: `ShaderFlags::Holes` -> `HOLES`
  variants of terrain.vs/ps, terrain_depth.ps (a fragment stage the depth pass gains under HOLES
  only; its vertex stage is the colour pass's `terrain` module for BOTH the prepass and the
  cascades, so it declares terrain.vs's full VSOut under `// preserve-interface` - see
  Systems/shaders.md) and terrain_pick.vs/ps; the renderer keeps a second colour / depth / pick PSO
  and a `(height, mask, sampler)` bind group per holed heightfield, chosen per chunk by
  `hasHoles`. Solid chunks and solid terrains are untouched (no mask, no fragment stage, no
  discard). Trade: the visible rim sits up to half a cell INSIDE the collision hole, so a
  character can stand on that half-cell of air at the edge; the guide says so.
- **P2 seeds** (not scheduled): import holes from a heightfield image's alpha or a sibling
  `_holes.png`; a "fill all" panel action; a hole-aware CDLOD morph when morphing lands.

## Files to touch (P0 + P1)

`Foundation/Heightfield/Heightfield.cppm` (+ tests), `Foundation/Heightfield.Resource/
HeightfieldResource.cppm` (+ tests), `Pipeline/Heightfield.Pipeline/HeightfieldAsset.cppm`
(+ tests), `Foundation/Terrain/Terrain.cppm` (+ tests), `Engine/Engine.Terrain/
{TerrainComponents.cppm, TerrainRenderData.cppm, TerrainRenderer.cppm, HoledChunkMesh.cppm
(new)}` (+ component / renderer / backend pixel-probe tests), `Engine/Engine.Physics/
PhysicsSubsystem.cppm` (+ `Foundation/Physics.Tests`), `Editor/Editor.Navigation/
NavigationBakeImpl.cpp` (+ tests), `Foundation/Vegetation/ScatterImpl.cpp` (+ tests),
`Editor/Editor.Terrain/{HoleTool.cppm, HoleToolImpl.cpp (new), ToolPanelsImpl.cpp,
SculptImpl.cpp (provider registration)}` (+ tests), `Editor/Editor.Heightfield/
HeightfieldEditorPage*` (overlay), `Documentation/Guides/terrain-authoring.md` (a "Holes"
section), the weekly.

## Gotchas

- **Strict versioning bites twice**: the source dataVersion AND the builder version bump
  together, and every heightfield in every project re-cooks; the RaptorUAT terrain is one.
  Say so in the commit and the weekly.
- **Chunk edges**: the skirt decision and the quad decision both read shared edge samples, so
  two neighbouring chunks never disagree about a rim; the test cuts a hole exactly on a chunk
  boundary and checks both chunks' index sets.
- **`Index()` clamps**: a brush disc past the grid edge marks edge samples, never wraps; the
  test paints at a corner.
- **LOD blocks**: the "any sample in the block" rule means a one-sample hole removes a whole
  LOD-6 quad (64 x 64 cells) at distance from collision and nav. Since P2 the DRAW keeps that
  quad and the mask cuts the one sample out of it, so the picture no longer grows at distance;
  still cut holes a few samples wide so the mask's 0.5 iso-line has something to trace.
- **Jolt block size**: nothing changes - Jolt pads with the same sentinel we now write;
  no hand-padding, as the physics rule already says.
- **Vegetation regrow under the brush**: `InvalidateRegion` with the stroke's region, else the
  whole layer regrows per dab (the flicker lesson of 2026-09-22 is already fixed, the cost is
  not).
- **The texture is unchanged**: the height texture keeps its R16 samples for hole samples too
  (chunk bounds and the skirt height read them). Do not zero heights under a hole; the brush
  can fill it back.

## Tests (the spec's contract)

Heightfield: default plane zero, `HasHoles` false; `CutHoles` marks a disc with a hard edge and
returns the region; `FillHoles` clears it; `CellHasHole`/`BlockHasHole` semantics at edges
and corners; `QueryRay` misses through a cut and hits beside it; source round-trip with both
streams; a wrong-size hole blob is refused. Terrain: `BuildHoledChunkIndices` at every LOD
against a mask with a hole on a chunk boundary (both chunks drop the shared skirt segment;
surface prefix counts match a hand count; a fully cut chunk yields no surface). Engine:
render data carries the holed meshes for exactly the holed chunks; a chunk with no holes
never allocates; pixel probe (Vulkan + WebGPU) shows the clear colour through the cut and the
shadow-map hole, and (P2) the same cut with the mask unbound draws the rim ring back; physics ray test; nav bake gap; scatter rejects the hole cell and the set
regrows through `InvalidateRegion`. Editor: a scripted stroke cuts / undoes / redoes one
command; persist writes the `"holes"` stream; the panel state and status text; the page
overlay marks the cut samples.
