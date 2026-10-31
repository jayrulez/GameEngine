// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cook-time optimization pass. Everything is a pure reorder, so these tests pin the
// invariants (triangle set, vertex values, submesh ranges and non-triangle index streams
// survive as sets or position sequences), the wins (ACMR improves on a cache-hostile
// fixture; unused vertices compact away), and the guards (malformed input passes through
// untouched; the pass is idempotent).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

#include <algorithm>
#include <vector>

import foundation.core;
import foundation.geometry;
import foundation.geometry.resource;
import geometry.pipeline;

using namespace foundation::core;
using namespace foundation::geometry;
using namespace pipeline;

namespace
{
    // A cache-hostile grid: (n+1)^2 vertices, triangles emitted column-major over a
    // row-major vertex layout, so consecutive triangles jump n+1 vertices apart: a worst
    // case the optimizer must visibly improve.
    void BuildHostileGrid(u32 n, StaticMeshSource& out)
    {
        const u32 side = n + 1;
        Array<StaticMeshVertex> vertices;
        for (u32 z = 0; z < side; ++z)
        {
            for (u32 x = 0; x < side; ++x)
            {
                StaticMeshVertex v;
                v.position = Float3{static_cast<f32>(x), 0.0f, static_cast<f32>(z)};
                v.texCoord = Float2{static_cast<f32>(x), static_cast<f32>(z)};
                vertices.PushBack(v);
            }
        }
        out.vertexBlob.Resize(vertices.Size() * sizeof(StaticMeshVertex));
        MemCopy(out.vertexBlob.Data(), vertices.Data(), out.vertexBlob.Size());
        out.indexData.Clear();
        for (u32 x = 0; x < n; ++x) // column-major emission: hostile
        {
            for (u32 z = 0; z < n; ++z)
            {
                const u32 a = z * side + x;
                const u32 b = z * side + x + 1;
                const u32 c = (z + 1) * side + x;
                const u32 d = (z + 1) * side + x + 1;
                out.indexData.PushBack(a);
                out.indexData.PushBack(c);
                out.indexData.PushBack(b);
                out.indexData.PushBack(b);
                out.indexData.PushBack(c);
                out.indexData.PushBack(d);
            }
        }
        out.subStart.Clear();
        out.subCount.Clear();
        out.subMaterial.Clear();
        out.subPrim.Clear();
        out.subStart.PushBack(0);
        out.subCount.PushBack(static_cast<i32>(out.indexData.Size()));
        out.subMaterial.PushBack(0);
        out.subPrim.PushBack(static_cast<u8>(PrimitiveType::Triangles));
    }

    [[nodiscard]] Float3 PositionOf(const StaticMeshSource& s, u32 index)
    {
        const auto* v = reinterpret_cast<const StaticMeshVertex*>(
            s.vertexBlob.Data() + static_cast<usize>(index) * sizeof(StaticMeshVertex));
        return v->position;
    }

    struct Tri
    {
        Float3 a, b, c;
    };
    [[nodiscard]] bool Less(const Float3& l, const Float3& r)
    {
        if (l.x != r.x)
        {
            return l.x < r.x;
        }
        if (l.y != r.y)
        {
            return l.y < r.y;
        }
        return l.z < r.z;
    }
    // Winding-preserving canonical form: rotate so the smallest position leads.
    [[nodiscard]] Tri Canonical(Tri t)
    {
        while (Less(t.b, t.a) || Less(t.c, t.a))
        {
            const Float3 first = t.a;
            t.a = t.b;
            t.b = t.c;
            t.c = first;
        }
        return t;
    }
    // The triangle set of a submesh range, as position triples (index values may be
    // remapped freely; geometry and winding may not change).
    [[nodiscard]] std::vector<Tri> TriangleSet(const StaticMeshSource& s, usize submesh)
    {
        std::vector<Tri> tris;
        const i32 start = s.subStart[submesh];
        const i32 count = s.subCount[submesh];
        for (i32 i = 0; i + 2 < count; i += 3)
        {
            tris.push_back(Canonical(Tri{PositionOf(s, s.indexData[start + i]),
                                         PositionOf(s, s.indexData[start + i + 1]),
                                         PositionOf(s, s.indexData[start + i + 2])}));
        }
        std::sort(tris.begin(), tris.end(),
                  [](const Tri& l, const Tri& r)
                  {
                      if (!(l.a == r.a))
                      {
                          return Less(l.a, r.a);
                      }
                      if (!(l.b == r.b))
                      {
                          return Less(l.b, r.b);
                      }
                      return Less(l.c, r.c);
                  });
        return tris;
    }
    [[nodiscard]] bool SameTriangles(const std::vector<Tri>& l, const std::vector<Tri>& r)
    {
        if (l.size() != r.size())
        {
            return false;
        }
        for (usize i = 0; i < l.size(); ++i)
        {
            if (!(l[i].a == r[i].a && l[i].b == r[i].b && l[i].c == r[i].c))
            {
                return false;
            }
        }
        return true;
    }
}

