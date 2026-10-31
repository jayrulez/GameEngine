// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Navigation - implementation unit.
//
// All Recast and Detour use lives here. The interface stays free of rc* and dt* types, both to
// keep the API clean and because GCC's module serializer cannot digest the third-party headers
// inside an interface unit's global fragment. Each tile is baked with Recast's standard pipeline;
// the runtime wrappers hold Detour objects behind PIMPL.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourCrowd.h>

#include <cmath>
#include <cstring>

module foundation.navigation;

import foundation.core;
import foundation.profiler;

using namespace foundation::core;

namespace foundation::navigation
{
    namespace
    {
        // Serialized-blob header. The tiled info block and tile records follow immediately after.
        constexpr u32 kBlobMagic = 0x564E4144u; // 'DANV' little-endian

        struct BlobHeader
        {
            u32 magic;
            u32 version;
            f32 agentRadius;
            f32 agentHeight;
            u32 navDataSize;
        };

        // Detour poly flag for the one walkable class we bake. Non-zero so the default query
        // filter (include 0xffff) accepts it.
        constexpr unsigned short kPolyFlagWalk = 0x01;

        // The blob format: tiled (version 2; a single-tile version 1 blob is refused). The
        // header's navDataSize covers this info block and the tile records.
        constexpr u32 kBlobVersionTiled = 2u;
        struct TiledBlobInfo
        {
            f32 origin[3];     // the tile grid origin (geometry min corner)
            f32 tileWorldSize; // tileCells * cellSize
            i32 tileCountX;
            i32 tileCountY;
            u32 tileCount; // records that follow (empty tiles are absent)
        };
        struct TileRecord
        {
            i32 tileX;
            i32 tileY;
            u32 dataSize;
        };

        // The tile grid BuildTiled and BuildTileAt derive from the geometry bounds, shared so
        // the per-tile primitive regenerates exactly the tile the full bake would.
        struct TileGrid
        {
            f32 bmin[3];
            f32 bmax[3];
            f32 tileWorldSize = 0.0f;
            i32 countX = 0;
            i32 countY = 0;
        };

        [[nodiscard]] bool ComputeTileGrid(Span<const Float3> vertices,
                                           const NavigationBakeParams& params, TileGrid& out)
        {
            if (vertices.IsEmpty() || params.tileCells == 0)
            {
                return false;
            }
            const float* verts = reinterpret_cast<const float*>(vertices.Data());
            for (int c = 0; c < 3; ++c)
            {
                out.bmin[c] = verts[c];
                out.bmax[c] = verts[c];
            }
            for (usize i = 1; i < vertices.Size(); ++i)
            {
                for (int c = 0; c < 3; ++c)
                {
                    out.bmin[c] = rcMin(out.bmin[c], verts[i * 3 + c]);
                    out.bmax[c] = rcMax(out.bmax[c], verts[i * 3 + c]);
                }
            }
            if (params.bounds.IsValid())
            {
                const f32 lo[3] = {params.bounds.min.x, params.bounds.min.y, params.bounds.min.z};
                const f32 hi[3] = {params.bounds.max.x, params.bounds.max.y, params.bounds.max.z};
                for (int c = 0; c < 3; ++c)
                {
                    out.bmin[c] = rcMax(out.bmin[c], lo[c]);
                    out.bmax[c] = rcMin(out.bmax[c], hi[c]);
                    if (out.bmin[c] > out.bmax[c])
                    {
                        return false; // the geometry is all outside the region
                    }
                }
            }
            out.tileWorldSize = static_cast<f32>(params.tileCells) * params.cellSize;
            out.countX = static_cast<i32>(
                std::ceil((out.bmax[0] - out.bmin[0]) / out.tileWorldSize));
            out.countY = static_cast<i32>(
                std::ceil((out.bmax[2] - out.bmin[2]) / out.tileWorldSize));
            out.countX = out.countX < 1 ? 1 : out.countX;
            out.countY = out.countY < 1 ? 1 : out.countY;
            return true;
        }
    }

    // ---------------------------------------------------------------------------------------
    // Bake (Recast)
    // ---------------------------------------------------------------------------------------

    Status NavigationMeshBuilder::Build(Span<const Float3> vertices, Span<const u32> indices,
                                        const NavigationBakeParams& params, Array<byte>& outData)
    {
        return BuildTiled(vertices, indices, params, outData, nullptr); // one blob format: tiled
    }

