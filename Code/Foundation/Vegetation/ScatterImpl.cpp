// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.vegetation:scatter implementation; Scatter.cppm states the contract.

module;
#include "Core/Prelude.h"

module foundation.vegetation;

import foundation.core;
import foundation.heightfield;
import foundation.terrain;
import foundation.terrain.resource;
import foundation.vegetation.resource;

using namespace foundation::core;

namespace foundation::vegetation
{
    namespace
    {
        // The splat texel (nearest) under a terrain-local XZ point: the splat raster spans the
        // whole footprint, like the terrain shader's splat uv (0..1 across the grid).
        f32 SplatShare(const terrain::SplatWeights& splat,
                       const heightfield::Heightfield& heightfield, u32 paletteIndex, f32 localX,
                       f32 localZ) noexcept
        {
            if (splat.IsEmpty())
            {
                return 0.0f;
            }
            const Float2 size = heightfield.WorldSize();
            const f32 u = Clamp(localX / Max(size.x, 1e-6f) + 0.5f, 0.0f, 1.0f);
            const f32 v = Clamp(localZ / Max(size.y, 1e-6f) + 0.5f, 0.0f, 1.0f);
            const i32 sx = Clamp(static_cast<i32>(u * static_cast<f32>(splat.Width() - 1) + 0.5f),
                                 0, splat.Width() - 1);
            const i32 sy = Clamp(static_cast<i32>(v * static_cast<f32>(splat.Height() - 1) + 0.5f),
                                 0, splat.Height() - 1);
            const u8 w = (paletteIndex == kSplatBaseLayer) ? splat.BaseWeight(sx, sy)
                                                            : splat.WeightOfLayer(sx, sy, paletteIndex);
            return static_cast<f32>(w) / 255.0f;
        }

        // The mask plane's density (0..1) under a terrain-local XZ point (nearest texel; the
        // same footprint mapping as the splat).
        f32 MaskShare(const VegetationMask& mask, const heightfield::Heightfield& heightfield,
                      u32 plane, f32 localX, f32 localZ) noexcept
        {
            const Float2 size = heightfield.WorldSize();
            const f32 u = localX / Max(size.x, 1e-6f) + 0.5f;
            const f32 v = localZ / Max(size.y, 1e-6f) + 0.5f;
            return mask.ShareAt(plane, u, v);
        }

        // A right-handed frame whose Y axis is `up` (row-vector convention: the rows are the axes).
        Float4x4 FrameFromUp(Float3 up, f32 yaw) noexcept
        {
            // Yaw first (about the terrain-local Y), then tilt the whole frame onto the normal.
            const f32 c = Cos(yaw);
            const f32 s = Sin(yaw);
            const Float3 flatX{c, 0.0f, -s};
            const Float3 flatZ{s, 0.0f, c};
            const Float3 n = Normalized(up);
            // Rotate the flat frame's X onto the plane perpendicular to n; Z completes it.
            Float3 x = flatX - n * Dot(flatX, n);
            if (LengthSquared(x) < 1e-8f)
            {
                x = flatZ - n * Dot(flatZ, n);
            }
            x = Normalized(x);
            const Float3 z = Cross(x, n);
            return Float4x4{{{x.x, x.y, x.z, 0.0f},
                             {n.x, n.y, n.z, 0.0f},
                             {z.x, z.y, z.z, 0.0f},
                             {0.0f, 0.0f, 0.0f, 1.0f}}};
        }
    }

