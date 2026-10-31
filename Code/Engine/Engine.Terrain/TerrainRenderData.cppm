// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Terrain - the `:renderdata` partition.
//
// The whole-terrain render item: one TerrainRenderData per terrain component per frame (the
// manager's extract emits it). The renderer culls and LODs its chunks per view at resolve time
// with foundation.terrain's ExtractVisibleChunkDraws over these fields, so the whole terrain draws
// as one snapshot item with per-view chunk selection.
//
// The splat is the top-K model: the index and weight texture pair (from the CPU SplatWeights), an
// explicit base albedo, the palette Texture2DArray, and the per-palette-layer tileScale buffer.
// With paletteCount == 0 or no weight pair the PS renders pure base; with no base either, the
// height-ramp fallback.

module;
#include "Core/Prelude.h"

export module engine.terrain:renderdata;

import foundation.core;
import foundation.render;
import foundation.rhi;
import foundation.terrain;

using namespace foundation::core;

export namespace engine::terrain
{
    namespace render = foundation::render;
    namespace rhi = foundation::rhi;

    /// The largest LOD-threshold table a terrain carries inline (kMaxChunkLod + 1 = 7 levels).
    inline constexpr u32 kMaxLodThresholds = 8;

    /// The index buffers of one holed chunk, per LOD: built on the CPU by BuildHoledChunkIndices
    /// when the heightfield's version changes, owned by the manager's TerrainHoledMeshCache, copied
    /// into the frame arena by value (the buffers outlive the frame through the retire queue). A
    /// chunk without holes draws the renderer's shared grid.
    struct HoledChunkMesh
    {
        u32 chunkIndex = 0;
        rhi::Buffer* indexBuffers[foundation::terrain::kMaxChunkLod + 1] = {};
        u32 indexCounts[foundation::terrain::kMaxChunkLod + 1] = {};
        u32 surfaceIndexCounts[foundation::terrain::kMaxChunkLod + 1] = {};
    };

    struct TerrainRenderData : render::RenderData
    {
        // Snapshot CPU model: the chunk grid and flattened quadtree nodes, copied into the
        // ExtractedScene's frame arena at extraction (AddArray). Never pointers into manager
        // storage: the snapshot is read at record time, after arbitrary scene mutations, and a
        // mid-frame cache rebuild would free what a borrowed pointer refers to.
        const foundation::terrain::TerrainChunk* chunks = nullptr;
        const foundation::terrain::TerrainQuadtree::Node* nodes = nullptr;
        u32 chunkCount = 0;
        u32 nodeCount = 0;

        // The R16Uint height texture (from TerrainHeightTextureCache); the VS/PS fetch it via Load.
        rhi::TextureView* heightView = nullptr;

        // Holed chunks' own index buffers (arena copy; one record per chunk with holes, in chunk
        // order). The renderer draws these for a chunk whose `hasHoles` is set and skips an
        // `allCut` chunk outright.
        const HoledChunkMesh* holedMeshes = nullptr;
        u32 holedMeshCount = 0;
        // The R8 hole mask (null = no holes): the HOLES pixel shaders sample it bilinearly to
        // shape a holed chunk's rim (TerrainHoleTextureCache).
        rhi::TextureView* holeView = nullptr;

        // Terrain placement and height mapping (local heightfield space -> world).
        Float4x4 chunkToWorld = Float4x4::Identity();
        i32 gridSize = 0;         // heightfield side S (texture is S x S)
        Float2 worldSizeXZ{0.0f, 0.0f};
        f32 minY = 0.0f;          // world Y at sample 0
        f32 maxY = 0.0f;          // world Y at sample 65535

        // Descending coverage thresholds (thresholds[0] = 1.0 by convention; level 0 = fallback).
        f32 thresholds[kMaxLodThresholds] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        u32 thresholdCount = 1;
        f32 lodBias = 0.0f; // per-component LOD bias (negative = finer, positive = coarser)

        // --- Top-K splat material (set 3) ------------------------------------------------------
        // The SplatWeights texture pair (weights RGBA8Unorm, indices RGBA8Uint, load-only: a
        // filtered index sample interpolates layer ids into garbage).
        rhi::TextureView* weightView = nullptr;
        rhi::TextureView* indexView = nullptr;
        // The base layer: shows wherever painted weights don't sum to 1. Null = dummy (white albedo /
        // flat normal / default ORM).
        rhi::TextureView* baseAlbedoView = nullptr;
        rhi::TextureView* baseNormalView = nullptr; // null = flat-normal dummy
        rhi::TextureView* baseOrmView = nullptr;    // null = default-ORM dummy
        rhi::TextureView* baseHeightView = nullptr; // null = mid-height dummy (height-blend)
        f32 baseTileScale = 1.0f;
        // The paint palette: a Texture2DArray (one slice per palette layer, cook-resized to a
        // common size) and the per-layer tileScale buffer (slot 0 = base, slot 1+i = palette i).
        // The normal and ORM arrays are null when no palette layer used that map (a 1x1 dummy binds).
        rhi::TextureView* paletteArrayView = nullptr;
        rhi::TextureView* normalArrayView = nullptr;
        rhi::TextureView* ormArrayView = nullptr;
        rhi::TextureView* heightArrayView = nullptr; // null = no palette layer used a height map
        rhi::TextureView* maskArrayView = nullptr;   // null = no palette layer used a coverage mask
        rhi::Buffer* tileScaleBuffer = nullptr;
        u64 tileScaleGeneration = 0; // part of the set-3 cache key (never raw pointers)
        u32 paletteCount = 0;
        // Height-blend soft-skirt width; the renderer packs it into the view UBO only when a base
        // or palette height map is present (otherwise the linear path runs).
        f32 heightBlendContrast = 0.25f;
    };
}
