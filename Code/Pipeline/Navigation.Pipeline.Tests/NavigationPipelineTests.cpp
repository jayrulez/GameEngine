// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full navigation pipeline: bake a navmesh into a NavigationZoneAsset (the blob in a sidecar
// stream), cook it through the builder into an output db, load the NavigationZoneResource product
// through the factory, and path across it with a query. Also shows the blob is not in the
// (text-capable) envelope.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.navigation;
import foundation.navigation.resource;
import navigation.pipeline;

using namespace foundation::core;
using namespace foundation::navigation;
namespace content = foundation::content;
using foundation::resource::ResourceManager;
using foundation::resource::Proxy;

namespace
{
    void RemoveTree(StringView root)
    {
        foundation::vfs::NativeFileSystem fs(root, foundation::core::DefaultAllocator());
        Array<foundation::vfs::DirEntry> entries;
        if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            for (const auto& e : entries)
            {
                if (!e.isDirectory)
                {
                    (void)fs.AsWritable()->Delete(e.name.AsView());
                }
            }
        }
        (void)RemoveDirectory(root);
    }

    // A 20x20 ground quad on y=0, wound +Y (both triangles walkable).
    void GroundSoup(Array<Float3>& verts, Array<u32>& indices)
    {
        verts.PushBack(Float3{-10, 0, -10});
        verts.PushBack(Float3{10, 0, -10});
        verts.PushBack(Float3{10, 0, 10});
        verts.PushBack(Float3{-10, 0, 10});
        const u32 t[] = {0, 3, 2, 0, 2, 1};
        for (u32 i : t)
        {
            indices.PushBack(i);
        }
    }
}

TEST_CASE("navigation.pipeline: bake -> asset (sidecar) -> cook -> product -> query")
{
    pipeline::RegisterNavigationZoneAsset();
    RegisterNavigationResource();
    RemoveTree(u8"scratch_navpipe_src_db");
    RemoveTree(u8"scratch_navpipe_out_db");

    foundation::vfs::NativeFileSystem srcMount(u8"scratch_navpipe_src_db", foundation::core::DefaultAllocator());
    foundation::vfs::NativeFileSystem outMount(u8"scratch_navpipe_out_db", foundation::core::DefaultAllocator());
    content::ContentDatabase srcDb(DefaultAllocator(), srcMount, BinarySerializerFactory(), u8".rasset");
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");

    // Bake a navmesh blob from a ground plane.
    Array<Float3> verts;
    Array<u32> indices;
    GroundSoup(verts, indices);
    Array<byte> blob;
    REQUIRE(NavigationMeshBuilder::Build(Span<const Float3>{verts.Data(), verts.Size()},
                                         Span<const u32>{indices.Data(), indices.Size()},
                                         NavigationBakeParams{}, blob)
                .IsOk());
    REQUIRE(blob.Size() > 0);

    // Author the source asset: blob written to the sidecar, not inline in the envelope.
    auto* zoneInstance =
        srcDb.RootGroup()->CreateInstance(u8"zone", pipeline::NavigationZoneAsset::StaticType());
    REQUIRE(zoneInstance != nullptr);
    {
        pipeline::NavigationZoneAsset asset;
        asset.navMeshBlob.Resize(blob.Size());
        MemCopy(asset.navMeshBlob.Data(), blob.Data(), blob.Size());
        REQUIRE(pipeline::WriteNavigationZoneAsset(*zoneInstance, asset).IsOk());
    }

    // The envelope alone carries no blob; the sidecar restores it.
    {
        RefPtr<ISerializable> object = zoneInstance->ReadObject();
        auto* readBack = Cast<pipeline::NavigationZoneAsset>(object.Get());
        REQUIRE(readBack != nullptr);
        CHECK(readBack->navMeshBlob.IsEmpty()); // not inline in the (text-capable) envelope
        REQUIRE(pipeline::EnsureNavMeshLoaded(*zoneInstance, *readBack).IsOk());
        CHECK(readBack->navMeshBlob.Size() == blob.Size()); // but the sidecar has it
    }

    // Cook through the builder into the output db.
    pipeline::NavigationZoneAssetBuilder builder;
    {
        pipeline::NavigationZoneAsset asset;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.db = &srcDb;
        ctx.source = zoneInstance; // the sidecar stream lives on the source instance

        pipeline::AssetDependencies deps;
        builder.ScanDependencies(asset, ctx, deps);
        REQUIRE(deps.sourceStreams.Size() == 1u);
        CHECK(deps.sourceStreams[0] == StringView(u8"navmesh"));

        auto* outInstance = outDb.RootGroup()->CreateInstance(
            u8"zone", NavigationZoneSource::StaticType());
        ctx.output = outInstance;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        // The cooked product carries the same blob.
        RefPtr<ISerializable> object = outInstance->ReadObject();
        auto* src = Cast<NavigationZoneSource>(object.Get());
        REQUIRE(src != nullptr);
        CHECK(src->navMeshBlob.Size() == blob.Size());
    }

    // Load the runtime product through the factory and path across it.
    {
        NavigationZoneFactory factory(DefaultAllocator());
        ResourceManager manager(DefaultAllocator(), outDb);
        manager.AddFactory(&factory);
        auto* outInstance = outDb.RootGroup()->GetInstance(u8"zone");
        REQUIRE(outInstance != nullptr);
        Proxy<NavigationZoneResource> zone = manager.Bind<NavigationZoneResource>(outInstance->Id());
        REQUIRE(zone);
        REQUIRE(zone->IsValid());

        NavigationMeshQuery query(DefaultAllocator(), zone->mesh);
        REQUIRE(query.IsValid());
        NavigationPath path;
        REQUIRE(query.FindPath(Float3{-8, 0, 0}, Float3{8, 0, 0}, path).IsOk());
        CHECK(path.complete);
    }

    RemoveTree(u8"scratch_navpipe_src_db");
    RemoveTree(u8"scratch_navpipe_out_db");
}