    f32 PlacementShareAt(const ScatterLayer& layer, const heightfield::Heightfield& heightfield,
                         const terrain::SplatWeights* splat, const VegetationMask* mask,
                         f32 localX, f32 localZ) noexcept
    {
        if (heightfield.HasHoles()) // a cut cell has no surface: nothing grows there
        {
            i32 cx = 0;
            i32 cz = 0;
            heightfield.CellOfLocal(localX, localZ, cx, cz);
            if (heightfield.CellHasHole(cx, cz))
            {
                return 0.0f;
            }
        }
        switch (layer.placement)
        {
        case VegetationPlacement::Uniform:
            return 1.0f;
        case VegetationPlacement::Splat:
            return splat != nullptr ? SplatShare(*splat, heightfield, layer.splatLayer, localX, localZ)
                                    : 0.0f;
        case VegetationPlacement::Mask:
            return mask != nullptr ? MaskShare(*mask, heightfield, layer.maskPlane, localX, localZ)
                                   : 0.0f;
        case VegetationPlacement::SplatTimesMask:
            return (splat != nullptr && mask != nullptr)
                       ? SplatShare(*splat, heightfield, layer.splatLayer, localX, localZ) *
                             MaskShare(*mask, heightfield, layer.maskPlane, localX, localZ)
                       : 0.0f;
        default:
            return 0.0f;
        }
    }

    void ScatterChunk(u64 seed, const terrain::TerrainChunk& chunk,
                      const heightfield::Heightfield& heightfield,
                      const terrain::SplatWeights* splat, const VegetationMask* mask,
                      const ScatterLayer& layer, const AABB& meshLocalBounds,
                      ScatterResult& out)
    {
        out.transforms.Clear();
        out.localBounds = chunk.bounds;
        out.candidateCount = 0;
        out.effectiveDensity = 0.0f;
        out.densityClamped = false;
        if (heightfield.IsEmpty() || layer.density <= 0.0f || layer.maxInstancesPerChunk == 0)
        {
            return;
        }

        const f32 minX = chunk.bounds.min.x;
        const f32 minZ = chunk.bounds.min.z;
        const f32 sizeX = chunk.bounds.max.x - minX;
        const f32 sizeZ = chunk.bounds.max.z - minZ;
        const f32 area = sizeX * sizeZ;
        if (area <= 0.0f)
        {
            return;
        }

        // Candidates: the layer's own density x the chunk area (a CPU ceiling, never a scale).
        // The cap (maxInstancesPerChunk) bounds what the chunk holds: the loop stops when it is
        // reached, so a mask patch covering a slice of the chunk still grows at the layer's
        // density (a candidate outside the patch costs no cap), and only a chunk that fills up
        // is clamped (the manager warns once). Scaling the density down to the cap over the
        // whole chunk area first would starve a small painted patch.
        const f32 wanted = Min(layer.density * area, static_cast<f32>(kMaxCandidatesPerChunk));
        const u32 candidates = static_cast<u32>(wanted + 0.5f);
        const u32 cap = layer.maxInstancesPerChunk;
        out.candidateCount = candidates;
        out.effectiveDensity = layer.density;
        if (candidates == 0)
        {
            return;
        }

        const f32 scaleMin = Min(layer.scaleRange.x, layer.scaleRange.y);
        const f32 scaleMax = Max(layer.scaleRange.x, layer.scaleRange.y);
        const f32 minNormalY = Cos(Clamp(layer.maxSlopeDegrees, 0.0f, 90.0f) * (kPi / 180.0f));
        const f32 heightMin = Min(layer.heightRange.x, layer.heightRange.y);
        const f32 heightMax = Max(layer.heightRange.x, layer.heightRange.y);

        Random rng(seed);
        out.transforms.Reserve(Min(candidates, cap));
        for (u32 i = 0; i < candidates; ++i)
        {
            if (out.transforms.Size() >= cap) // full: the cap holds, the density does not
            {
                out.densityClamped = true;
                out.effectiveDensity = static_cast<f32>(cap) / area;
                out.candidateCount = i;
                break;
            }
            // Draw every random number a candidate can consume up front, so a rejection never
            // shifts the stream of the ones after it (the accepted set stays a stable prefix
            // thinning of the candidate set as parameters move).
            const f32 x = minX + rng.NextFloat() * sizeX;
            const f32 z = minZ + rng.NextFloat() * sizeZ;
            const f32 keep = rng.NextFloat();
            const f32 yaw = rng.NextFloat() * kTwoPi;
            const f32 scale = scaleMin + rng.NextFloat() * (scaleMax - scaleMin);

            const f32 share = PlacementShareAt(layer, heightfield, splat, mask, x, z);
            const bool thresholded = layer.placement == VegetationPlacement::Splat ||
                                     layer.placement == VegetationPlacement::SplatTimesMask;
            if (share <= 0.0f || (thresholded && share < layer.splatThreshold) || keep >= share)
            {
                continue;
            }
            const Float3 normal = heightfield.GetNormalAt(x, z);
            if (normal.y < minNormalY)
            {
                continue;
            }
            const f32 y = heightfield.GetHeightAt(x, z);
            if (y < heightMin || y > heightMax)
            {
                continue;
            }

            const Float4x4 rotation = layer.alignToNormal ? FrameFromUp(normal, yaw)
                                                          : Float4x4::RotationY(yaw);
            out.transforms.PushBack(Float4x4::Scale(Float3{scale, scale, scale}) * rotation *
                                    Float4x4::Translation(Float3{x, y, z}));
        }

        // Bounds: the chunk's terrain AABB grown by the mesh's extent at the largest scale (a
        // blade's tip, a rock's overhang), so the set's cull sphere covers every instance.
        if (!out.transforms.IsEmpty() && meshLocalBounds.max.x >= meshLocalBounds.min.x)
        {
            const f32 reach = Length(meshLocalBounds.Extents()) + Length(meshLocalBounds.Center());
            const f32 grow = reach * scaleMax;
            out.localBounds.min = out.localBounds.min - Float3{grow, grow, grow};
            out.localBounds.max = out.localBounds.max + Float3{grow, grow, grow};
        }
    }

