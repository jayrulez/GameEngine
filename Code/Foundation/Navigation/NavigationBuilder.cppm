// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Navigation - :builder partition.
//
// NavigationMeshBuilder: the Recast bake. Triangle soup (positions and triangle indices, in
// whatever space the caller chose; the engine bakes in zone-local space) and an agent
// profile go in; a serialized tiled navmesh blob comes out. It is pure and deterministic:
// identical input and params produce byte-identical output (Recast has no threading or RNG in
// this path). Recast lives entirely in NavigationImpl.cpp; this interface is free of rc* types.

module;
#include "Core/Prelude.h"

export module foundation.navigation:builder;

import foundation.core;

using namespace foundation::core;

export namespace foundation::navigation
{
    // The agent profile the navmesh is baked for. Radius, height and climb are baked in, so a
    // navmesh is per profile, which the zone model relies on. Cell size and height set the voxel
    // resolution of the bake. The defaults are Recast's starting point for a human-sized agent.
    struct NavigationBakeParams
    {
        f32 cellSize = 0.3f;             // xz voxel size (world units)
        f32 cellHeight = 0.2f;           // y voxel size (world units)
        f32 agentRadius = 0.6f;          // eroded off walkable edges
        f32 agentHeight = 2.0f;          // headroom needed to stand
        f32 agentMaxClimb = 0.9f;        // step/ledge the agent walks up
        f32 agentMaxSlopeDegrees = 45.0f; // steeper faces are non-walkable
        // Tiled bake: cells per tile edge (tile world size = tileCells * cellSize).
        // 64 cells at 0.3 gives 19.2-unit tiles: small zones stay one tile, big ones split, and a
        // partial rebake regenerates one tile, not the zone.
        u32 tileCells = 64;
        // Bake tiles across worker threads. Tiles are independent and assembly stays row-major,
        // so the output is byte-identical either way; the toggle trades bake latency only.
        bool parallelBake = true;
        // The region to bake, in the input's space; the default (an empty box) is the input's own
        // extent. Geometry reaching past it (a ground plane as wide as a level) is clipped to it
        // rather than widening the grid: an editor zone passes its box.
        AABB bounds = AABB::Empty();
    };

    // A tiled blob's grid, for partial rebakes (ReadTiledBlobGrid / BuildTileInGrid).
    struct NavigationTileGridDesc
    {
        // XZ anchor the tiles align to (the original bake's geometry min corner). The
        // vertical range is always taken from the current geometry, since a rebake may add
        // taller or lower content without invalidating the grid.
        Float3 origin{0, 0, 0};
        f32 tileWorldSize = 0.0f;
        i32 countX = 0;
        i32 countY = 0;
    };

    // Opt-in bake-stage capture for diagnostics: the intermediate Recast data a failed or
    // surprising bake is debugged with, in zone-local space, ready for debug draw. The live
    // dtNavMesh draw stays the ground truth for what queries run on; the stages show how the
    // bake arrived there (where spans rasterized, where contours went).
    struct NavigationBakeStages
    {
        // Simplified region contours: line-segment pairs (i, i+1 per segment).
        Array<Float3> contourLines;
        // Walkable span tops from the rasterized heightfield, one point per cell (strided
        // and capped: diagnostics, not a point cloud export).
        Array<Float3> walkableSamples;
    };

    class NavigationMeshBuilder
    {
    public:
        // Bake `vertices` (contiguous positions) and `indices` (3 per triangle) into `outData`
        // (a self-describing header and the Detour navmesh tiles). Returns:
        //   Ok               a navmesh was produced (outData filled)
        //   InvalidArgument  empty or malformed input (index out of range, indices % 3 != 0)
        //   Internal         a Recast stage failed
        //   NotFound         the bake produced zero walkable polygons (degenerate geometry
        //                    or an agent profile that fits nowhere); outData is left empty
        // Deterministic: the same input and params give byte-identical outData.
        // This is BuildTiled without stage capture: there is one blob format, the tiled one.
        [[nodiscard]] static Status Build(Span<const Float3> vertices, Span<const u32> indices,
                                          const NavigationBakeParams& params, Array<byte>& outData);

        // Tiled bake: the geometry's bounds split into params.tileCells-sized tiles, each baked
        // independently (in row-major order, so the result is deterministic) into one
        // multi-tile navmesh blob. Empty tiles are absent. Same status contract as Build;
        // NotFound means no tile produced any walkable polygon.
        [[nodiscard]] static Status BuildTiled(Span<const Float3> vertices,
                                               Span<const u32> indices,
                                               const NavigationBakeParams& params,
                                               Array<byte>& outData,
                                               NavigationBakeStages* outStages = nullptr);

        // The per-tile regeneration primitive: bake one tile (tx, ty of the grid BuildTiled
        // would derive from these bounds) to raw Detour tile data. BuildTiled is this in a
        // loop, so a regenerated tile is byte-identical to the full bake's. NotFound means the
        // tile holds no walkable polygons (outData empty).
        [[nodiscard]] static Status BuildTileAt(Span<const Float3> vertices,
                                                Span<const u32> indices,
                                                const NavigationBakeParams& params, i32 tileX,
                                                i32 tileY, Array<byte>& outTileData);

        // Partial rebake: rebuild one tile against an explicit grid (the grid a previous
        // bake's blob recorded) instead of deriving it from the current geometry bounds.
        // Edited geometry may shift the bounds, and a patched tile has to stay on the original
        // grid or it lands in the wrong place. A patched blob is byte-identical to a full
        // rebake when the geometry's vertical envelope is unchanged (tile headers record it);
        // a grown envelope patches correctly but far tiles' header bytes differ.
        [[nodiscard]] static Status BuildTileInGrid(Span<const Float3> vertices,
                                                    Span<const u32> indices,
                                                    const NavigationBakeParams& params,
                                                    const NavigationTileGridDesc& grid,
                                                    i32 tileX, i32 tileY,
                                                    Array<byte>& outTileData);
    };

    /// The tile grid a v2 (tiled) blob was baked on. False for v1/invalid blobs.
    [[nodiscard]] bool ReadTiledBlobGrid(Span<const byte> blob, NavigationTileGridDesc& out);

    /// Replace tile (tileX, tileY)'s record in a v2 blob (insert when absent; empty tileData
    /// removes the record). Every other tile's bytes are untouched, so a patched blob equals
    /// the full rebake of the same edited geometry byte-for-byte. False for v1/invalid blobs.
    [[nodiscard]] bool PatchTiledNavMeshBlob(Array<byte>& blob, i32 tileX, i32 tileY,
                                             Span<const byte> tileData);
}
