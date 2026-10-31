// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Terrain - `foundation.terrain`.
///
/// The terrain model over a heightfield (foundation.heightfield): the chunk grid and quadtree the
/// chunked geo-mipmap renderer draws, per-chunk LOD selection given a camera, and the splat layer
/// descriptors. Pure data and math with no RHI: every function here is unit-testable without a device.
/// A heightfield of side S = 64k+1 tiles into k x k chunks of 64 quads (65 verts) sharing edges.

module;
#include "Core/Prelude.h"

export module foundation.terrain;

import foundation.core;
import foundation.heightfield;
import foundation.lod; // the shared LOD selection math (coverage and threshold walk), one formula

using namespace foundation::core;
namespace lod = foundation::lod;

export namespace foundation::terrain
{
    /// One splat layer's pure parameters (the material and texture refs live in foundation.terrain.
    /// resource). tileScale is how many times the layer texture repeats across the whole terrain.
    struct SplatLayer
    {
        f32 tileScale = 1.0f;
    };

    inline constexpr i32 kChunkQuads = 64; // quads per chunk side (65 verts, shared edges)
    inline constexpr i32 kChunkVerts = kChunkQuads + 1; // 65 verts per side
    inline constexpr u32 kMaxChunkLod = 6; // stride 2^6 = 64 -> a single quad (the coarsest)

    /// Chunks along one side of a heightfield of the given size (S = 64k+1 -> k).
    [[nodiscard]] constexpr i32 ChunksPerSide(i32 heightfieldSize) noexcept
    {
        return heightfieldSize > 1 ? (heightfieldSize - 1) / kChunkQuads : 0;
    }

    // ---- Shared chunk grid mesh (chunked geo-mipmapping) ----
    // One 65x65 vertex grid is uploaded once and drawn for every chunk; per-chunk instance data
    // places it in the world and the VS fetches height from the terrain texture. Each LOD reuses the
    // same vertices through a stride-2^lod index buffer (finest is lod 0). Generation is RHI-free;
    // the renderer just uploads these arrays.

    /// Grid vertices of the shared chunk mesh, two copies stacked: first the 65x65 surface grid
    /// (skirt flag 0), then a 65x65 skirt copy (skirt flag 1) the VS drops below the surface to hide
    /// LOD-seam cracks. Each vertex is (u, v, skirtFlag): u,v in [0,1] row-major (v-major); the
    /// renderer maps u,v to world XZ and the height sample coordinate per chunk. The skirt copy shares
    /// the border u,v so a skirt vertex sits directly under its surface twin; interior skirt vertices
    /// are never indexed (only the border ring is). One VB, uploaded once, drawn for every chunk.
    inline void BuildChunkGridVertices(Array<Float3>& out)
    {
        out.Clear();
        out.Reserve(static_cast<usize>(kChunkVerts) * kChunkVerts * 2);
        const f32 span = static_cast<f32>(kChunkQuads);
        for (i32 pass = 0; pass < 2; ++pass) // 0 surface, 1 skirt copy
        {
            const f32 flag = static_cast<f32>(pass);
            for (i32 z = 0; z < kChunkVerts; ++z)
            {
                for (i32 x = 0; x < kChunkVerts; ++x)
                {
                    out.PushBack(
                        Float3{static_cast<f32>(x) / span, static_cast<f32>(z) / span, flag});
                }
            }
        }
    }

    /// The surface-vertex count (the skirt copy begins at this index in the shared VB).
    inline constexpr u32 kChunkSurfaceVertexCount =
        static_cast<u32>(kChunkVerts) * static_cast<u32>(kChunkVerts);

    /// Quads per chunk side at LOD `lod` (stride 2^lod); 0 past kMaxChunkLod.
    [[nodiscard]] constexpr u32 ChunkLodQuadsPerSide(u32 lod) noexcept
    {
        return lod > kMaxChunkLod ? 0u : (static_cast<u32>(kChunkQuads) >> lod);
    }