    namespace
    {
        // The Recast pipeline for one tile of the grid (borders overlap neighbours so edge
        // polys stitch; the border region never emits polygons). Produces raw Detour tile
        // data, or NotFound (empty outNavData) when the tile has no walkable surface, which
        // is a normal result for tiles off the geometry.
        [[nodiscard]] Status BuildOneTile(Span<const Float3> vertices, Span<const u32> indices,
                                          const NavigationBakeParams& params,
                                          const TileGrid& grid, i32 tileX, i32 tileY,
                                          Array<byte>& outNavData,
                                          NavigationBakeStages* outStages = nullptr)
        {
            outNavData.Clear();
            if (tileX < 0 || tileY < 0 || tileX >= grid.countX || tileY >= grid.countY)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const int vertexCount = static_cast<int>(vertices.Size());
            const int triangleCount = static_cast<int>(indices.Size() / 3u);
            Array<int> tris;
            tris.Resize(indices.Size());
            for (usize i = 0; i < indices.Size(); ++i)
            {
                if (indices[i] >= vertices.Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                tris[i] = static_cast<int>(indices[i]);
            }
            const float* verts = reinterpret_cast<const float*>(vertices.Data());

            rcConfig cfg;
            std::memset(&cfg, 0, sizeof(cfg));
            cfg.cs = params.cellSize;
            cfg.ch = params.cellHeight;
            cfg.walkableSlopeAngle = params.agentMaxSlopeDegrees;
            // In cells, a setting that is a whole number of them counting as exactly that many:
            // 0.45 / 0.15 is 2.9999998 in floats, and a bare floor would make a 0.45 m climb 0.3 m.
            constexpr float kWhole = 1e-4f;
            cfg.walkableHeight = static_cast<int>(std::ceil(params.agentHeight / cfg.ch - kWhole));
            cfg.walkableClimb = static_cast<int>(std::floor(params.agentMaxClimb / cfg.ch + kWhole));
            cfg.walkableRadius = static_cast<int>(std::ceil(params.agentRadius / cfg.cs - kWhole));
            cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
            cfg.maxSimplificationError = 1.3f;
            cfg.minRegionArea = static_cast<int>(rcSqr(8));
            cfg.mergeRegionArea = static_cast<int>(rcSqr(20));
            cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
            cfg.detailSampleDist = cfg.cs * 6.0f;
            cfg.detailSampleMaxError = cfg.ch * 1.0f;
            cfg.tileSize = static_cast<int>(params.tileCells);
            cfg.borderSize = cfg.walkableRadius + 3; // the Recast tile-sample expansion
            cfg.width = cfg.tileSize + cfg.borderSize * 2;
            cfg.height = cfg.tileSize + cfg.borderSize * 2;
            // This tile's bounds on the grid, expanded by the border so neighbours overlap.
            cfg.bmin[0] = grid.bmin[0] + static_cast<f32>(tileX) * grid.tileWorldSize;
            cfg.bmin[1] = grid.bmin[1];
            cfg.bmin[2] = grid.bmin[2] + static_cast<f32>(tileY) * grid.tileWorldSize;
            cfg.bmax[0] = cfg.bmin[0] + grid.tileWorldSize;
            cfg.bmax[1] = grid.bmax[1];
            cfg.bmax[2] = cfg.bmin[2] + grid.tileWorldSize;
            cfg.bmin[0] -= static_cast<f32>(cfg.borderSize) * cfg.cs;
            cfg.bmin[2] -= static_cast<f32>(cfg.borderSize) * cfg.cs;
            cfg.bmax[0] += static_cast<f32>(cfg.borderSize) * cfg.cs;
            cfg.bmax[2] += static_cast<f32>(cfg.borderSize) * cfg.cs;

            rcContext ctx(false);
            rcHeightfield* solid = nullptr;
            rcCompactHeightfield* chf = nullptr;
            rcContourSet* cset = nullptr;
            rcPolyMesh* pmesh = nullptr;
            rcPolyMeshDetail* dmesh = nullptr;

            auto run = [&]() -> Status
            {
                solid = rcAllocHeightfield();
                if (solid == nullptr ||
                    !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin,
                                         cfg.bmax, cfg.cs, cfg.ch))
                {
                    return Status{ErrorCode::Internal};
                }
                Array<unsigned char> triAreas;
                triAreas.Resize(static_cast<usize>(triangleCount));
                std::memset(triAreas.Data(), 0, triAreas.Size());
                rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, vertexCount,
                                        tris.Data(), triangleCount, triAreas.Data());
                if (!rcRasterizeTriangles(&ctx, verts, vertexCount, tris.Data(),
                                          triAreas.Data(), triangleCount, *solid,
                                          cfg.walkableClimb))
                {
                    return Status{ErrorCode::Internal};
                }
                rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
                rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
                rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

                chf = rcAllocCompactHeightfield();
                if (chf == nullptr ||
                    !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb,
                                               *solid, *chf))
                {
                    return Status{ErrorCode::Internal};
                }
                if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
                {
                    return Status{ErrorCode::Internal};
                }
                if (!rcBuildDistanceField(&ctx, *chf) ||
                    !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea,
                                    cfg.mergeRegionArea))
                {
                    return Status{ErrorCode::Internal};
                }
                cset = rcAllocContourSet();
                if (cset == nullptr || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError,
                                                        cfg.maxEdgeLen, *cset))
                {
                    return Status{ErrorCode::Internal};
                }
                pmesh = rcAllocPolyMesh();
                if (pmesh == nullptr || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly,
                                                         *pmesh))
                {
                    return Status{ErrorCode::Internal};
                }
                dmesh = rcAllocPolyMeshDetail();
                if (dmesh == nullptr ||
                    !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist,
                                           cfg.detailSampleMaxError, *dmesh))
                {
                    return Status{ErrorCode::Internal};
                }
                if (outStages != nullptr)
                {
                    // Stage 1: walkable span tops (strided, capped), where geometry
                    // rasterized before any erosion removed it.
                    constexpr usize kMaxSamples = 60000;
                    constexpr int kStride = 2;
                    for (int y = 0; y < solid->height; y += kStride)
                    {
                        for (int x = 0; x < solid->width; x += kStride)
                        {
                            if (outStages->walkableSamples.Size() >= kMaxSamples)
                            {
                                break;
                            }
                            for (const rcSpan* span = solid->spans[x + y * solid->width];
                                 span != nullptr; span = span->next)
                            {
                                if (span->area == RC_NULL_AREA)
                                {
                                    continue;
                                }
                                outStages->walkableSamples.PushBack(Float3{
                                    cfg.bmin[0] + (static_cast<f32>(x) + 0.5f) * cfg.cs,
                                    cfg.bmin[1] + static_cast<f32>(span->smax) * cfg.ch,
                                    cfg.bmin[2] + (static_cast<f32>(y) + 0.5f) * cfg.cs});
                            }
                        }
                    }
                    // Stage 2: simplified contours, the region outlines polygons come from.
                    for (int c = 0; c < cset->nconts; ++c)
                    {
                        const rcContour& contour = cset->conts[c];
                        for (int v = 0; v < contour.nverts; ++v)
                        {
                            const int* a = &contour.verts[v * 4];
                            const int* b =
                                &contour.verts[((v + 1) % contour.nverts) * 4];
                            outStages->contourLines.PushBack(Float3{
                                cset->bmin[0] + static_cast<f32>(a[0]) * cset->cs,
                                cset->bmin[1] + static_cast<f32>(a[1]) * cset->ch,
                                cset->bmin[2] + static_cast<f32>(a[2]) * cset->cs});
                            outStages->contourLines.PushBack(Float3{
                                cset->bmin[0] + static_cast<f32>(b[0]) * cset->cs,
                                cset->bmin[1] + static_cast<f32>(b[1]) * cset->ch,
                                cset->bmin[2] + static_cast<f32>(b[2]) * cset->cs});
                        }
                    }
                }
                if (pmesh->npolys == 0)
                {
                    return Status{ErrorCode::NotFound}; // an empty tile, normal off the geometry
                }
                for (int i = 0; i < pmesh->npolys; ++i)
                {
                    if (pmesh->areas[i] == RC_WALKABLE_AREA)
                    {
                        pmesh->flags[i] = kPolyFlagWalk;
                    }
                }

                dtNavMeshCreateParams np;
                std::memset(&np, 0, sizeof(np));
                np.verts = pmesh->verts;
                np.vertCount = pmesh->nverts;
                np.polys = pmesh->polys;
                np.polyAreas = pmesh->areas;
                np.polyFlags = pmesh->flags;
                np.polyCount = pmesh->npolys;
                np.nvp = pmesh->nvp;
                np.detailMeshes = dmesh->meshes;
                np.detailVerts = dmesh->verts;
                np.detailVertsCount = dmesh->nverts;
                np.detailTris = dmesh->tris;
                np.detailTriCount = dmesh->ntris;
                np.walkableHeight = params.agentHeight;
                np.walkableRadius = params.agentRadius;
                np.walkableClimb = params.agentMaxClimb;
                np.tileX = tileX;
                np.tileY = tileY;
                np.tileLayer = 0;
                rcVcopy(np.bmin, pmesh->bmin);
                rcVcopy(np.bmax, pmesh->bmax);
                np.cs = cfg.cs;
                np.ch = cfg.ch;
                np.buildBvTree = true;

                unsigned char* navData = nullptr;
                int navDataSize = 0;
                if (!dtCreateNavMeshData(&np, &navData, &navDataSize) || navData == nullptr ||
                    navDataSize <= 0)
                {
                    return Status{ErrorCode::Internal};
                }
                outNavData.Resize(static_cast<usize>(navDataSize));
                std::memcpy(outNavData.Data(), navData, static_cast<usize>(navDataSize));
                dtFree(navData);
                return Status{};
            };

            const Status status = run();
            rcFreePolyMeshDetail(dmesh);
            rcFreePolyMesh(pmesh);
            rcFreeContourSet(cset);
            rcFreeCompactHeightfield(chf);
            rcFreeHeightField(solid);
            if (!status.IsOk())
            {
                outNavData.Clear();
            }
            return status;
        }
    }

    Status NavigationMeshBuilder::BuildTileAt(Span<const Float3> vertices,
                                              Span<const u32> indices,
                                              const NavigationBakeParams& params, i32 tileX,
                                              i32 tileY, Array<byte>& outTileData)
    {
        outTileData.Clear();
        if (vertices.IsEmpty() || indices.IsEmpty() || (indices.Size() % 3u) != 0u)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        TileGrid grid;
        if (!ComputeTileGrid(vertices, params, grid))
        {
            return Status{ErrorCode::InvalidArgument};
        }
        return BuildOneTile(vertices, indices, params, grid, tileX, tileY, outTileData);
    }

    Status NavigationMeshBuilder::BuildTileInGrid(Span<const Float3> vertices,
                                                  Span<const u32> indices,
                                                  const NavigationBakeParams& params,
                                                  const NavigationTileGridDesc& gridDesc,
                                                  i32 tileX, i32 tileY, Array<byte>& outTileData)
    {
        outTileData.Clear();
        if (vertices.IsEmpty() || indices.IsEmpty() || (indices.Size() % 3u) != 0u ||
            gridDesc.tileWorldSize <= 0.0f || gridDesc.countX <= 0 || gridDesc.countY <= 0)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        // XZ anchoring comes from the recorded grid, the vertical range from the current
        // geometry (a rebake may add taller or lower content without invalidating the grid).
        TileGrid grid;
        grid.bmin[0] = gridDesc.origin.x;
        grid.bmin[2] = gridDesc.origin.z;
        grid.tileWorldSize = gridDesc.tileWorldSize;
        grid.countX = gridDesc.countX;
        grid.countY = gridDesc.countY;
        const float* verts = reinterpret_cast<const float*>(vertices.Data());
        grid.bmin[1] = verts[1];
        grid.bmax[1] = verts[1];
        for (usize i = 1; i < vertices.Size(); ++i)
        {
            grid.bmin[1] = rcMin(grid.bmin[1], verts[i * 3 + 1]);
            grid.bmax[1] = rcMax(grid.bmax[1], verts[i * 3 + 1]);
        }
        grid.bmax[0] = grid.bmin[0] + static_cast<f32>(grid.countX) * grid.tileWorldSize;
        grid.bmax[2] = grid.bmin[2] + static_cast<f32>(grid.countY) * grid.tileWorldSize;
        return BuildOneTile(vertices, indices, params, grid, tileX, tileY, outTileData);
    }

    bool ReadTiledBlobGrid(Span<const byte> blob, NavigationTileGridDesc& out)
    {
        if (blob.Size() < sizeof(BlobHeader) + sizeof(TiledBlobInfo))
        {
            return false;
        }
        BlobHeader header;
        std::memcpy(&header, blob.Data(), sizeof(BlobHeader));
        if (header.magic != kBlobMagic || header.version != kBlobVersionTiled)
        {
            return false;
        }
        TiledBlobInfo info;
        std::memcpy(&info, blob.Data() + sizeof(BlobHeader), sizeof(TiledBlobInfo));
        out.origin = Float3{info.origin[0], info.origin[1], info.origin[2]};
        out.tileWorldSize = info.tileWorldSize;
        out.countX = info.tileCountX;
        out.countY = info.tileCountY;
        return out.tileWorldSize > 0.0f && out.countX > 0 && out.countY > 0;
    }

    bool PatchTiledNavMeshBlob(Array<byte>& blob, i32 tileX, i32 tileY,
                               Span<const byte> tileData)
    {
        if (blob.Size() < sizeof(BlobHeader) + sizeof(TiledBlobInfo))
        {
            return false;
        }
        BlobHeader header;
        std::memcpy(&header, blob.Data(), sizeof(BlobHeader));
        if (header.magic != kBlobMagic || header.version != kBlobVersionTiled)
        {
            return false;
        }
        TiledBlobInfo info;
        std::memcpy(&info, blob.Data() + sizeof(BlobHeader), sizeof(TiledBlobInfo));

        // Walk the records, copying every other tile verbatim; the target tile's record is
        // replaced (or omitted for empty tileData); a previously absent tile is inserted in
        // row-major position so a patched blob stays byte-identical to a full rebake.
        Array<byte> out;
        out.Reserve(blob.Size() + tileData.Size());
        const auto append = [&out](const void* bytes, usize size)
        {
            const byte* p = static_cast<const byte*>(bytes);
            for (usize i = 0; i < size; ++i)
            {
                out.PushBack(p[i]);
            }
        };
        const auto appendTarget = [&]()
        {
            if (tileData.IsEmpty())
            {
                return; // removal
            }
            TileRecord record;
            record.tileX = tileX;
            record.tileY = tileY;
            record.dataSize = static_cast<u32>(tileData.Size());
            append(&record, sizeof(TileRecord));
            append(tileData.Data(), tileData.Size());
        };
        const i64 targetOrder =
            static_cast<i64>(tileY) * static_cast<i64>(info.tileCountX) + tileX;

        const byte* cursor = blob.Data() + sizeof(BlobHeader) + sizeof(TiledBlobInfo);
        const byte* end = blob.Data() + blob.Size();
        u32 written = 0;
        bool placed = false;
        for (u32 i = 0; i < info.tileCount; ++i)
        {
            if (end - cursor < static_cast<isize>(sizeof(TileRecord)))
            {
                return false;
            }
            TileRecord record;
            std::memcpy(&record, cursor, sizeof(TileRecord));
            if (end - cursor <
                static_cast<isize>(sizeof(TileRecord) + record.dataSize))
            {
                return false;
            }
            const i64 order = static_cast<i64>(record.tileY) *
                                  static_cast<i64>(info.tileCountX) +
                              record.tileX;
            if (!placed && order >= targetOrder)
            {
                appendTarget();
                placed = true;
                if (!tileData.IsEmpty())
                {
                    ++written;
                }
                if (order == targetOrder)
                {
                    cursor += sizeof(TileRecord) + record.dataSize; // the replaced original
                    continue;
                }
            }
            append(cursor, sizeof(TileRecord) + record.dataSize);
            cursor += sizeof(TileRecord) + record.dataSize;
            ++written;
        }
        if (!placed)
        {
            appendTarget();
            if (!tileData.IsEmpty())
            {
                ++written;
            }
        }
        if (written == 0)
        {
            return false; // a blob with zero tiles would not load
        }

        info.tileCount = written;
        header.navDataSize =
            static_cast<u32>(sizeof(TiledBlobInfo) + out.Size());
        blob.Clear();
        blob.Reserve(sizeof(BlobHeader) + sizeof(TiledBlobInfo) + out.Size());
        const auto appendBlob = [&blob](const void* bytes, usize size)
        {
            const byte* p = static_cast<const byte*>(bytes);
            for (usize i = 0; i < size; ++i)
            {
                blob.PushBack(p[i]);
            }
        };
        appendBlob(&header, sizeof(BlobHeader));
        appendBlob(&info, sizeof(TiledBlobInfo));
        appendBlob(out.Data(), out.Size());
        return true;
    }

    Status NavigationMeshBuilder::BuildTiled(Span<const Float3> vertices,
                                             Span<const u32> indices,
                                             const NavigationBakeParams& params,
                                             Array<byte>& outData,
                                             NavigationBakeStages* outStages)
    {
        PROFILE_SCOPE("Navigation.BakeTiled");
        outData.Clear();
        if (vertices.IsEmpty() || indices.IsEmpty() || (indices.Size() % 3u) != 0u)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        TileGrid grid;
        if (!ComputeTileGrid(vertices, params, grid))
        {
            return Status{ErrorCode::InvalidArgument};
        }

        // Tiles are independent, so they may bake across workers; assembly stays row-major,
        // so the blob is byte-identical parallel or serial (the toggle trades latency only).
        // Stages capture per-tile and concatenate in the same order for the same reason.
        const u32 tileTotal = static_cast<u32>(grid.countX) * static_cast<u32>(grid.countY);
        Array<Array<byte>> tileResults;
        tileResults.Resize(tileTotal);
        Array<Status> tileStatuses;
        Array<NavigationBakeStages> tileStages;
        tileStatuses.Resize(tileTotal, Status{ErrorCode::NotFound});
        if (outStages != nullptr)
        {
            tileStages.Resize(tileTotal);
        }
        const auto bakeIndex = [&](u32 index)
        {
            const i32 tx = static_cast<i32>(index % static_cast<u32>(grid.countX));
            const i32 ty = static_cast<i32>(index / static_cast<u32>(grid.countX));
            tileStatuses[index] = BuildOneTile(
                vertices, indices, params, grid, tx, ty, tileResults[index],
                (outStages != nullptr) ? &tileStages[index] : nullptr);
        };
        if (params.parallelBake && tileTotal > 1)
        {
            JobSystem jobs(outData.Allocator()); // scoped pool serving the caller's blob
            jobs.ParallelFor(tileTotal, bakeIndex, 1);
        }
        else
        {
            for (u32 i = 0; i < tileTotal; ++i)
            {
                bakeIndex(i);
            }
        }

        Array<TileRecord> records;
        Array<Array<byte>> tiles;
        usize payloadBytes = 0;
        for (u32 index = 0; index < tileTotal; ++index) // row-major: the deterministic order
        {
            const Status tileStatus = tileStatuses[index];
            // Stages append for Ok and NotFound alike: an empty tile still rasterized spans,
            // and that emptiness is what the overlay is for debugging.
            if (outStages != nullptr &&
                (tileStatus.IsOk() || tileStatus.Code() == ErrorCode::NotFound))
            {
                for (const Float3& v : tileStages[index].contourLines)
                {
                    outStages->contourLines.PushBack(v);
                }
                for (const Float3& v : tileStages[index].walkableSamples)
                {
                    outStages->walkableSamples.PushBack(v);
                }
            }
            if (tileStatus.Code() == ErrorCode::NotFound)
            {
                continue; // empty tile
            }
            if (!tileStatus.IsOk())
            {
                return tileStatus;
            }
            TileRecord record;
            record.tileX = static_cast<i32>(index % static_cast<u32>(grid.countX));
            record.tileY = static_cast<i32>(index / static_cast<u32>(grid.countX));
            record.dataSize = static_cast<u32>(tileResults[index].Size());
            payloadBytes += sizeof(TileRecord) + tileResults[index].Size();
            records.PushBack(record);
            tiles.PushBack(static_cast<Array<byte>&&>(tileResults[index]));
        }
        if (records.IsEmpty())
        {
            return Status{ErrorCode::NotFound}; // nothing walkable anywhere
        }

        TiledBlobInfo info;
        info.origin[0] = grid.bmin[0];
        info.origin[1] = grid.bmin[1];
        info.origin[2] = grid.bmin[2];
        info.tileWorldSize = grid.tileWorldSize;
        info.tileCountX = grid.countX;
        info.tileCountY = grid.countY;
        info.tileCount = static_cast<u32>(records.Size());

        BlobHeader header;
        header.magic = kBlobMagic;
        header.version = kBlobVersionTiled;
        header.agentRadius = params.agentRadius;
        header.agentHeight = params.agentHeight;
        header.navDataSize = static_cast<u32>(sizeof(TiledBlobInfo) + payloadBytes);

        outData.Resize(sizeof(BlobHeader) + sizeof(TiledBlobInfo) + payloadBytes);
        usize cursor = 0;
        std::memcpy(outData.Data() + cursor, &header, sizeof(BlobHeader));
        cursor += sizeof(BlobHeader);
        std::memcpy(outData.Data() + cursor, &info, sizeof(TiledBlobInfo));
        cursor += sizeof(TiledBlobInfo);
        for (usize i = 0; i < records.Size(); ++i)
        {
            std::memcpy(outData.Data() + cursor, &records[i], sizeof(TileRecord));
            cursor += sizeof(TileRecord);
            std::memcpy(outData.Data() + cursor, tiles[i].Data(), tiles[i].Size());
            cursor += tiles[i].Size();
        }
        return Status{};
    }

    // ---------------------------------------------------------------------------------------
    // NavigationMesh
    // ---------------------------------------------------------------------------------------

    struct NavigationMesh::Impl
    {
        dtNavMesh* navMesh = nullptr;
        f32 agentRadius = 0.0f;
        f32 agentHeight = 0.0f;

        ~Impl()
        {
            if (navMesh != nullptr)
            {
                dtFreeNavMesh(navMesh); // frees the DT_TILE_FREE_DATA-owned tile too
            }
        }
    };

    NavigationMesh::NavigationMesh(IAllocator& allocator) : m_impl(MakeUnique<Impl>(allocator))
    {
    }
    NavigationMesh::~NavigationMesh() = default;
    NavigationMesh::NavigationMesh(NavigationMesh&&) noexcept = default;
    NavigationMesh& NavigationMesh::operator=(NavigationMesh&&) noexcept = default;

    Status NavigationMesh::Load(Span<const byte> data)
    {
        Impl& impl = *m_impl;
        if (impl.navMesh != nullptr)
        {
            dtFreeNavMesh(impl.navMesh);
            impl.navMesh = nullptr;
            impl.agentRadius = 0.0f;
            impl.agentHeight = 0.0f;
        }

        if (data.Size() < sizeof(BlobHeader))
        {
            return Status{ErrorCode::InvalidArgument};
        }
        BlobHeader header;
        std::memcpy(&header, data.Data(), sizeof(BlobHeader));
        if (header.magic != kBlobMagic || header.version != kBlobVersionTiled)
        {
            return Status{ErrorCode::InvalidArgument}; // one blob format: tiled (v2)
        }
        if (data.Size() != sizeof(BlobHeader) + static_cast<usize>(header.navDataSize))
        {
            return Status{ErrorCode::InvalidArgument};
        }

        // Tiled blob: init a tiled dtNavMesh over the recorded grid, then add each tile
        // (Detour places them by the x/y baked into the tile headers).
        const byte* cursor = data.Data() + sizeof(BlobHeader);
        const byte* end = data.Data() + data.Size();
        if (end - cursor < static_cast<isize>(sizeof(TiledBlobInfo)))
        {
            return Status{ErrorCode::InvalidArgument};
        }
        TiledBlobInfo info;
        std::memcpy(&info, cursor, sizeof(TiledBlobInfo));
        cursor += sizeof(TiledBlobInfo);
        if (info.tileCount == 0 || info.tileCountX <= 0 || info.tileCountY <= 0 ||
            info.tileWorldSize <= 0.0f)
        {
            return Status{ErrorCode::InvalidArgument};
        }

        dtNavMesh* mesh = dtAllocNavMesh();
        if (mesh == nullptr)
        {
            return Status{ErrorCode::OutOfMemory};
        }
        dtNavMeshParams meshParams;
        std::memset(&meshParams, 0, sizeof(meshParams));
        meshParams.orig[0] = info.origin[0];
        meshParams.orig[1] = info.origin[1];
        meshParams.orig[2] = info.origin[2];
        meshParams.tileWidth = info.tileWorldSize;
        meshParams.tileHeight = info.tileWorldSize;
        meshParams.maxTiles = info.tileCountX * info.tileCountY;
        meshParams.maxPolys = 1 << 14;
        if (dtStatusFailed(mesh->init(&meshParams)))
        {
            dtFreeNavMesh(mesh);
            return Status{ErrorCode::Internal};
        }
        for (u32 i = 0; i < info.tileCount; ++i)
        {
            if (end - cursor < static_cast<isize>(sizeof(TileRecord)))
            {
                dtFreeNavMesh(mesh);
                return Status{ErrorCode::InvalidArgument};
            }
            TileRecord record;
            std::memcpy(&record, cursor, sizeof(TileRecord));
            cursor += sizeof(TileRecord);
            if (record.dataSize == 0 ||
                end - cursor < static_cast<isize>(record.dataSize))
            {
                dtFreeNavMesh(mesh);
                return Status{ErrorCode::InvalidArgument};
            }
            unsigned char* tileBytes =
                static_cast<unsigned char*>(dtAlloc(record.dataSize, DT_ALLOC_PERM));
            if (tileBytes == nullptr)
            {
                dtFreeNavMesh(mesh);
                return Status{ErrorCode::OutOfMemory};
            }
            std::memcpy(tileBytes, cursor, record.dataSize);
            cursor += record.dataSize;
            const dtStatus added =
                mesh->addTile(tileBytes, static_cast<int>(record.dataSize),
                              DT_TILE_FREE_DATA, 0, nullptr);
            if (dtStatusFailed(added))
            {
                dtFree(tileBytes);
                dtFreeNavMesh(mesh);
                return Status{ErrorCode::Internal};
            }
        }
        impl.navMesh = mesh;
        impl.agentRadius = header.agentRadius;
        impl.agentHeight = header.agentHeight;
        return Status{};
    }

    Status NavigationMesh::ReplaceTile(i32 tileX, i32 tileY, Span<const byte> tileData)
    {
        dtNavMesh* mesh = m_impl->navMesh;
        if (mesh == nullptr)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        // Only a mesh whose grid can place arbitrary tiles (a multi-tile grid).
        if (mesh->getParams()->maxTiles <= 1)
        {
            return Status{ErrorCode::NotSupported};
        }
        const dtTileRef existing = mesh->getTileRefAt(tileX, tileY, 0);
        if (existing != 0)
        {
            if (dtStatusFailed(mesh->removeTile(existing, nullptr, nullptr)))
            {
                return Status{ErrorCode::Internal};
            }
        }
        if (tileData.IsEmpty())
        {
            return Status{}; // removal only
        }
        unsigned char* bytes =
            static_cast<unsigned char*>(dtAlloc(tileData.Size(), DT_ALLOC_PERM));
        if (bytes == nullptr)
        {
            return Status{ErrorCode::OutOfMemory};
        }
        std::memcpy(bytes, tileData.Data(), tileData.Size());
        const dtStatus added = mesh->addTile(bytes, static_cast<int>(tileData.Size()),
                                             DT_TILE_FREE_DATA, 0, nullptr);
        if (dtStatusFailed(added))
        {
            dtFree(bytes);
            return Status{ErrorCode::Internal};
        }
        return Status{};
    }

    bool NavigationMesh::IsValid() const noexcept { return m_impl->navMesh != nullptr; }

    void NavigationMesh::DebugTriangles(Array<Float3>& out) const
    {
        const dtNavMesh* mesh = m_impl->navMesh;
        if (mesh == nullptr)
        {
            return;
        }
        for (int t = 0; t < mesh->getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = mesh->getTile(t);
            if (tile == nullptr || tile->header == nullptr)
            {
                continue;
            }
            for (int i = 0; i < tile->header->polyCount; ++i)
            {
                const dtPoly* poly = &tile->polys[i];
                if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                {
                    continue; // off-mesh links are lines, not surface
                }
                const dtPolyDetail* detail = &tile->detailMeshes[i];
                for (int j = 0; j < detail->triCount; ++j)
                {
                    const unsigned char* tri = &tile->detailTris[(detail->triBase + j) * 4];
                    for (int k = 0; k < 3; ++k)
                    {
                        const float* v;
                        if (tri[k] < poly->vertCount)
                        {
                            v = &tile->verts[poly->verts[tri[k]] * 3];
                        }
                        else
                        {
                            v = &tile->detailVerts[(detail->vertBase + tri[k] - poly->vertCount) *
                                                   3];
                        }
                        out.PushBack(Float3{v[0], v[1], v[2]});
                    }
                }
            }
        }
    }

    f32 NavigationMesh::BakedAgentRadius() const noexcept { return m_impl->agentRadius; }
    f32 NavigationMesh::BakedAgentHeight() const noexcept { return m_impl->agentHeight; }
    void* NavigationMesh::NativeHandle() const noexcept { return m_impl->navMesh; }

    // ---------------------------------------------------------------------------------------
    // NavigationMeshQuery
    // ---------------------------------------------------------------------------------------

    namespace
    {
        constexpr int kMaxPathPolys = 256;
        constexpr int kMaxStraightPath = 256;
        // Search box half-size for snapping a world point onto the navmesh.
        constexpr float kQueryHalfExtents[3] = {2.0f, 4.0f, 2.0f};
    }

    struct NavigationMeshQuery::Impl
    {
        dtNavMeshQuery* query = nullptr;
        dtQueryFilter filter; // default: include 0xffff, unit area costs

        ~Impl()
        {
            if (query != nullptr)
            {
                dtFreeNavMeshQuery(query);
            }
        }
    };

    NavigationMeshQuery::NavigationMeshQuery(IAllocator& allocator, const NavigationMesh& mesh)
        : m_impl(MakeUnique<Impl>(allocator))
    {
        dtNavMesh* navMesh = static_cast<dtNavMesh*>(mesh.NativeHandle());
        if (navMesh == nullptr)
        {
            return;
        }
        dtNavMeshQuery* query = dtAllocNavMeshQuery();
        if (query == nullptr)
        {
            return;
        }
        if (dtStatusFailed(query->init(navMesh, 2048)))
        {
            dtFreeNavMeshQuery(query);
            return;
        }
        m_impl->query = query;
    }

    NavigationMeshQuery::~NavigationMeshQuery() = default;
    NavigationMeshQuery::NavigationMeshQuery(NavigationMeshQuery&&) noexcept = default;
    NavigationMeshQuery& NavigationMeshQuery::operator=(NavigationMeshQuery&&) noexcept = default;

    bool NavigationMeshQuery::IsValid() const noexcept { return m_impl->query != nullptr; }

    bool NavigationMeshQuery::FindNearestPoint(Float3 point, Float3& out) const
    {
        if (m_impl->query == nullptr)
        {
            return false;
        }
        const float p[3] = {point.x, point.y, point.z};
        dtPolyRef ref = 0;
        float nearest[3] = {0, 0, 0};
        const dtStatus st =
            m_impl->query->findNearestPoly(p, kQueryHalfExtents, &m_impl->filter, &ref, nearest);
        if (dtStatusFailed(st) || ref == 0)
        {
            return false;
        }
        out = Float3{nearest[0], nearest[1], nearest[2]};
        return true;
    }

    Status NavigationMeshQuery::FindPath(Float3 start, Float3 end, NavigationPath& out) const
    {
        out.corners.Clear();
        out.complete = false;
        if (m_impl->query == nullptr)
        {
            return Status{ErrorCode::Internal};
        }

        const float startPos[3] = {start.x, start.y, start.z};
        const float endPos[3] = {end.x, end.y, end.z};
        dtPolyRef startRef = 0, endRef = 0;
        float startNearest[3], endNearest[3];
        const dtStatus s1 = m_impl->query->findNearestPoly(startPos, kQueryHalfExtents,
                                                           &m_impl->filter, &startRef, startNearest);
        const dtStatus s2 = m_impl->query->findNearestPoly(endPos, kQueryHalfExtents,
                                                           &m_impl->filter, &endRef, endNearest);
        if (dtStatusFailed(s1) || dtStatusFailed(s2) || startRef == 0 || endRef == 0)
        {
            return Status{ErrorCode::NotFound}; // start or end is off-mesh
        }

        dtPolyRef pathPolys[kMaxPathPolys];
        int pathCount = 0;
        const dtStatus fp = m_impl->query->findPath(startRef, endRef, startNearest, endNearest,
                                                    &m_impl->filter, pathPolys, &pathCount,
                                                    kMaxPathPolys);
        if (dtStatusFailed(fp) || pathCount == 0)
        {
            return Status{ErrorCode::NotFound};
        }

        // Reachable only if the path actually ends on the destination polygon. Detour also flags
        // DT_PARTIAL_RESULT; check both.
        const bool partial =
            dtStatusDetail(fp, DT_PARTIAL_RESULT) || pathPolys[pathCount - 1] != endRef;

        float straight[kMaxStraightPath * 3];
        unsigned char straightFlags[kMaxStraightPath];
        dtPolyRef straightRefs[kMaxStraightPath];
        int straightCount = 0;
        const dtStatus sp = m_impl->query->findStraightPath(startNearest, endNearest, pathPolys,
                                                            pathCount, straight, straightFlags,
                                                            straightRefs, &straightCount,
                                                            kMaxStraightPath);
        if (dtStatusFailed(sp))
        {
            return Status{ErrorCode::Internal};
        }

        out.corners.Reserve(static_cast<usize>(straightCount));
        for (int i = 0; i < straightCount; ++i)
        {
            out.corners.PushBack(
                Float3{straight[i * 3 + 0], straight[i * 3 + 1], straight[i * 3 + 2]});
        }
        out.complete = !partial;
        return Status{};
    }

    // ---------------------------------------------------------------------------------------
    // NavigationCrowd
    // ---------------------------------------------------------------------------------------

    struct NavigationCrowd::Impl
    {
        dtCrowd* crowd = nullptr;

        ~Impl()
        {
            if (crowd != nullptr)
            {
                dtFreeCrowd(crowd);
            }
        }
    };

    NavigationCrowd::NavigationCrowd(IAllocator& allocator, const NavigationMesh& mesh,
                                     i32 maxAgents, f32 maxAgentRadius)
        : m_impl(MakeUnique<Impl>(allocator))
    {
        dtNavMesh* navMesh = static_cast<dtNavMesh*>(mesh.NativeHandle());
        if (navMesh == nullptr || maxAgents <= 0)
        {
            return;
        }
        dtCrowd* crowd = dtAllocCrowd();
        if (crowd == nullptr)
        {
            return;
        }
        if (!crowd->init(maxAgents, maxAgentRadius > 0.0f ? maxAgentRadius : 0.6f, navMesh))
        {
            dtFreeCrowd(crowd);
            return;
        }
        m_impl->crowd = crowd;
    }

    NavigationCrowd::~NavigationCrowd() = default;
    NavigationCrowd::NavigationCrowd(NavigationCrowd&&) noexcept = default;
    NavigationCrowd& NavigationCrowd::operator=(NavigationCrowd&&) noexcept = default;

    bool NavigationCrowd::IsValid() const noexcept { return m_impl->crowd != nullptr; }

    namespace
    {
        // The crowd behaviours an agent runs: always the path smoothing, and the avoidance and
        // separation unless it goes straight where it is sent.
        unsigned char UpdateFlags(const NavigationAgentParams& params)
        {
            unsigned char flags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
            if (params.avoidOthers)
            {
                flags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
            }
            return flags;
        }
    }

    i32 NavigationCrowd::AddAgent(Float3 position, const NavigationAgentParams& params)
    {
        if (m_impl->crowd == nullptr)
        {
            return -1;
        }
        dtCrowdAgentParams ap;
        std::memset(&ap, 0, sizeof(ap));
        ap.radius = params.radius;
        ap.height = params.height;
        ap.maxSpeed = params.maxSpeed;
        ap.maxAcceleration = params.maxAcceleration;
        ap.collisionQueryRange = params.radius * 12.0f;
        ap.pathOptimizationRange = params.radius * 30.0f;
        ap.separationWeight = 2.0f;
        ap.obstacleAvoidanceType = 3;
        ap.updateFlags = UpdateFlags(params);
        const float pos[3] = {position.x, position.y, position.z};
        return m_impl->crowd->addAgent(pos, &ap);
    }

    NavigationAgentState NavigationCrowd::AgentState(i32 agentId) const
    {
        NavigationAgentState out;
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return out;
        }
        const dtCrowdAgent* agent = m_impl->crowd->getAgent(agentId);
        if (agent == nullptr || !agent->active)
        {
            return out;
        }
        out.valid = true;
        switch (agent->state)
        {
        case DT_CROWDAGENT_STATE_WALKING: out.state = NavAgentCrowdState::Walking; break;
        case DT_CROWDAGENT_STATE_OFFMESH: out.state = NavAgentCrowdState::OffMesh; break;
        default: out.state = NavAgentCrowdState::Invalid; break;
        }
        switch (agent->targetState)
        {
        case DT_CROWDAGENT_TARGET_NONE: out.targetState = NavAgentTargetState::None; break;
        case DT_CROWDAGENT_TARGET_VALID: out.targetState = NavAgentTargetState::Valid; break;
        case DT_CROWDAGENT_TARGET_VELOCITY:
            out.targetState = NavAgentTargetState::Velocity;
            break;
        case DT_CROWDAGENT_TARGET_FAILED: out.targetState = NavAgentTargetState::Failed; break;
        default: // REQUESTING, WAITING_FOR_QUEUE, WAITING_FOR_PATH: all "in flight"
            out.targetState = NavAgentTargetState::Requesting;
            break;
        }
        out.desiredSpeed = agent->desiredSpeed;
        out.cornerCount = agent->ncorners;
        return out;
    }

    void NavigationCrowd::SetAgentParams(i32 agentId, const NavigationAgentParams& params)
    {
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return;
        }
        const dtCrowdAgent* agent = m_impl->crowd->getAgent(agentId);
        if (agent == nullptr || !agent->active)
        {
            return;
        }
        dtCrowdAgentParams ap = agent->params; // keep the flags and avoidance the add configured
        ap.radius = params.radius;
        ap.height = params.height;
        ap.maxSpeed = params.maxSpeed;
        ap.maxAcceleration = params.maxAcceleration;
        ap.collisionQueryRange = params.radius * 12.0f;
        ap.pathOptimizationRange = params.radius * 30.0f;
        ap.updateFlags = UpdateFlags(params);
        m_impl->crowd->updateAgentParameters(agentId, &ap);
    }

    void NavigationCrowd::RemoveAgent(i32 agentId)
    {
        if (m_impl->crowd != nullptr && agentId >= 0)
        {
            m_impl->crowd->removeAgent(agentId);
        }
    }

    bool NavigationCrowd::SetTarget(i32 agentId, Float3 target)
    {
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return false;
        }
        const dtNavMeshQuery* query = m_impl->crowd->getNavMeshQuery();
        const dtQueryFilter* filter = m_impl->crowd->getFilter(0);
        const float* ext = m_impl->crowd->getQueryExtents();
        const float t[3] = {target.x, target.y, target.z};
        dtPolyRef ref = 0;
        float nearest[3] = {0, 0, 0};
        if (dtStatusFailed(query->findNearestPoly(t, ext, filter, &ref, nearest)) || ref == 0)
        {
            return false;
        }
        return m_impl->crowd->requestMoveTarget(agentId, ref, nearest);
    }

    bool NavigationCrowd::SetVelocity(i32 agentId, Float3 velocity)
    {
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return false;
        }
        const float v[3] = {velocity.x, velocity.y, velocity.z};
        return m_impl->crowd->requestMoveVelocity(agentId, v);
    }

    void NavigationCrowd::ClearTarget(i32 agentId)
    {
        if (m_impl->crowd != nullptr && agentId >= 0)
        {
            m_impl->crowd->resetMoveTarget(agentId);
        }
    }

    void NavigationCrowd::Update(f32 deltaTime)
    {
        if (m_impl->crowd != nullptr && deltaTime > 0.0f)
        {
            m_impl->crowd->update(deltaTime, nullptr);
        }
    }

    Float3 NavigationCrowd::AgentPosition(i32 agentId) const
    {
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return Float3{0, 0, 0};
        }
        const dtCrowdAgent* agent = m_impl->crowd->getAgent(agentId);
        if (agent == nullptr || !agent->active)
        {
            return Float3{0, 0, 0};
        }
        return Float3{agent->npos[0], agent->npos[1], agent->npos[2]};
    }

    Float3 NavigationCrowd::AgentVelocity(i32 agentId) const
    {
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return Float3{0, 0, 0};
        }
        const dtCrowdAgent* agent = m_impl->crowd->getAgent(agentId);
        if (agent == nullptr || !agent->active)
        {
            return Float3{0, 0, 0};
        }
        return Float3{agent->vel[0], agent->vel[1], agent->vel[2]};
    }

    bool NavigationCrowd::IsAgentValid(i32 agentId) const
    {
        if (m_impl->crowd == nullptr || agentId < 0)
        {
            return false;
        }
        const dtCrowdAgent* agent = m_impl->crowd->getAgent(agentId);
        return agent != nullptr && agent->active;
    }
}