    StampResult ScatterStamp(u64 seed, const heightfield::Heightfield& heightfield,
                             const ScatterLayer& layer, const AABB& meshLocalBounds,
                             f32 centreX, f32 centreZ, f32 radius, f32 density, f32 amount,
                             f32 spacing, Span<const Float4x4> existing,
                             const BlockedQuery& blocked, Array<Float4x4>& out)
    {
        StampResult result;
        if (heightfield.IsEmpty() || radius <= 0.0f || density <= 0.0f || amount <= 0.0f)
        {
            return result;
        }
        const f32 area = kPi * radius * radius;
        const u32 candidates = static_cast<u32>(density * area * Clamp(amount, 0.0f, 1.0f) + 0.5f);
        result.candidates = candidates;
        if (candidates == 0)
        {
            return result;
        }
        const f32 scaleMin = Min(layer.scaleRange.x, layer.scaleRange.y);
        const f32 scaleMax = Max(layer.scaleRange.x, layer.scaleRange.y);
        const f32 minNormalY = Cos(Clamp(layer.maxSlopeDegrees, 0.0f, 90.0f) * (kPi / 180.0f));
        const f32 heightMin = Min(layer.heightRange.x, layer.heightRange.y);
        const f32 heightMax = Max(layer.heightRange.x, layer.heightRange.y);
        const bool hasBounds = meshLocalBounds.max.x >= meshLocalBounds.min.x;
        const f32 meshRadius = hasBounds ? Length(meshLocalBounds.Extents()) : 0.5f;
        const usize firstNew = out.Size();

        // The spacing test against the existing instances near the disc (their XZ) and the
        // ones this stamp placed.
        const auto tooClose = [&](f32 x, f32 z, f32 reach) -> bool
        {
            const auto near = [&](const Float4x4& m) -> bool
            {
                const f32 dx = m.m[3][0] - x;
                const f32 dz = m.m[3][2] - z;
                return dx * dx + dz * dz < reach * reach;
            };
            for (const Float4x4& m : existing)
            {
                if (near(m))
                {
                    return true;
                }
            }
            for (usize i = firstNew; i < out.Size(); ++i)
            {
                if (near(out[i]))
                {
                    return true;
                }
            }
            return false;
        };

        Random rng(seed);
        for (u32 i = 0; i < candidates; ++i)
        {
            // Every random number a candidate can use is drawn up front (a rejection never
            // shifts the ones after it).
            const f32 angle = rng.NextFloat() * kTwoPi;
            const f32 r = radius * Sqrt(rng.NextFloat()); // uniform over the disc
            const f32 yaw = rng.NextFloat() * kTwoPi;
            const f32 scale = scaleMin + rng.NextFloat() * (scaleMax - scaleMin);
            const f32 x = centreX + Cos(angle) * r;
            const f32 z = centreZ + Sin(angle) * r;

            const Float3 normal = heightfield.GetNormalAt(x, z);
            const f32 y = heightfield.GetHeightAt(x, z);
            if (normal.y < minNormalY || y < heightMin || y > heightMax)
            {
                ++result.rejectedRules;
                continue;
            }
            if (heightfield.HasHoles()) // no prop stands over a cut cell
            {
                i32 cx = 0;
                i32 cz = 0;
                heightfield.CellOfLocal(x, z, cx, cz);
                if (heightfield.CellHasHole(cx, cz))
                {
                    ++result.rejectedRules;
                    continue;
                }
            }
            const f32 reach = Max(spacing, 0.0f) * meshRadius * scale;
            if (reach > 0.0f && tooClose(x, z, reach))
            {
                ++result.rejectedSpacing;
                continue;
            }
            if (blocked && blocked(Float3{x, y, z}, meshRadius * scale))
            {
                ++result.rejectedBlocked;
                continue;
            }
            const Float4x4 rotation = layer.alignToNormal ? FrameFromUp(normal, yaw)
                                                          : Float4x4::RotationY(yaw);
            out.PushBack(Float4x4::Scale(Float3{scale, scale, scale}) * rotation *
                         Float4x4::Translation(Float3{x, y, z}));
            ++result.placed;
        }
        return result;
    }

