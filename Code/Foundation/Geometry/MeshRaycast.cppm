// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Geometry - the `:mesh_raycast` partition.
///
/// A ray against a mesh's triangles on the CPU: the nearest hit on its base level (LOD 0, the
/// shape every coarser level approximates), both faces counted, with the triangle's corners, so
/// a caller can take the nearest corner (vertex snapping). The ray is in the mesh's own space; a
/// caller with a world ray transforms it by the inverse world matrix WITHOUT normalizing the
/// direction, so the hit's distance stays the world distance along the world ray.

module;
#include "Core/Prelude.h"

export module foundation.geometry:mesh_raycast;

import foundation.core;
import :types;
import :mesh;

using namespace foundation::core;

export namespace foundation::geometry
{
    /// Where a ray met a mesh, in the mesh's space.
    struct MeshRayHit
    {
        f32 distance = 0.0f; ///< along the ray, in units of the ray's direction
        Float3 position;
        Float3 normal;       ///< the triangle's, facing the ray
        Float3 corners[3];   ///< the hit triangle's corners
    };

    /// The ray's intersection with triangle (a, b, c), both faces; the distance in units of
    /// `direction`, or a negative value when it misses (Moller-Trumbore).
    [[nodiscard]] inline f32 RayTriangle(Float3 origin, Float3 direction, Float3 a, Float3 b, Float3 c) noexcept
    {
        const Float3 ab = b - a;
        const Float3 ac = c - a;
        const Float3 p = Cross(direction, ac);
        const f32 det = Dot(ab, p);
        if (Abs(det) < 1e-12f)
        {
            return -1.0f; // parallel to the triangle (or a degenerate one)
        }
        const f32 inv = 1.0f / det;
        const Float3 s = origin - a;
        const f32 u = Dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f)
        {
            return -1.0f;
        }
        const Float3 q = Cross(s, ab);
        const f32 v = Dot(direction, q) * inv;
        if (v < 0.0f || u + v > 1.0f)
        {
            return -1.0f;
        }
        return Dot(ac, q) * inv;
    }

    /// The nearest hit of the ray on `mesh`'s LOD 0 triangles within `maxDistance` (in units of
    /// `direction`); false when it misses or the mesh has no triangles on the CPU.
    [[nodiscard]] inline bool RaycastMesh(const StaticMesh& mesh, Float3 origin, Float3 direction, f32 maxDistance,
                                          MeshRayHit& out) noexcept
    {
        const bool indexed = mesh.IndexCount() > 0;
        const auto vertexAt = [&mesh, indexed](u32 index) -> Float3
        { return mesh.vertices[indexed ? mesh.indices.Get(index) : index].position; };
        const auto inRange = [&mesh, indexed](u32 index)
        { return (indexed ? mesh.indices.Get(index) : index) < mesh.VertexCount(); };

        bool hit = false;
        f32 best = maxDistance;
        const auto test = [&](u32 first, u32 count)
        {
            const u32 limit = indexed ? mesh.IndexCount() : mesh.VertexCount();
            for (u32 i = first; i + 2 < first + count && i + 2 < limit; i += 3)
            {
                if (!inRange(i) || !inRange(i + 1) || !inRange(i + 2))
                {
                    continue; // a malformed index never reads past the vertices
                }
                const Float3 a = vertexAt(i);
                const Float3 b = vertexAt(i + 1);
                const Float3 c = vertexAt(i + 2);
                const f32 t = RayTriangle(origin, direction, a, b, c);
                if (t >= 0.0f && t < best)
                {
                    best = t;
                    hit = true;
                    out.distance = t;
                    out.position = origin + direction * t;
                    Float3 n = Cross(b - a, c - a);
                    n = LengthSquared(n) > 0.0f ? Normalized(n) : Float3::UnitY;
                    out.normal = Dot(n, direction) > 0.0f ? -n : n;
                    out.corners[0] = a;
                    out.corners[1] = b;
                    out.corners[2] = c;
                }
            }
        };
        if (mesh.subMeshes.IsEmpty())
        {
            test(0, indexed ? mesh.IndexCount() : mesh.VertexCount());
        }
        for (const SubMesh& sub : mesh.subMeshes)
        {
            if (sub.primitiveType == PrimitiveType::Triangles && sub.startIndex >= 0 && sub.indexCount > 0)
            {
                test(static_cast<u32>(sub.startIndex), static_cast<u32>(sub.indexCount));
            }
        }
        return hit;
    }

    /// The corner of the hit triangle nearest the hit point.
    [[nodiscard]] inline Float3 NearestCorner(const MeshRayHit& hit) noexcept
    {
        Float3 nearest = hit.corners[0];
        f32 bestDistance = LengthSquared(hit.corners[0] - hit.position);
        for (u32 i = 1; i < 3; ++i)
        {
            const f32 d = LengthSquared(hit.corners[i] - hit.position);
            if (d < bestDistance)
            {
                bestDistance = d;
                nearest = hit.corners[i];
            }
        }
        return nearest;
    }
}