    /// The count of surface indices at LOD `lod` (the prefix of BuildChunkGridIndices before the skirt
    /// walls): the draw range for a skirtless pass. Skirt indices follow (the rest of the buffer).
    [[nodiscard]] constexpr u32 ChunkLodSurfaceIndexCount(u32 lod) noexcept
    {
        const u32 q = ChunkLodQuadsPerSide(lod);
        return q * q * 6u;
    }

    /// The 32-bit triangle indices for LOD `lod` (stride 2^lod) over the shared grid: the surface
    /// (two tris per quad, wound CCW seen from +Y) followed by a skirt wall around the 4 chunk edges
    /// (each border segment lifted to a quad joining the surface border verts to their dropped skirt
    /// twins), so a coarser neighbour's lower edge can never open a see-through crack. A lod past
    /// kMaxChunkLod yields none.
    inline void BuildChunkGridIndices(u32 lod, Array<u32>& out)
    {
        out.Clear();
        if (lod > kMaxChunkLod)
        {
            return;
        }
        const i32 stride = 1 << lod;
        const i32 quads = kChunkQuads / stride; // quads per side at this LOD
        out.Reserve(static_cast<usize>(quads) * quads * 6 + static_cast<usize>(quads) * 4 * 6);
        for (i32 qz = 0; qz < quads; ++qz)
        {
            for (i32 qx = 0; qx < quads; ++qx)
            {
                const i32 x0 = qx * stride;
                const i32 z0 = qz * stride;
                const i32 x1 = x0 + stride;
                const i32 z1 = z0 + stride;
                const u32 v00 = static_cast<u32>(z0 * kChunkVerts + x0);
                const u32 v10 = static_cast<u32>(z0 * kChunkVerts + x1);
                const u32 v01 = static_cast<u32>(z1 * kChunkVerts + x0);
                const u32 v11 = static_cast<u32>(z1 * kChunkVerts + x1);
                out.PushBack(v00);
                out.PushBack(v01);
                out.PushBack(v11);
                out.PushBack(v00);
                out.PushBack(v11);
                out.PushBack(v10);
            }
        }

        // Skirt walls: one quad per border segment, its top the surface border vert, its bottom the
        // skirt twin (surfaceIndex + kChunkSurfaceVertexCount). Emitted for both windings (double-sided) so
        // the wall plugs the seam regardless of view angle. The four edges walk the border at `stride`.
        const auto surf = [](i32 x, i32 z) { return static_cast<u32>(z * kChunkVerts + x); };
        const auto skirt = [&surf](i32 x, i32 z) { return surf(x, z) + kChunkSurfaceVertexCount; };
        const auto wall = [&out](u32 t0, u32 t1, u32 b0, u32 b1)
        {
            // top t0-t1, bottom b0-b1; both windings (double-sided plug).
            out.PushBack(t0); out.PushBack(b0); out.PushBack(b1);
            out.PushBack(t0); out.PushBack(b1); out.PushBack(t1);
            out.PushBack(t0); out.PushBack(b1); out.PushBack(b0);
            out.PushBack(t0); out.PushBack(t1); out.PushBack(b1);
        };
        for (i32 i = 0; i < kChunkQuads; i += stride)
        {
            const i32 j = i + stride;
            wall(surf(i, 0), surf(j, 0), skirt(i, 0), skirt(j, 0));                     // z = 0 edge
            wall(surf(i, kChunkQuads), surf(j, kChunkQuads), skirt(i, kChunkQuads),
                 skirt(j, kChunkQuads));                                                // z = max edge
            wall(surf(0, i), surf(0, j), skirt(0, i), skirt(0, j));                     // x = 0 edge
            wall(surf(kChunkQuads, i), surf(kChunkQuads, j), skirt(kChunkQuads, i),
                 skirt(kChunkQuads, j));                                                // x = max edge
        }
    }

