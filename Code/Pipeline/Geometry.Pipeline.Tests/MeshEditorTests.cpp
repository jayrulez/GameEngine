// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Source-side mesh cook: capture a primitive into a StaticMeshAsset, cook it through
// the builder into an output DB, and verify the cooked StaticMeshSource carries the
// vertex and index data. Also the skinned path.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import pipeline.core;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.xml.serialization; // XmlSerializerFactory (sidecar tests use text envelopes)
import geometry.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::geometry;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_mesh_edit_db/cube.rasset");
        FileDelete(u8"scratch_mesh_edit_db/skinned.rasset");
        RemoveDirectory(u8"scratch_mesh_edit_db");
    }
}

TEST_CASE("mesh editor: cooks a StaticMeshAsset -> StaticMeshSource")
{
    GlobalTypeRegistry().Register(StaticMeshSource::StaticType());
    RegisterSerializable<StaticMeshSource>();
    RegisterMeshAssets();

    RemoveTree();
    NativeFileSystem outMount(u8"scratch_mesh_edit_db", DefaultAllocator());
    Guid id;

    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"cube", StaticMeshSource::StaticType());
        id = inst->Id();

        RefPtr<StaticMesh> cube = Primitives::Cube(DefaultAllocator(), 1.0f);
        StaticMeshAsset asset;
        MeshImporter::Import(*cube, asset);

        StaticMeshAssetBuilder builder;
        REQUIRE(builder.AssetType() == &StaticMeshAsset::StaticType());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        RefPtr<ISerializable> object = outDb.ReadObject(id);
        StaticMeshSource* cooked = Cast<StaticMeshSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->vertexBlob.Size() == 24 * sizeof(StaticMeshVertex));
        CHECK(cooked->indexData.Size() == 36);
        CHECK(cooked->subStart.Size() == 1);
    }

    RemoveTree();
}

TEST_CASE("mesh editor: cooks a SkinnedMeshAsset -> SkinnedMeshSource")
{
    GlobalTypeRegistry().Register(SkinnedMeshSource::StaticType());
    RegisterSerializable<SkinnedMeshSource>();
    RegisterMeshAssets();

    RemoveTree();
    NativeFileSystem outMount(u8"scratch_mesh_edit_db", DefaultAllocator());
    Guid id;

    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst =
            outDb.RootGroup()->CreateInstance(u8"skinned", SkinnedMeshSource::StaticType());
        id = inst->Id();

        RefPtr<SkinnedMesh> mesh = MakeRef<SkinnedMesh>(DefaultAllocator());
        mesh->skeletonIndex = 4;
        mesh->vertices.PushBack(StaticMeshVertex{});
        mesh->vertices.PushBack(StaticMeshVertex{});
        VertexSkinning s{};
        s.joints[1] = 9;
        mesh->skinning.PushBack(s);
        mesh->skinning.PushBack(s);

        SkinnedMeshAsset asset;
        MeshImporter::Import(*mesh, asset);

        SkinnedMeshAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        RefPtr<ISerializable> object = outDb.ReadObject(id);
        SkinnedMeshSource* cooked = Cast<SkinnedMeshSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->vertexBlob.Size() == 2 * sizeof(StaticMeshVertex));
        CHECK(cooked->skinningBlob.Size() == 2 * sizeof(VertexSkinning));
        CHECK(cooked->skeletonIndex == 4);
    }

    RemoveTree();
}

TEST_CASE("mesh editor: sidecar round-trip - tiny XML envelope, binary geometry stream")
{
    // Inline mesh geometry makes a large XML envelope that every project open DOM-parses for a
    // few header fields, so the envelope holds metadata and the geometry is a BinarySerializer
    // sidecar stream. This pins both halves: the envelope stays tiny, and the sidecar loads
    // back byte-faithful.
    GlobalTypeRegistry().Register(StaticMeshSource::StaticType());
    RegisterSerializable<StaticMeshSource>();
    RegisterMeshAssets();

    (void)RemoveDirectoryRecursive(u8"scratch_mesh_sidecar_db");
    NativeFileSystem mount(u8"scratch_mesh_sidecar_db", DefaultAllocator());
    Guid id;
    usize originalVertexCount = 0;

    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"sphere", StaticMeshAsset::StaticType());
        id = inst->Id();

        RefPtr<StaticMesh> sphere = Primitives::Sphere(DefaultAllocator(), 1.0f, 32, 32); // real bulk
        StaticMeshAsset asset;
        MeshImporter::Import(*sphere, asset);
        originalVertexCount = asset.source.vertexBlob.Size();
        REQUIRE(originalVertexCount > 0);
        REQUIRE(WriteMeshAsset(*inst, asset).IsOk());
    }

    // The envelope does not scale with geometry.
    {
        UniquePtr<IStream> envelope =
            mount.Open(u8"sphere.xasset", FileMode::Read);
        REQUIRE(envelope);
        CHECK(envelope->Size() < 4 * 1024); // a 32x32 sphere inline is about 500 KB
    }

    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        foundation::content::Instance* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* asset = Cast<StaticMeshAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->source.vertexBlob.IsEmpty()); // the envelope carries no bulk
        REQUIRE(EnsureMeshSourceLoaded(*inst, *asset).IsOk());
        CHECK(asset->source.vertexBlob.Size() == originalVertexCount);

        // The builder path: cook from the sidecar-backed source instance.
        auto* out = db.RootGroup()->CreateInstance(u8"sphere_cooked",
                                                   StaticMeshSource::StaticType());
        StaticMeshAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.source = inst;
        ctx.output = out;
        RefPtr<ISerializable> fresh = inst->ReadObject(); // unloaded copy; Build must load it
        auto* freshAsset = Cast<StaticMeshAsset>(fresh.Get());
        REQUIRE(freshAsset != nullptr);
        REQUIRE(builder.Build(*freshAsset, ctx).IsOk());
    }

    (void)RemoveDirectoryRecursive(u8"scratch_mesh_sidecar_db");
}