TEST_CASE("mesh optimize: reorder preserves triangles/ranges/vertices and improves ACMR")
{
    StaticMeshSource source;
    BuildHostileGrid(24, source);
    const std::vector<Tri> before = TriangleSet(source, 0);
    const usize vertexBytes = source.vertexBlob.Size();
    const usize indexCount = source.indexData.Size();

    pipeline::MeshOptimizeStats stats;
    pipeline::OptimizeStaticMeshSource(source, &stats);

    // Ranges and sizes are untouched (every vertex is referenced, so nothing compacts).
    CHECK(source.subStart[0] == 0);
    CHECK(source.subCount[0] == static_cast<i32>(indexCount));
    CHECK(source.indexData.Size() == indexCount);
    CHECK(source.vertexBlob.Size() == vertexBytes);
    CHECK(stats.triangleSubmeshes == 1);
    CHECK(stats.verticesBefore == stats.verticesAfter);

    // Same triangles (as position triples, winding preserved), in a better order.
    CHECK(SameTriangles(before, TriangleSet(source, 0)));

    // The hostile order must measurably improve.
    CHECK(stats.acmrAfter < stats.acmrBefore);

    // Idempotent: a second pass keeps the triangle set and does not regress ACMR.
    pipeline::MeshOptimizeStats again;
    pipeline::OptimizeStaticMeshSource(source, &again);
    CHECK(SameTriangles(before, TriangleSet(source, 0)));
    CHECK(again.acmrAfter <= stats.acmrAfter + 1e-6f);
}

TEST_CASE("mesh optimize: non-triangle submeshes keep their order; unused vertices compact")
{
    // Two submeshes, a triangle pair and a lines range, and one vertex nothing references.
    StaticMeshSource source;
    Array<StaticMeshVertex> vertices;
    for (u32 i = 0; i < 6; ++i)
    {
        StaticMeshVertex v;
        v.position = Float3{static_cast<f32>(i), static_cast<f32>(i * 2), 0.0f};
        vertices.PushBack(v);
    }
    // vertex 5 is unused: the fetch remap must compact it away.
    source.vertexBlob.Resize(vertices.Size() * sizeof(StaticMeshVertex));
    MemCopy(source.vertexBlob.Data(), vertices.Data(), source.vertexBlob.Size());
    const u32 triangleIndices[] = {0, 1, 2, 2, 1, 3};
    const u32 lineIndices[] = {4, 0, 3, 2};
    for (u32 i : triangleIndices)
    {
        source.indexData.PushBack(i);
    }
    for (u32 i : lineIndices)
    {
        source.indexData.PushBack(i);
    }
    source.subStart.PushBack(0);
    source.subCount.PushBack(6);
    source.subMaterial.PushBack(0);
    source.subPrim.PushBack(static_cast<u8>(PrimitiveType::Triangles));
    source.subStart.PushBack(6);
    source.subCount.PushBack(4);
    source.subMaterial.PushBack(1);
    source.subPrim.PushBack(static_cast<u8>(PrimitiveType::Lines));

    const std::vector<Tri> before = TriangleSet(source, 0);
    Array<Float3> linePositionsBefore;
    for (u32 i = 0; i < 4; ++i)
    {
        linePositionsBefore.PushBack(PositionOf(source, source.indexData[6 + i]));
    }

    pipeline::MeshOptimizeStats stats;
    pipeline::OptimizeStaticMeshSource(source, &stats);

    CHECK(stats.triangleSubmeshes == 1);      // the lines range was not reordered
    CHECK(stats.verticesBefore == 6);
    CHECK(stats.verticesAfter == 5);          // the unused vertex is gone
    CHECK(source.vertexBlob.Size() == 5 * sizeof(StaticMeshVertex));
    CHECK(SameTriangles(before, TriangleSet(source, 0)));
    // The lines range: index values were remapped, but the position sequence (order
    // included, since line segments are order-sensitive) is identical.
    for (u32 i = 0; i < 4; ++i)
    {
        CHECK(PositionOf(source, source.indexData[6 + i]) == linePositionsBefore[i]);
    }
    // Every index is in range of the compacted buffer.
    for (u32 index : source.indexData)
    {
        CHECK(index < 5);
    }
}

