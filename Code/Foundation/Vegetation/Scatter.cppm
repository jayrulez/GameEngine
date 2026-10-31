// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.vegetation:scatter: the deterministic per-chunk scatter and the distance fade.
//
// ScatterChunk is a pure function of (seed, chunk, heightfield, splat, layer): the same inputs give
// byte-identical instances frame to frame and on any machine. Candidates are uniform XZ points in
// the chunk's footprint, accepted by the placement source (rejection sampling against its share),
// the slope limit and the height window; each accepted point gets Y from the heightfield, a random
// yaw, a uniform scale, and optionally the surface-normal frame. The output order is the fade
// order: the generator emits a uniformly random sequence, so the first N entries are a uniform
// thinning of the whole set. Distance fade is therefore a draw-count prefix (FadePrefix), never a
// re-upload, and an instance's rank never changes, so a chunk thins per instance instead of
// popping as a whole. Transforms are terrain-local (the heightfield's frame, footprint centred on
// the origin); the terrain entity's world matrix places them.

module;
#include "Core/Prelude.h"

export module foundation.vegetation:scatter;

import foundation.core;
import foundation.heightfield;      // Heightfield, HeightfieldRegion
import foundation.terrain;          // TerrainChunk, kChunkQuads, ChunksPerSide
import foundation.terrain.resource;    // SplatWeights
import foundation.vegetation.resource; // VegetationMask
import :layer;

using namespace foundation::core;

export namespace foundation::vegetation
{
    namespace heightfield = foundation::heightfield;
    namespace terrain = foundation::terrain;

    // The candidate ceiling per chunk: a CPU bound per rebuild (density x area can be asked for
    // anything) that never shapes the picture. The cap on what a chunk holds is the layer's
    // maxInstancesPerChunk, counted against placed instances (a candidate the mask rejects
    // costs nothing, so a painted patch grows at the layer's full density).
    inline constexpr u32 kMaxCandidatesPerChunk = 1u << 20;

    // One scattered chunk: the instances in fade order and what the scatter had to do to fit.
    struct ScatterResult
    {
        Array<Float4x4> transforms;      // terrain-local, in fade order
        AABB localBounds = AABB::Empty(); // the chunk's terrain AABB grown by the mesh extent
        u32 candidateCount = 0;          // points tried (density x area; fewer when the cap filled)
        f32 effectiveDensity = 0.0f;     // the layer's density, or cap / area when the chunk filled
        bool densityClamped = false;     // true when the chunk holds maxInstancesPerChunk with candidates left
    };

    // The seed of one (layer, chunk) set: a hash over the owning entity's persistent id, the
    // layer's index and the chunk index, never a pointer or a frame counter. The renderer
    // keys its persistent instance buffer on the same value.
    [[nodiscard]] inline u64 ChunkSeed(const Guid& ownerId, u32 layerIndex, i32 chunkX,
                                       i32 chunkZ) noexcept
    {
        u64 h = HashBytes(&ownerId, sizeof(ownerId));
        h = HashBytes(&layerIndex, sizeof(layerIndex), h);
        h = HashBytes(&chunkX, sizeof(chunkX), h);
        h = HashBytes(&chunkZ, sizeof(chunkZ), h);
        return h == 0 ? 1 : h; // 0 is the "no set" key downstream
    }

    // Density multiplier at `distance` metres: 1 inside fadeStart, a smooth fall to 0 at fadeEnd,
    // 0 beyond. A degenerate window (fadeEnd <= fadeStart) is a hard cut at fadeEnd.
    [[nodiscard]] inline f32 DensityAtDistance(f32 distance, f32 fadeStart, f32 fadeEnd) noexcept
    {
        if (distance >= fadeEnd)
        {
            return 0.0f;
        }
        if (distance <= fadeStart || fadeEnd <= fadeStart)
        {
            return 1.0f;
        }
        const f32 t = (distance - fadeStart) / (fadeEnd - fadeStart); // 0..1 across the fade
        const f32 s = t * t * (3.0f - 2.0f * t);                     // smoothstep
        return 1.0f - s;
    }

