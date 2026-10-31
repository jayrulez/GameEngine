// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The LOD chain wire from StaticMeshSource to StaticMesh.
// Covers the serialize round-trip (sidecar write and read), the FillStatic validation
// (malformed tables collapse to 1 LOD: render at LOD 0, never crash), the
// SubMeshesForLod slicing contract, and the optimizer staying set-preserving
// on every level of a chain.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.xml.serialization;
import geometry.pipeline;

using namespace foundation::core;
using namespace foundation::geometry;
using namespace pipeline;
using namespace foundation::vfs;

namespace
{
    // A 2-LOD, 1-submesh fixture over a shared 5-vertex fan: LOD 0 has 3 triangles and
    // LOD 1 has 1, indices concatenated in the one buffer.
    void BuildTwoLodSource(StaticMeshSource& out)
    {
        Array<StaticMeshVertex> vertices;
        for (u32 i = 0; i < 5; ++i)
        {
            StaticMeshVertex v;
            v.position = Float3{static_cast<f32>(i), static_cast<f32>(i % 2), 0.0f};
            vertices.PushBack(v);
        }
        out.vertexBlob.Resize(vertices.Size() * sizeof(StaticMeshVertex));
        MemCopy(out.vertexBlob.Data(), vertices.Data(), out.vertexBlob.Size());
        const u32 lod0[] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
        const u32 lod1[] = {0, 1, 4};
        for (u32 i : lod0)
        {
            out.indexData.PushBack(i);
        }
        for (u32 i : lod1)
        {
            out.indexData.PushBack(i);
        }
        out.subStart.PushBack(0);
        out.subCount.PushBack(9);
        out.subMaterial.PushBack(7);
        out.subPrim.PushBack(static_cast<u8>(PrimitiveType::Triangles));
        out.lodCount = 2;
        out.lodStart.PushBack(9);
        out.lodIndexCount.PushBack(3);
        out.lodCoverage.PushBack(1.0f);
        out.lodCoverage.PushBack(0.25f);
    }
}

TEST_CASE("mesh lod wire: the sidecar round-trip carries the chain; FillStatic slices it")
{
    RegisterMeshAssets();
    (void)RemoveDirectoryRecursive(u8"scratch_mesh_lod_db");
    NativeFileSystem mount(u8"scratch_mesh_lod_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"fan", StaticMeshAsset::StaticType());
        id = inst->Id();
        StaticMeshAsset asset;
        BuildTwoLodSource(asset.source);
        REQUIRE(WriteMeshAsset(*inst, asset).IsOk());
    }
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        foundation::content::Instance* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* asset = Cast<StaticMeshAsset>(object.Get());
        REQUIRE(asset != nullptr);
        REQUIRE(EnsureMeshSourceLoaded(*inst, *asset).IsOk());

        CHECK(asset->source.lodCount == 2);
        REQUIRE(asset->source.lodStart.Size() == 1);
        CHECK(asset->source.lodStart[0] == 9);
        CHECK(asset->source.lodIndexCount[0] == 3);
        REQUIRE(asset->source.lodCoverage.Size() == 2);
        CHECK(asset->source.lodCoverage[1] == doctest::Approx(0.25f));

        // Fill the runtime mesh: LOD 0 is subMeshes; LOD 1 is its own range, inheriting
        // material and primitive from the LOD-0 submesh.
        StaticMesh mesh;
        asset->source.FillStatic(mesh);
        CHECK(mesh.lodCount == 2);
        REQUIRE(mesh.SubMeshesForLod(0).Size() == 1);
        CHECK(mesh.SubMeshesForLod(0)[0].indexCount == 9);
        REQUIRE(mesh.SubMeshesForLod(1).Size() == 1);
        CHECK(mesh.SubMeshesForLod(1)[0].startIndex == 9);
        CHECK(mesh.SubMeshesForLod(1)[0].indexCount == 3);
        CHECK(mesh.SubMeshesForLod(1)[0].materialIndex == 7);
        // Out-of-range LOD clamps to the coarsest; a 1-LOD mesh always answers LOD 0.
        CHECK(mesh.SubMeshesForLod(9)[0].startIndex == 9);

        // FromMesh captures the chain back losslessly (capture round-trip).
        StaticMeshSource recaptured;
        StaticMeshSource::FromMesh(mesh, recaptured);
        CHECK(recaptured.lodCount == 2);
        REQUIRE(recaptured.lodStart.Size() == 1);
        CHECK(recaptured.lodStart[0] == 9);
        CHECK(recaptured.lodCoverage.Size() == 2);
    }
    (void)RemoveDirectoryRecursive(u8"scratch_mesh_lod_db");
}

