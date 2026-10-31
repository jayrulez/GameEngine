// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Geometry - implementation unit for the mesh optimization pass.
//
// meshoptimizer is included here only, so the module interface stays clean of the vendored
// header. The pass:
//   1. per triangle submesh: meshopt_optimizeVertexCache then meshopt_optimizeOverdraw
//      (in place on that submesh's index range; ranges themselves never move);
//   2. whole mesh: meshopt_optimizeVertexFetchRemap, then remap the vertex blob (compacting
//      vertices no index references) and rewrite every index (non-triangle ranges too, so
//      shared vertices stay coherent).
// Everything is a pure reorder: triangle set, winding, submesh ranges and vertex values are
// unchanged, so rendering output is identical and fetch and cache behaviour is better.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

#include <meshoptimizer.h>

module geometry.pipeline;

import foundation.core;
import foundation.geometry;
import foundation.geometry.resource;

using namespace foundation::core;
using namespace foundation::geometry;

namespace pipeline
{
    namespace
    {
        // GPU-cache ACMR over every triangle range (the number the cook log and tests
        // watch; a 16-entry FIFO is meshoptimizer's recommended modern-GPU model).
        [[nodiscard]] f32 TriangleAcmr(const StaticMeshSource& source, usize vertexCount)
        {
            f32 weighted = 0.0f;
            usize triangles = 0;
            for (usize i = 0; i < source.subStart.Size(); ++i)
            {
                const u8 prim = (i < source.subPrim.Size()) ? source.subPrim[i] : 0;
                const i32 count = source.subCount[i];
                if (static_cast<PrimitiveType>(prim) != PrimitiveType::Triangles || count < 3)
                {
                    continue;
                }
                const meshopt_VertexCacheStatistics stats = meshopt_analyzeVertexCache(
                    source.indexData.Data() + source.subStart[i], static_cast<usize>(count),
                    vertexCount, 16, 0, 0);
                weighted += stats.acmr * static_cast<f32>(count / 3);
                triangles += static_cast<usize>(count / 3);
            }
            return (triangles > 0) ? weighted / static_cast<f32>(triangles) : 0.0f;
        }
    }

    u32 GenerateLodChain(StaticMeshSource& source, const LodGenerationSettings& settings)
    {
        constexpr usize kStride = sizeof(StaticMeshVertex);
        const usize vertexCount = source.vertexBlob.Size() / kStride;
        if (vertexCount == 0 || source.indexData.IsEmpty() || source.subStart.IsEmpty() ||
            source.lodCount > 1) // authored chains win; never regenerate over one
        {
            return 0;
        }
        // Validate (same refusal contract as the optimizer: bad assets pass through).
        for (usize i = 0; i < source.subStart.Size(); ++i)
        {
            const i64 start = source.subStart[i];
            const i64 count = (i < source.subCount.Size()) ? source.subCount[i] : -1;
            if (start < 0 || count < 0 ||
                start + count > static_cast<i64>(source.indexData.Size()))
            {
                return 0;
            }
        }
        for (const u32 index : source.indexData)
        {
            if (index >= vertexCount)
            {
                return 0;
            }
        }

        const auto* positions = reinterpret_cast<const float*>(source.vertexBlob.Data());
        constexpr u32 kMaxGeneratedLevels = 3; // the ladder {0.5, 0.25, 0.125}: 3 halvings
        const usize submeshCount = source.subStart.Size();
        u32 levelsAdded = 0;
        // Each level simplifies from the previous level's indices (a progressive chain is
        // cheaper and more coherent than always re-simplifying LOD 0). Per submesh, border
        // locked so cross-submesh seams cannot crack apart.
        Array<Array<u32>> previous; // per submesh: the level we simplify from
        previous.Resize(submeshCount);
        for (usize s = 0; s < submeshCount; ++s)
        {
            if (static_cast<PrimitiveType>((s < source.subPrim.Size()) ? source.subPrim[s] : 0) !=
                PrimitiveType::Triangles)
            {
                continue; // non-triangle submeshes never simplify; their level range repeats LOD 0
            }
            for (i32 i = 0; i < source.subCount[s]; ++i)
            {
                previous[s].PushBack(source.indexData[source.subStart[s] + i]);
            }
        }

        for (u32 levelIndex = 0; levelIndex < kMaxGeneratedLevels; ++levelIndex)
        {
            // Simplify every triangle submesh for this level first; commit the level only if
            // every submesh got acceptably close to its target (a chain is per-mesh, so a
            // level either exists for all submeshes or not at all).
            Array<Array<u32>> level;
            level.Resize(submeshCount);
            bool acceptable = true;
            usize levelTriangles = 0;
            for (usize s = 0; s < submeshCount && acceptable; ++s)
            {
                if (previous[s].IsEmpty())
                {
                    continue; // non-triangle submesh: empty here, range repeats LOD 0 below
                }
                const usize sourceCount = previous[s].Size();
                const usize target = ((sourceCount / 3) / 2) * 3; // half the triangles, aligned
                if (target < 3)
                {
                    acceptable = false;
                    break;
                }
                level[s].Resize(sourceCount); // simplify writes at most sourceCount indices
                f32 resultError = 0.0f;
                const usize written = meshopt_simplify(
                    level[s].Data(), previous[s].Data(), sourceCount, positions, vertexCount,
                    kStride, target, settings.targetError, meshopt_SimplifyLockBorder,
                    &resultError);
                level[s].Resize(written);
                // Accept when the simplifier got near the halving target within the error
                // bound; stalling (barely below the source count) means quality is exhausted.
                if (written == 0 || written > (sourceCount * 3) / 4)
                {
                    acceptable = false;
                    break;
                }
                levelTriangles += written / 3;
            }
            if (!acceptable || levelTriangles < settings.minTriangles)
            {
                break; // the chain is as long as quality allows, never padded
            }
            // Commit: append each submesh's range (non-triangle submeshes repeat their
            // LOD-0 range so every level keeps the full submesh table).
            if (source.lodCount <= 1)
            {
                source.lodCount = 1;
                source.lodCoverage.Clear();
                source.lodCoverage.PushBack(1.0f);
            }
            for (usize s = 0; s < submeshCount; ++s)
            {
                if (level[s].IsEmpty())
                {
                    source.lodStart.PushBack(source.subStart[s]);
                    source.lodIndexCount.PushBack(source.subCount[s]);
                    continue;
                }
                source.lodStart.PushBack(static_cast<i32>(source.indexData.Size()));
                source.lodIndexCount.PushBack(static_cast<i32>(level[s].Size()));
                for (const u32 index : level[s])
                {
                    source.indexData.PushBack(index);
                }
            }
            source.lodCoverage.PushBack(0.25f * Pow(0.5f, static_cast<f32>(source.lodCount - 1)));
            source.lodCount += 1;
            ++levelsAdded;
            previous = Move(level); // next level simplifies from this one
        }
        if (levelsAdded > 0)
        {
            LOG_INFO(u8"Cook", u8"mesh '{}': generated {} LOD level(s)", source.name,
                     levelsAdded);
        }
        return levelsAdded;
    }