    /// The indices of one holed chunk at `lod`: the same walk as BuildChunkGridIndices with
    /// quads dropped by their sample block (the stride-square they span, interior included) and
    /// every skirt segment dropped whose border quad was, so no wall hangs under a cut rim. Two
    /// rules: `dropWhenAnyCut` drops a quad holding any cut sample (the strict rule every non-GPU
    /// consumer applies, so a hole never shrinks with distance); otherwise a quad is dropped only
    /// when every sample in its block is cut (the render rule: the pixel shaders' bilinear hole
    /// mask then shapes the rim inside the quads that remain, at every LOD). The surface prefix
    /// count (the depth and pick draw range) comes
    /// back in `outSurfaceIndexCount`. Neighbours share the edge samples: both sides agree.
    inline void BuildHoledChunkIndices(const heightfield::Heightfield& hf, i32 gridX0, i32 gridZ0,
                                       u32 lod, Array<u32>& out, u32& outSurfaceIndexCount,
                                       bool dropWhenAnyCut = true)
    {
        out.Clear();
        outSurfaceIndexCount = 0;
        if (lod > kMaxChunkLod)
        {
            return;
        }
        const i32 stride = 1 << lod;
        const i32 quads = kChunkQuads / stride;
        const auto quadKept = [&](i32 qx, i32 qz)
        {
            const i32 x0 = gridX0 + qx * stride;
            const i32 z0 = gridZ0 + qz * stride;
            if (dropWhenAnyCut)
            {
                return !hf.BlockHasHole(x0, z0, x0 + stride, z0 + stride);
            }
            for (i32 gz = z0; gz <= z0 + stride; ++gz) // kept while one sample is still solid
            {
                for (i32 gx = x0; gx <= x0 + stride; ++gx)
                {
                    if (!hf.IsHole(gx, gz))
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        out.Reserve(static_cast<usize>(quads) * quads * 6 + static_cast<usize>(quads) * 4 * 6);
        for (i32 qz = 0; qz < quads; ++qz)
        {
            for (i32 qx = 0; qx < quads; ++qx)
            {
                if (!quadKept(qx, qz))
                {
                    continue;
                }
                const i32 x0 = qx * stride;
                const i32 z0 = qz * stride;
                const i32 x1 = x0 + stride;
                const i32 z1 = z0 + stride;
                const u32 v00 = static_cast<u32>(z0 * kChunkVerts + x0);
                const u32 v10 = static_cast<u32>(z0 * kChunkVerts + x1);
                const u32 v01 = static_cast<u32>(z1 * kChunkVerts + x0);
                const u32 v11 = static_cast<u32>(z1 * kChunkVerts + x1);
                out.PushBack(v00);
                out.PushBack(v01);
                out.PushBack(v11);
                out.PushBack(v00);
                out.PushBack(v11);
                out.PushBack(v10);
            }
        }
        outSurfaceIndexCount = static_cast<u32>(out.Size());

        const auto surf = [](i32 x, i32 z) { return static_cast<u32>(z * kChunkVerts + x); };
        const auto skirt = [&surf](i32 x, i32 z) { return surf(x, z) + kChunkSurfaceVertexCount; };
        const auto wall = [&out](u32 t0, u32 t1, u32 b0, u32 b1)
        {
            out.PushBack(t0); out.PushBack(b0); out.PushBack(b1);
            out.PushBack(t0); out.PushBack(b1); out.PushBack(t1);
            out.PushBack(t0); out.PushBack(b1); out.PushBack(b0);
            out.PushBack(t0); out.PushBack(t1); out.PushBack(b1);
        };
        for (i32 q = 0; q < quads; ++q)
        {
            const i32 i = q * stride;
            const i32 j = i + stride;
            if (quadKept(q, 0)) // z = 0 edge: under the first row's quads
            {
                wall(surf(i, 0), surf(j, 0), skirt(i, 0), skirt(j, 0));
            }
            if (quadKept(q, quads - 1)) // z = max edge
            {
                wall(surf(i, kChunkQuads), surf(j, kChunkQuads), skirt(i, kChunkQuads),
                     skirt(j, kChunkQuads));
            }
            if (quadKept(0, q)) // x = 0 edge
            {
                wall(surf(0, i), surf(0, j), skirt(0, i), skirt(0, j));
            }
            if (quadKept(quads - 1, q)) // x = max edge
            {
                wall(surf(kChunkQuads, i), surf(kChunkQuads, j), skirt(kChunkQuads, i),
                     skirt(kChunkQuads, j));
            }
        }
    }

    /// One terrain chunk: its position in the chunk grid, the grid origin of its sample block, and
    /// its local-space bounds (the renderer and physics apply the entity transform).
    struct TerrainChunk
    {
        i32 chunkX = 0;
        i32 chunkZ = 0;
        i32 gridX0 = 0; // sample-grid origin (covers [gridX0, gridX0+64] x [gridZ0, gridZ0+64])
        i32 gridZ0 = 0;
        AABB bounds = AABB::Empty();
        // A cut sample in the chunk's block (edges shared with the neighbours): this chunk draws
        // its own index buffers (BuildHoledChunkIndices) instead of the shared grid's. When every
        // sample is cut the chunk has no surface at all (allCut) and is not drawn.
        bool hasHoles = false;
        bool allCut = false;
    };

    /// Build the k x k chunk grid from a heightfield: each chunk's XZ footprint from the world
    /// mapping, its Y range from the heightfield's per-cell bounds. Row-major (chunkZ outer): the
    /// chunk at (cx, cz) is index cz * ChunksPerSide + cx.
    inline void BuildChunks(const heightfield::Heightfield& hf, Array<TerrainChunk>& out)
    {
        out.Clear();
        const i32 k = ChunksPerSide(hf.Size());
        for (i32 cz = 0; cz < k; ++cz)
        {
            for (i32 cx = 0; cx < k; ++cx)
            {
                const i32 gx0 = cx * kChunkQuads;
                const i32 gz0 = cz * kChunkQuads;
                f32 minY = 0.0f;
                f32 maxY = 0.0f;
                hf.CellBounds(gx0, gz0, gx0 + kChunkQuads, gz0 + kChunkQuads, minY, maxY);
                const Float2 w0 = hf.GridToWorld(static_cast<f32>(gx0), static_cast<f32>(gz0));
                const Float2 w1 = hf.GridToWorld(static_cast<f32>(gx0 + kChunkQuads),
                                                 static_cast<f32>(gz0 + kChunkQuads));
                TerrainChunk chunk;
                chunk.chunkX = cx;
                chunk.chunkZ = cz;
                chunk.gridX0 = gx0;
                chunk.gridZ0 = gz0;
                chunk.bounds.min = Float3{w0.x, minY, w0.y};
                chunk.bounds.max = Float3{w1.x, maxY, w1.y};
                chunk.hasHoles = hf.BlockHasHole(gx0, gz0, gx0 + kChunkQuads, gz0 + kChunkQuads);
                if (chunk.hasHoles)
                {
                    bool all = true;
                    for (i32 gz = gz0; gz <= gz0 + kChunkQuads && all; ++gz)
                    {
                        for (i32 gx = gx0; gx <= gx0 + kChunkQuads; ++gx)
                        {
                            if (!hf.IsHole(gx, gz))
                            {
                                all = false;
                                break;
                            }
                        }
                    }
                    chunk.allCut = all;
                }
                out.PushBack(chunk);
            }
        }
    }

    /// A chunk's local-space bounding sphere, the bridge to the shared LOD coverage metric.
    [[nodiscard]] inline BoundingSphere ChunkBoundingSphere(const TerrainChunk& chunk) noexcept
    {
        BoundingSphere sphere;
        sphere.center = chunk.bounds.Center();
        sphere.radius = Length(chunk.bounds.Extents());
        return sphere;
    }

    /// Per-chunk LOD through the shared coverage metric (foundation.lod: one formula for meshes and
    /// terrain). The chunk's local bounding sphere is placed in the world by `chunkToWorld`
    /// (translation and Y-rotation per the terrain instance model, so the radius is preserved),
    /// projected to screen coverage, then walked against the descending `coverageThresholds`
    /// (thresholds[0] = 1.0 by convention, level 0 the fallback). Pure, so unit-testable headless.
    [[nodiscard]] inline u32 SelectChunkLod(const TerrainChunk& chunk, const Float4x4& chunkToWorld,
                                            const Float4x4& view, const Float4x4& projection,
                                            Span<const f32> coverageThresholds,
                                            f32 bias = 0.0f) noexcept
    {
        const BoundingSphere sphere = ChunkBoundingSphere(chunk);
        const Float3 worldCenter = TransformPoint(sphere.center, chunkToWorld);
        const f32 coverage =
            lod::ProjectedSphereCoverage(view, projection, worldCenter, sphere.radius, bias);
        return lod::SelectLevelByCoverage(coverageThresholds, coverage);
    }

    /// Per-chunk LOD levels for a view (the renderer feeds these to the draw).
    inline void SelectChunkLods(Span<const TerrainChunk> chunks, const Float4x4& chunkToWorld,
                                const Float4x4& view, const Float4x4& projection,
                                Span<const f32> coverageThresholds, f32 bias, Array<u32>& out)
    {
        out.Clear();
        out.Reserve(chunks.Size());
        for (usize i = 0; i < chunks.Size(); ++i)
        {
            out.PushBack(
                SelectChunkLod(chunks[i], chunkToWorld, view, projection, coverageThresholds, bias));
        }
    }

    /// A quadtree over the chunk grid for hierarchical frustum culling. Leaves are single chunks;
    /// internal nodes carry the union bounds so a Disjoint node prunes its whole subtree. Works for
    /// any k (uneven splits when a side is odd).
    class TerrainQuadtree
    {
    public:
        TerrainQuadtree() = default;

        void Build(Span<const TerrainChunk> chunks, i32 chunksPerSide)
        {
            m_nodes.Clear();
            m_chunks = chunks;
            m_side = chunksPerSide;
            if (chunksPerSide > 0 && chunks.Size() >= static_cast<usize>(chunksPerSide) * chunksPerSide)
            {
                BuildNode(0, chunksPerSide, 0, chunksPerSide);
            }
        }

        /// Quadtree node. Public and trivially destructible: the render extraction snapshots the
        /// node array into its frame arena (see Nodes() and CullNodes), because a pointer to this
        /// quadtree object is only as stable as its owner, and owners can mutate mid-frame; the
        /// snapshot must be self-contained.
        struct Node
        {
            AABB bounds = AABB::Empty();
            i32 chunkIndex = -1; // >= 0 for a leaf
            i32 childCount = 0;
            i32 children[4] = {-1, -1, -1, -1};
        };

        [[nodiscard]] bool IsEmpty() const noexcept { return m_nodes.IsEmpty(); }
        [[nodiscard]] usize NodeCount() const noexcept { return m_nodes.Size(); }
        /// The flat node storage (root = index 0). Snapshot-friendly: a copied node span culls
        /// identically through CullNodes with no reference back to this object or its chunk span.
        [[nodiscard]] Span<const Node> Nodes() const noexcept
        {
            return Span<const Node>{m_nodes.Data(), m_nodes.Size()};
        }

        /// Gather the indices of chunks whose bounds are not disjoint from the frustum.
        void Cull(const BoundingFrustum& frustum, Array<i32>& outVisible) const
        {
            CullNodes(Nodes(), frustum, outVisible);
        }

        /// Cull over a bare node span (root = index 0): the form the renderer uses against its
        /// frame-arena copy of the nodes, so no live quadtree object is dereferenced at draw time.
        static void CullNodes(Span<const Node> nodes, const BoundingFrustum& frustum,
                              Array<i32>& outVisible)
        {
            outVisible.Clear();
            if (!nodes.IsEmpty())
            {
                CullNodeIn(nodes, 0, frustum, outVisible);
            }
        }

    private:

        // Build over the chunk-index rectangle [x0, x1) x [z0, z1); returns the node index.
        i32 BuildNode(i32 x0, i32 x1, i32 z0, i32 z1)
        {
            const i32 nodeIdx = static_cast<i32>(m_nodes.Size());
            m_nodes.PushBack(Node{});
            if (x1 - x0 == 1 && z1 - z0 == 1)
            {
                const i32 ci = z0 * m_side + x0;
                m_nodes[static_cast<usize>(nodeIdx)].chunkIndex = ci;
                m_nodes[static_cast<usize>(nodeIdx)].bounds = m_chunks[static_cast<usize>(ci)].bounds;
                return nodeIdx;
            }
            const i32 xm = (x1 - x0 > 1) ? (x0 + x1) / 2 : x1;
            const i32 zm = (z1 - z0 > 1) ? (z0 + z1) / 2 : z1;
            const i32 xEdges[3] = {x0, xm, x1};
            const i32 zEdges[3] = {z0, zm, z1};
            const i32 xCount = (x1 - x0 > 1) ? 2 : 1;
            const i32 zCount = (z1 - z0 > 1) ? 2 : 1;
            AABB bounds = AABB::Empty();
            i32 childCount = 0;
            for (i32 zi = 0; zi < zCount; ++zi)
            {
                for (i32 xi = 0; xi < xCount; ++xi)
                {
                    const i32 child =
                        BuildNode(xEdges[xi], xEdges[xi + 1], zEdges[zi], zEdges[zi + 1]);
                    m_nodes[static_cast<usize>(nodeIdx)].children[childCount++] = child;
                    bounds = Merge(bounds, m_nodes[static_cast<usize>(child)].bounds);
                }
            }
            m_nodes[static_cast<usize>(nodeIdx)].childCount = childCount;
            m_nodes[static_cast<usize>(nodeIdx)].bounds = bounds;
            return nodeIdx;
        }

        static void CullNodeIn(Span<const Node> nodes, i32 nodeIdx, const BoundingFrustum& frustum,
                               Array<i32>& out)
        {
            const Node& node = nodes[static_cast<usize>(nodeIdx)];
            if (Contains(frustum, node.bounds) == ContainmentType::Disjoint)
            {
                return;
            }
            if (node.chunkIndex >= 0)
            {
                out.PushBack(node.chunkIndex);
                return;
            }
            for (i32 i = 0; i < node.childCount; ++i)
            {
                CullNodeIn(nodes, node.children[i], frustum, out);
            }
        }

        Array<Node> m_nodes;
        Span<const TerrainChunk> m_chunks;
        i32 m_side = 0;
    };

    /// One chunk to draw this frame: which chunk and its selected LOD (the index buffer to bind).
    struct ChunkDraw
    {
        i32 chunkIndex = 0;
        u32 lod = 0;
    };

    /// The renderer's per-frame CPU extract: frustum-cull the chunk quadtree, then LOD each visible
    /// chunk by the shared coverage metric, giving the {chunk, lod} draw list the GPU renderer
    /// consumes. Pure (no RHI): view and projection are plain matrices, chunkToWorld places the
    /// terrain entity.
    inline void ExtractVisibleChunkDraws(Span<const TerrainQuadtree::Node> nodes,
                                         Span<const TerrainChunk> chunks,
                                         const Float4x4& chunkToWorld, const Float4x4& view,
                                         const Float4x4& projection, const BoundingFrustum& frustum,
                                         Span<const f32> coverageThresholds, f32 bias,
                                         Array<ChunkDraw>& out)
    {
        out.Clear();
        Array<i32> visible;
        TerrainQuadtree::CullNodes(nodes, frustum, visible);
        out.Reserve(visible.Size());
        for (usize i = 0; i < visible.Size(); ++i)
        {
            const i32 ci = visible[i];
            const u32 lod = SelectChunkLod(chunks[static_cast<usize>(ci)], chunkToWorld, view,
                                           projection, coverageThresholds, bias);
            out.PushBack(ChunkDraw{ci, lod});
        }
    }

    /// Convenience over a live quadtree (tests and CPU callers that own the tree).
    inline void ExtractVisibleChunkDraws(const TerrainQuadtree& tree, Span<const TerrainChunk> chunks,
                                         const Float4x4& chunkToWorld, const Float4x4& view,
                                         const Float4x4& projection, const BoundingFrustum& frustum,
                                         Span<const f32> coverageThresholds, f32 bias,
                                         Array<ChunkDraw>& out)
    {
        ExtractVisibleChunkDraws(tree.Nodes(), chunks, chunkToWorld, view, projection, frustum,
                                 coverageThresholds, bias, out);
    }
}