    u32 EraseInstancesInDisc(Array<Float4x4>& instances, f32 centreX, f32 centreZ,
                             f32 radius) noexcept
    {
        u32 removed = 0;
        usize w = 0;
        for (usize i = 0; i < instances.Size(); ++i)
        {
            const f32 dx = instances[i].m[3][0] - centreX;
            const f32 dz = instances[i].m[3][2] - centreZ;
            if (dx * dx + dz * dz <= radius * radius)
            {
                ++removed;
                continue;
            }
            if (w != i)
            {
                instances[w] = instances[i];
            }
            ++w;
        }
        instances.Resize(w);
        return removed;
    }

    void ChunksTouchedBy(const heightfield::HeightfieldRegion& region, i32 chunksPerSide,
                         Array<u32>& outChunkIndices)
    {
        if (region.IsEmpty() || chunksPerSide <= 0)
        {
            return;
        }
        const i32 last = chunksPerSide - 1;
        // A boundary sample (gx = k * 64) belongs to chunk k-1's far edge and chunk k's near edge.
        const i32 cx0 = Clamp((region.minX - 1) / terrain::kChunkQuads, 0, last);
        const i32 cx1 = Clamp(region.maxX / terrain::kChunkQuads, 0, last);
        const i32 cz0 = Clamp((region.minZ - 1) / terrain::kChunkQuads, 0, last);
        const i32 cz1 = Clamp(region.maxZ / terrain::kChunkQuads, 0, last);
        for (i32 cz = cz0; cz <= cz1; ++cz)
        {
            for (i32 cx = cx0; cx <= cx1; ++cx)
            {
                const u32 index = static_cast<u32>(cz * chunksPerSide + cx);
                bool seen = false;
                for (u32 existing : outChunkIndices)
                {
                    if (existing == index)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    outChunkIndices.PushBack(index);
                }
            }
        }
    }
}