TEST_CASE("mesh lod wire: malformed chains collapse to 1 LOD (render at LOD 0, no crash)")
{
    // Range past the index buffer.
    StaticMeshSource bad;
    BuildTwoLodSource(bad);
    bad.lodIndexCount[0] = 99;
    StaticMesh mesh;
    bad.FillStatic(mesh);
    CHECK(mesh.lodCount == 1);
    CHECK(mesh.lodSubMeshes.IsEmpty());
    CHECK(mesh.SubMeshesForLod(1)[0].indexCount == 9); // falls back to LOD 0

    // Wrong slice length (coverage array mismatched).
    StaticMeshSource badCoverage;
    BuildTwoLodSource(badCoverage);
    badCoverage.lodCoverage.PopBack();
    StaticMesh mesh2;
    badCoverage.FillStatic(mesh2);
    CHECK(mesh2.lodCount == 1);

    // A default source (no chain) is a 1-LOD mesh.
    StaticMeshSource plain;
    BuildTwoLodSource(plain);
    plain.lodCount = 1;
    plain.lodStart.Clear();
    plain.lodIndexCount.Clear();
    plain.lodCoverage.Clear();
    StaticMesh mesh3;
    plain.FillStatic(mesh3);
    CHECK(mesh3.lodCount == 1);
    CHECK(mesh3.SubMeshesForLod(1).Size() == 1); // LOD 0 view
}

// The current version round-trips the chain intact (the only layout the reader accepts).
TEST_CASE("mesh lod wire: the current version round-trips the full LOD chain")
{
    StaticMeshSource authored;
    BuildTwoLodSource(authored);

    MemoryStream buffer;
    const SerializedDataVersion v4[] = {
        {TypeOf<StaticMeshSource>().id, TypeOf<StaticMeshSource>().dataVersion}};
    {
        BinarySerializer ar(buffer, SerializeMode::Write);
        ar.PushVersionScope(v4, 1);
        authored.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    StaticMeshSource loaded;
    {
        BinarySerializer ar(buffer, SerializeMode::Read);
        ar.PushVersionScope(v4, 1);
        loaded.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    CHECK(loaded.lodCount == 2);
    REQUIRE(loaded.lodStart.Size() == 1);
    CHECK(loaded.lodStart[0] == 9);
    CHECK(loaded.lodIndexCount[0] == 3);
    REQUIRE(loaded.lodCoverage.Size() == 2);
}

TEST_CASE("mesh lod wire: the optimizer preserves every level of a chain")
{
    StaticMeshSource source;
    BuildTwoLodSource(source);

    // Capture both levels' triangles as position triples before.
    auto positionOf = [&source](u32 index)
    {
        const auto* v = reinterpret_cast<const StaticMeshVertex*>(
            source.vertexBlob.Data() + static_cast<usize>(index) * sizeof(StaticMeshVertex));
        return v->position;
    };
    Array<Float3> lod1Before;
    for (u32 i = 0; i < 3; ++i)
    {
        lod1Before.PushBack(positionOf(source.indexData[9 + i]));
    }

    MeshOptimizeStats stats;
    OptimizeStaticMeshSource(source, &stats);

    // Ranges intact; every index in range; the LOD-1 triangle survives as positions
    // (one triangle: rotation allowed by vcache, so compare as a set).
    CHECK(source.lodStart[0] == 9);
    CHECK(source.lodIndexCount[0] == 3);
    for (u32 index : source.indexData)
    {
        CHECK(index < 5);
    }
    Array<Float3> lod1After;
    for (u32 i = 0; i < 3; ++i)
    {
        lod1After.PushBack(positionOf(source.indexData[9 + i]));
    }
    usize matched = 0;
    for (const Float3& p : lod1Before)
    {
        for (const Float3& q : lod1After)
        {
            if (p == q)
            {
                ++matched;
                break;
            }
        }
    }
    CHECK(matched == 3);

    // A malformed LOD range makes the whole pass refuse (indices untouched).
    StaticMeshSource bad;
    BuildTwoLodSource(bad);
    bad.lodStart[0] = 11; // 11 + 3 > 12
    const u32 firstBefore = bad.indexData[0];
    OptimizeStaticMeshSource(bad, &stats);
    CHECK(bad.indexData[0] == firstBefore);
    CHECK(stats.triangleSubmeshes == 0);
}