    // The draw-count prefix of a `count`-instance set at density multiplier `density` (0..1).
    [[nodiscard]] inline u32 FadePrefix(u32 count, f32 density) noexcept
    {
        if (density <= 0.0f)
        {
            return 0;
        }
        if (density >= 1.0f)
        {
            return count;
        }
        const f32 n = static_cast<f32>(count) * density;
        const u32 prefix = static_cast<u32>(n + 0.5f);
        return prefix > count ? count : prefix;
    }

    // The placement source's share (0..1) at a terrain-local XZ point: 1 for Uniform, the splat
    // layer's painted weight for Splat (0 with no splat), the mask plane's density for Mask (0
    // with no mask), their product for SplatTimesMask. Both rasters span the
    // terrain footprint like the shader's splat uv (local / worldSize + 0.5).
    [[nodiscard]] f32 PlacementShareAt(const ScatterLayer& layer,
                                       const heightfield::Heightfield& heightfield,
                                       const terrain::SplatWeights* splat,
                                       const VegetationMask* mask, f32 localX,
                                       f32 localZ) noexcept;

    // Scatter one chunk. `meshLocalBounds` is the instanced mesh's own AABB (its extent grows the
    // chunk's bounds by the maximum scale); Empty() leaves the terrain bounds as they are.
    void ScatterChunk(u64 seed, const terrain::TerrainChunk& chunk,
                      const heightfield::Heightfield& heightfield,
                      const terrain::SplatWeights* splat, const VegetationMask* mask,
                      const ScatterLayer& layer, const AABB& meshLocalBounds,
                      ScatterResult& out);

    // ---- the prop scatter brush (prop layers) ---------------------------------------------
    //
    // A brush stamp places authored instances into a prop layer: `density` per square metre
    // over the disc of `radius` at terrain-local (`centreX`, `centreZ`), scaled by `amount`
    // (0..1). Each candidate takes a uniform point in the disc and the layer's rules (slope,
    // height, scale, alignment) like the procedural scatter, then two rejections: the spacing
    // rule (no instance within `spacing` x the mesh's radius x its scale of an existing or
    // already-placed instance, the bounds test against scattered props) and an optional
    // `blocked` query (the editor wires the physics world's ShapeOverlap, so nothing lands inside
    // an existing body). Deterministic for a seed: a scripted stroke places the same count.
    struct StampResult
    {
        u32 candidates = 0;   // points tried
        u32 placed = 0;       // instances appended to `out`
        u32 rejectedSpacing = 0;
        u32 rejectedBlocked = 0;
        u32 rejectedRules = 0; // slope / height
    };

    // `blocked(localPosition, worldRadius)` returns true where an instance may not go (null means
    // never). `existing` are the layer's current instances (terrain-local); `out` receives the
    // new ones (appended; the spacing test sees them too).
    using BlockedQuery = Function<bool(Float3 localPosition, f32 radius)>;

    StampResult ScatterStamp(u64 seed, const heightfield::Heightfield& heightfield,
                             const ScatterLayer& layer, const AABB& meshLocalBounds,
                             f32 centreX, f32 centreZ, f32 radius, f32 density, f32 amount,
                             f32 spacing, Span<const Float4x4> existing,
                             const BlockedQuery& blocked, Array<Float4x4>& out);

    // Remove every instance whose terrain-local XZ lies inside the disc; returns how many.
    u32 EraseInstancesInDisc(Array<Float4x4>& instances, f32 centreX, f32 centreZ,
                             f32 radius) noexcept;

    // The chunk indices (row-major, chunkZ * chunksPerSide + chunkX) whose grass a sculpt or
    // paint over `region` (sample-grid coordinates) can change. Chunks share edge samples, so a
    // region on a boundary sample touches both neighbours. Appends unique indices, ascending.
    void ChunksTouchedBy(const heightfield::HeightfieldRegion& region, i32 chunksPerSide,
                         Array<u32>& outChunkIndices);
}