    void OptimizeStaticMeshSource(StaticMeshSource& source, MeshOptimizeStats* outStats)
    {
        constexpr usize kStride = sizeof(StaticMeshVertex);
        const usize vertexCount = source.vertexBlob.Size() / kStride;
        MeshOptimizeStats stats;
        stats.verticesBefore = static_cast<u32>(vertexCount);
        stats.verticesAfter = static_cast<u32>(vertexCount);
        if (vertexCount == 0 || source.indexData.IsEmpty() || source.subStart.IsEmpty())
        {
            if (outStats != nullptr)
            {
                *outStats = stats;
            }
            return;
        }
        // Malformed ranges never reach the optimizer: validate every submesh window and
        // every index before touching anything (a bad asset cooks as-is, with a warning).
        for (usize i = 0; i < source.subStart.Size(); ++i)
        {
            const i64 start = source.subStart[i];
            const i64 count = (i < source.subCount.Size()) ? source.subCount[i] : -1;
            if (start < 0 || count < 0 ||
                start + count > static_cast<i64>(source.indexData.Size()))
            {
                LOG_WARNING(u8"Cook", u8"mesh '{}': submesh {} range invalid - skipping optimization",
                         source.name, i);
                if (outStats != nullptr)
                {
                    *outStats = stats;
                }
                return;
            }
        }
        for (usize i = 0; i < source.lodStart.Size(); ++i)
        {
            const i64 start = source.lodStart[i];
            const i64 count = (i < source.lodIndexCount.Size()) ? source.lodIndexCount[i] : -1;
            if (start < 0 || count < 0 ||
                start + count > static_cast<i64>(source.indexData.Size()))
            {
                LOG_WARNING(u8"Cook",
                            u8"mesh '{}': LOD range {} invalid - skipping optimization",
                            source.name, i);
                if (outStats != nullptr)
                {
                    *outStats = stats;
                }
                return;
            }
        }
        for (const u32 index : source.indexData)
        {
            if (index >= vertexCount)
            {
                LOG_WARNING(u8"Cook",
                         u8"mesh '{}': index {} out of range ({} vertices) - skipping optimization",
                         source.name, index, vertexCount);
                if (outStats != nullptr)
                {
                    *outStats = stats;
                }
                return;
            }
        }

        // A skinned source whose parallel stream does not match the vertex count is
        // malformed; refusing beats permuting one stream and not the other.
        if (const auto* skinnedCheck = Cast<SkinnedMeshSource>(&source))
        {
            if (skinnedCheck->skinningBlob.Size() != vertexCount * sizeof(VertexSkinning))
            {
                LOG_WARNING(u8"Cook",
                            u8"mesh '{}': skinning stream size mismatch - skipping optimization",
                            source.name);
                if (outStats != nullptr)
                {
                    *outStats = stats;
                }
                return;
            }
        }

        stats.acmrBefore = TriangleAcmr(source, vertexCount);

        // Steps 1 and 2: per-triangle-submesh reorder. Positions sit at offset 0 of the
        // 52-byte vertex; 1.05 is meshoptimizer's recommended overdraw and cache balance.
        const auto* positions = reinterpret_cast<const float*>(source.vertexBlob.Data());
        for (usize i = 0; i < source.subStart.Size(); ++i)
        {
            const u8 prim = (i < source.subPrim.Size()) ? source.subPrim[i] : 0;
            const i32 count = source.subCount[i];
            if (static_cast<PrimitiveType>(prim) != PrimitiveType::Triangles || count < 3 ||
                (count % 3) != 0)
            {
                continue;
            }
            u32* range = source.indexData.Data() + source.subStart[i];
            meshopt_optimizeVertexCache(range, range, static_cast<usize>(count), vertexCount);
            meshopt_optimizeOverdraw(range, range, static_cast<usize>(count), positions,
                                     vertexCount, kStride, 1.05f);
            ++stats.triangleSubmeshes;
        }
        // LOD ranges get the same reorder: each level is its own triangle
        // range whose primitive type mirrors its LOD-0 submesh.
        const usize submeshCount = source.subStart.Size();
        for (usize i = 0; i < source.lodStart.Size(); ++i)
        {
            const usize submesh = (submeshCount > 0) ? (i % submeshCount) : 0;
            const u8 prim = (submesh < source.subPrim.Size()) ? source.subPrim[submesh] : 0;
            const i32 count = source.lodIndexCount[i];
            if (static_cast<PrimitiveType>(prim) != PrimitiveType::Triangles || count < 3 ||
                (count % 3) != 0)
            {
                continue;
            }
            u32* range = source.indexData.Data() + source.lodStart[i];
            meshopt_optimizeVertexCache(range, range, static_cast<usize>(count), vertexCount);
            meshopt_optimizeOverdraw(range, range, static_cast<usize>(count), positions,
                                     vertexCount, kStride, 1.05f);
        }

        // 3. Whole-mesh vertex-fetch remap: blob reordered to index order, unused
        // vertices compacted away, every index rewritten through the remap. A skinned
        // source's parallel skinning stream is per-vertex data and gets the identical
        // permutation, so the streams cannot diverge.
        Array<u32> remap;
        remap.Resize(vertexCount);
        const usize uniqueCount = meshopt_optimizeVertexFetchRemap(
            remap.Data(), source.indexData.Data(), source.indexData.Size(), vertexCount);
        Array<u8> remappedBlob;
        remappedBlob.Resize(uniqueCount * kStride);
        meshopt_remapVertexBuffer(remappedBlob.Data(), source.vertexBlob.Data(), vertexCount,
                                  kStride, remap.Data());
        if (auto* skinned = Cast<SkinnedMeshSource>(&source))
        {
            constexpr usize kSkinStride = sizeof(VertexSkinning);
            if (skinned->skinningBlob.Size() == vertexCount * kSkinStride)
            {
                Array<u8> remappedSkin;
                remappedSkin.Resize(uniqueCount * kSkinStride);
                meshopt_remapVertexBuffer(remappedSkin.Data(), skinned->skinningBlob.Data(),
                                          vertexCount, kSkinStride, remap.Data());
                skinned->skinningBlob = Move(remappedSkin);
            }
        }
        meshopt_remapIndexBuffer(source.indexData.Data(), source.indexData.Data(),
                                 source.indexData.Size(), remap.Data());
        source.vertexBlob = Move(remappedBlob);
        stats.verticesAfter = static_cast<u32>(uniqueCount);

        stats.acmrAfter = TriangleAcmr(source, uniqueCount);
        LOG_INFO(u8"Cook",
                 u8"mesh '{}': optimized {} triangle submesh(es), ACMR {} -> {}, "
                 u8"vertices {} -> {}",
                 source.name, stats.triangleSubmeshes, stats.acmrBefore, stats.acmrAfter,
                 stats.verticesBefore, stats.verticesAfter);
        if (outStats != nullptr)
        {
            *outStats = stats;
        }
    }
}