TEST_CASE("mesh optimize: malformed input passes through untouched")
{
    // Out-of-range index: the pass must refuse (warn) and change nothing.
    StaticMeshSource bad;
    StaticMeshVertex v;
    bad.vertexBlob.Resize(sizeof(StaticMeshVertex));
    MemCopy(bad.vertexBlob.Data(), &v, sizeof(StaticMeshVertex));
    bad.indexData.PushBack(0);
    bad.indexData.PushBack(7); // out of range
    bad.indexData.PushBack(0);
    bad.subStart.PushBack(0);
    bad.subCount.PushBack(3);
    bad.subMaterial.PushBack(0);
    bad.subPrim.PushBack(static_cast<u8>(PrimitiveType::Triangles));
    pipeline::MeshOptimizeStats stats;
    pipeline::OptimizeStaticMeshSource(bad, &stats);
    CHECK(bad.indexData[1] == 7); // untouched
    CHECK(stats.triangleSubmeshes == 0);

    // Bad submesh window: same refusal.
    StaticMeshSource badRange;
    badRange.vertexBlob.Resize(sizeof(StaticMeshVertex));
    MemCopy(badRange.vertexBlob.Data(), &v, sizeof(StaticMeshVertex));
    badRange.indexData.PushBack(0);
    badRange.subStart.PushBack(0);
    badRange.subCount.PushBack(9); // exceeds the index buffer
    badRange.subMaterial.PushBack(0);
    badRange.subPrim.PushBack(static_cast<u8>(PrimitiveType::Triangles));
    pipeline::OptimizeStaticMeshSource(badRange, &stats);
    CHECK(badRange.indexData.Size() == 1);
    CHECK(stats.triangleSubmeshes == 0);

    // Empty mesh: a no-op, zeroed stats.
    StaticMeshSource empty;
    pipeline::OptimizeStaticMeshSource(empty, &stats);
    CHECK(stats.verticesBefore == 0);
    CHECK(stats.triangleSubmeshes == 0);
}

TEST_CASE("mesh optimize: the builder runs the pass (cooked cube stays a cube)")
{
    // The builder-level guarantee: counts survive (the cook test pins them), and the cooked
    // triangle set equals the source's, through the real Build path.
    RegisterMeshAssets();
    RefPtr<StaticMesh> cube = Primitives::Cube(DefaultAllocator(), 1.0f);
    StaticMeshAsset asset;
    MeshImporter::Import(*cube, asset);
    const std::vector<Tri> before = TriangleSet(asset.source, 0);

    // Sources are Object-derived (no copy): import the same cube again and optimize
    // that instance: what Build writes is exactly the optimized source.
    StaticMeshAsset cookedAsset;
    MeshImporter::Import(*cube, cookedAsset);
    pipeline::OptimizeStaticMeshSource(cookedAsset.source);
    CHECK(cookedAsset.source.vertexBlob.Size() == asset.source.vertexBlob.Size());
    CHECK(cookedAsset.source.indexData.Size() == asset.source.indexData.Size());
    CHECK(SameTriangles(before, TriangleSet(cookedAsset.source, 0)));
}

TEST_CASE("mesh lod generate: a dense grid grows a halving chain; small/authored meshes do not")
{
    StaticMeshSource source;
    BuildHostileGrid(48, source); // 4608 triangles: room for three halvings above the floor
    const usize lod0Indices = source.indexData.Size();

    const u32 added = GenerateLodChain(source);
    CHECK(added >= 1);
    CHECK(source.lodCount == 1 + added);
    REQUIRE(source.lodStart.Size() == added);          // one submesh per level
    REQUIRE(source.lodCoverage.Size() == source.lodCount);
    CHECK(source.lodCoverage[1] == doctest::Approx(0.25f));

    // LOD 0 untouched; each level lands under ~60% of its predecessor (the halving
    // target with acceptance slack) and every index stays in range.
    CHECK(source.subCount[0] == static_cast<i32>(lod0Indices));
    usize prev = lod0Indices;
    for (u32 l = 0; l < added; ++l)
    {
        const usize count = static_cast<usize>(source.lodIndexCount[l]);
        CHECK(count > 0);
        CHECK(count <= (prev * 3) / 4);
        prev = count;
    }
    const usize vertexCount = source.vertexBlob.Size() / sizeof(StaticMeshVertex);
    for (const u32 index : source.indexData)
    {
        CHECK(index < vertexCount);
    }
    // The chain survives the runtime fill and the optimizer pass.
    pipeline::MeshOptimizeStats stats;
    pipeline::OptimizeStaticMeshSource(source, &stats);
    foundation::geometry::StaticMesh mesh;
    source.FillStatic(mesh);
    CHECK(mesh.lodCount == source.lodCount);

    // Authored or existing chains are never regenerated over.
    CHECK(GenerateLodChain(source) == 0);

    // A tiny mesh (two triangles) stays chainless: it is below any sensible floor.
    StaticMeshSource tiny;
    BuildHostileGrid(1, tiny);
    CHECK(GenerateLodChain(tiny) == 0);
    CHECK(tiny.lodCount == 1);
}

TEST_CASE("mesh optimize: a skinned source's parallel stream follows the vertex permutation")
{
    // Vertex i sits at x = i and carries joints[0] = i: after the fetch remap and compaction
    // the pairing must survive for every vertex, however the permutation shuffled them.
    SkinnedMeshSource source;
    Array<StaticMeshVertex> vertices;
    Array<VertexSkinning> skinning;
    for (u32 i = 0; i < 6; ++i)
    {
        StaticMeshVertex v;
        v.position = Float3{static_cast<f32>(i), 0.0f, 0.0f};
        vertices.PushBack(v);
        VertexSkinning s{};
        s.joints[0] = static_cast<u16>(i);
        skinning.PushBack(s);
    }
    source.vertexBlob.Resize(vertices.Size() * sizeof(StaticMeshVertex));
    MemCopy(source.vertexBlob.Data(), vertices.Data(), source.vertexBlob.Size());
    source.skinningBlob.Resize(skinning.Size() * sizeof(VertexSkinning));
    MemCopy(source.skinningBlob.Data(), skinning.Data(), source.skinningBlob.Size());
    // Two triangles; vertex 5 is dead and must compact away from both streams.
    const u32 indices[] = {0, 1, 2, 2, 1, 3, 0, 3, 4};
    for (u32 i : indices)
    {
        source.indexData.PushBack(i);
    }
    source.subStart.PushBack(0);
    source.subCount.PushBack(9);
    source.subMaterial.PushBack(0);
    source.subPrim.PushBack(static_cast<u8>(PrimitiveType::Triangles));

    pipeline::MeshOptimizeStats stats;
    pipeline::OptimizeStaticMeshSource(source, &stats);
    CHECK(stats.verticesAfter == 5);
    CHECK(source.skinningBlob.Size() == 5 * sizeof(VertexSkinning));
    const auto* outVerts =
        reinterpret_cast<const StaticMeshVertex*>(source.vertexBlob.Data());
    const auto* outSkin =
        reinterpret_cast<const VertexSkinning*>(source.skinningBlob.Data());
    for (usize i = 0; i < 5; ++i)
    {
        CHECK(outSkin[i].joints[0] == static_cast<u16>(outVerts[i].position.x));
    }

    // A mismatched parallel stream refuses the whole pass (nothing moves).
    SkinnedMeshSource bad;
    bad.vertexBlob = source.vertexBlob;
    bad.indexData = source.indexData;
    bad.subStart = source.subStart;
    bad.subCount = source.subCount;
    bad.subMaterial = source.subMaterial;
    bad.subPrim = source.subPrim;
    bad.skinningBlob.Resize(3 * sizeof(VertexSkinning)); // wrong size
    const u32 firstBefore = bad.indexData[0];
    pipeline::OptimizeStaticMeshSource(bad, &stats);
    CHECK(bad.indexData[0] == firstBefore);
    CHECK(stats.triangleSubmeshes == 0);
}
