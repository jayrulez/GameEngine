// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full heightfield asset pipeline: author a HeightfieldAsset (blank or a 16-bit heightmap), cook it
// with HeightfieldAssetBuilder into an output content DB, and load the cooked Heightfield through
// the ResourceManager. Also the resample helper and the size-snap guard.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.image;
import foundation.image.io;
import foundation.heightfield;
import foundation.heightfield.resource;
import heightfield.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::image;
using namespace foundation::heightfield;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_hfpipe_src.png");
        FileDelete(u8"scratch_hfpipe_out_db/hf.rasset");
        FileDelete(u8"scratch_hfpipe_out_db/hf.heights.bin");
        FileDelete(u8"scratch_hfpipe_out_db/hf.holes.bin");
        RemoveDirectory(u8"scratch_hfpipe_out_db");
    }

    // Cook a HeightfieldAsset through the builder into an output DB and bind the runtime product.
    Proxy<Heightfield> CookAndBind(const HeightfieldAsset& asset, NativeFileSystem& outMount,
                                   HeightfieldFactory& factory, ResourceManager*& outManager,
                                   foundation::content::ContentDatabase*& outDb)
    {
        Guid id;
        {
            foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
                outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
            auto* inst = db.RootGroup()->CreateInstance(u8"hf", HeightfieldSource::StaticType());
            id = inst->Id();

            HeightfieldAssetBuilder builder;
            NativeFileSystem srcMount(u8".", DefaultAllocator());
            pipeline::AssetBuildContext ctx{DefaultAllocator()};
            ctx.sources = &srcMount;
            ctx.output = inst;
            REQUIRE(builder.Build(asset, ctx).IsOk());
        }
        outDb = new foundation::content::ContentDatabase(
            foundation::core::DefaultAllocator(), outMount,
            foundation::core::BinarySerializerFactory(), u8".rasset");
        outManager = new ResourceManager(foundation::core::DefaultAllocator(), *outDb);
        outManager->AddFactory(&factory);
        return outManager->Bind<Heightfield>(id);
    }
}

TEST_CASE("heightfield.pipeline: resample helper - bilinear onto the grid")
{
    // A 2x2 source: (0, 20000) top row, (40000, 60000) bottom row.
    const Height src[4] = {0, 20000, 40000, 60000};
    RefPtr<Heightfield> hf =
        MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    ResampleHeightmapR16(src, 2, 2, *hf);

    // Corners map to the source corners exactly.
    CHECK(hf->GetSample(0, 0) == 0);
    CHECK(hf->GetSample(64, 0) == 20000);
    CHECK(hf->GetSample(0, 64) == 40000);
    CHECK(hf->GetSample(64, 64) == 60000);
    // Center is the average of the four corners.
    CHECK(hf->GetSample(32, 32) == doctest::Approx(30000).epsilon(0.01));
}

TEST_CASE("heightfield.pipeline: a blank asset cooks a flat grid")
{
    RegisterHeightfieldResourceTypes();
    RegisterHeightfieldAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_hfpipe_out_db", DefaultAllocator());

    HeightfieldAsset asset; // no fileName: blank
    asset.size = 65;
    asset.worldSize = Float2{64.0f, 64.0f};
    asset.minY = -2.0f;
    asset.maxY = 12.0f;

    HeightfieldFactory factory(DefaultAllocator());
    ResourceManager* manager = nullptr;
    foundation::content::ContentDatabase* db = nullptr;
    Proxy<Heightfield> hf = CookAndBind(asset, outMount, factory, manager, db);

    REQUIRE(hf);
    CHECK(hf->Size() == 65);
    CHECK(hf->WorldSize().x == doctest::Approx(64.0f));
    CHECK(hf->MinY() == doctest::Approx(-2.0f));
    CHECK(hf->MaxY() == doctest::Approx(12.0f));
    CHECK(hf->GetSample(10, 10) == 0); // blank
    CHECK_FALSE(hf->HasHoles());       // the hole stream is always cooked; a blank one is solid

    delete manager;
    delete db;
    RemoveTree();
}

TEST_CASE("heightfield.pipeline: an invalid size snaps to the next valid one")
{
    RegisterHeightfieldResourceTypes();
    RegisterHeightfieldAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_hfpipe_out_db", DefaultAllocator());

    HeightfieldAsset asset; // no fileName
    asset.size = 100; // not 64k+1: snaps to 129
    asset.worldSize = Float2{64.0f, 64.0f};

    HeightfieldFactory factory(DefaultAllocator());
    ResourceManager* manager = nullptr;
    foundation::content::ContentDatabase* db = nullptr;
    Proxy<Heightfield> hf = CookAndBind(asset, outMount, factory, manager, db);

    REQUIRE(hf);
    CHECK(hf->Size() == 129);

    delete manager;
    delete db;
    RemoveTree();
}

TEST_CASE("heightfield.pipeline: a heightmap image cooks into the grid (16-bit path)")
{
    RegisterHeightfieldResourceTypes();
    RegisterHeightfieldAsset();
    RemoveTree();

    // An 8-bit grayscale gradient rising along +X (stbi_load_16 promotes it to 16-bit).
    {
        Image src(8, 8, PixelFormat::R8);
        for (u32 y = 0; y < 8; ++y)
        {
            for (u32 x = 0; x < 8; ++x)
            {
                // R8 SetPixel stores the RGB luminance, so make the channels equal to get value x*36.
                const u8 v = static_cast<u8>(x * 36);
                src.SetPixel(x, y, Color32{v, v, v, 255});
            }
        }
        REQUIRE(foundation::image::io::SaveImage(src, u8"scratch_hfpipe_src.png",
                                               foundation::image::io::ImageFileFormat::PNG)
                    .IsOk());
    }

    NativeFileSystem outMount(u8"scratch_hfpipe_out_db", DefaultAllocator());
    HeightfieldAsset asset;
    asset.fileName = SourcePath(u8"scratch_hfpipe_src.png");
    asset.size = 65;
    asset.worldSize = Float2{64.0f, 64.0f};
    asset.minY = 0.0f;
    asset.maxY = 10.0f;

    HeightfieldFactory factory(DefaultAllocator());
    ResourceManager* manager = nullptr;
    foundation::content::ContentDatabase* db = nullptr;
    Proxy<Heightfield> hf = CookAndBind(asset, outMount, factory, manager, db);

    REQUIRE(hf);
    CHECK(hf->Size() == 65);
    // Gradient rises along +X: the low edge is near 0, the high edge much greater, monotonic.
    CHECK(hf->GetSample(0, 0) == 0);
    CHECK(hf->GetSample(64, 0) > hf->GetSample(0, 0));
    CHECK(hf->GetSample(64, 0) > 60000); // 252 * 257 promoted
    CHECK(hf->GetSample(32, 0) > hf->GetSample(0, 0));
    CHECK(hf->GetSample(32, 0) < hf->GetSample(64, 0));
    // Rows are identical (gradient is only along X).
    CHECK(hf->GetSample(20, 0) == hf->GetSample(20, 40));

    delete manager;
    delete db;
    RemoveTree();
}

TEST_CASE("heightfield cook: degenerate extents snap to legal values")
{
    // A hand-edited asset with a zero footprint or a closed Y range must still cook a grid
    // whose math is finite, as the size snap guards the grid size.
    RegisterHeightfieldResourceTypes();
    RegisterHeightfieldAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_hfpipe_out_db", DefaultAllocator());

    HeightfieldAsset asset;
    asset.size = 65;
    asset.worldSize = Float2{0.0f, 0.0f}; // degenerate footprint
    asset.minY = 5.0f;
    asset.maxY = 5.0f; // closed range

    HeightfieldFactory factory(DefaultAllocator());
    ResourceManager* manager = nullptr;
    foundation::content::ContentDatabase* db = nullptr;
    Proxy<Heightfield> hf = CookAndBind(asset, outMount, factory, manager, db);

    REQUIRE(hf);
    REQUIRE(!hf->IsEmpty());
    CHECK(hf->WorldSize().x > 0.0f);
    CHECK(hf->WorldSize().y > 0.0f);
    CHECK(hf->MaxY() > hf->MinY());
    const f32 h = hf->GetHeightAt(0.0f, 0.0f);
    CHECK(h == h); // finite, not NaN

    delete manager;
    delete db;
    RemoveTree();
}

// The editable-source convention: an embedded asset (fileName empty) cooks from the authored
// "heights" sidecar, which the sculpt save writes, and the builder declares the sidecar as a
// source stream so painting it re-cooks. A fileName-backed asset ignores the sidecar (the file
// is the truth; re-import resets).
TEST_CASE("heightfield.pipeline: an embedded asset cooks from the authored heights sidecar")
{
    RegisterHeightfieldResourceTypes();
    RegisterHeightfieldAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_hfpipe_out_db", DefaultAllocator());

    // The authored (sculpted) grid the sidecar carries.
    RefPtr<Heightfield> authored =
        MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    authored->SetSample(10, 12, 4321);
    authored->SetSample(33, 40, 60000);
    authored->SetHole(20, 21, true); // a cut the hole brush persisted

    HeightfieldAsset asset; // fileName empty, so embedded: the sidecar is the truth
    asset.size = 65;
    asset.worldSize = Float2{64.0f, 64.0f};
    asset.minY = 0.0f;
    asset.maxY = 10.0f;

    // The sidecar must be declared as a source stream (recipe-hash chaining) in embedded mode only.
    {
        HeightfieldAssetBuilder builder;
        pipeline::AssetBuildContext scanCtx{DefaultAllocator()};
        pipeline::AssetDependencies deps;
        builder.ScanDependencies(asset, scanCtx, deps);
        REQUIRE(deps.sourceStreams.Size() == 2);
        CHECK(deps.sourceStreams[0].AsView() == kHeightStream);
        CHECK(deps.sourceStreams[1].AsView() == kHoleStream); // a hole edit re-cooks too

        HeightfieldAsset imported; // Asset is non-copyable; only fileName matters here
        imported.fileName = foundation::vfs::SourcePath(u8"some.png");
        pipeline::AssetDependencies importedDeps;
        builder.ScanDependencies(imported, scanCtx, importedDeps);
        CHECK(importedDeps.sourceStreams.Size() == 0);
    }

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"hf", HeightfieldSource::StaticType());
        id = inst->Id();
        REQUIRE(inst->WriteData(kHeightStream, HeightfieldSource::HeightBlob(*authored)).IsOk());
        REQUIRE(inst->WriteData(kHoleStream, HeightfieldSource::HoleBlob(*authored)).IsOk());

        HeightfieldAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst; // the authored sidecar lives on the source instance
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                            u8".rasset");
    HeightfieldFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<Heightfield> hf = manager.Bind<Heightfield>(id);

    REQUIRE(hf);
    CHECK(hf->GetSample(10, 12) == 4321); // the sculpt survived the cook
    CHECK(hf->GetSample(33, 40) == 60000);
    CHECK(hf->GetSample(1, 1) == 0); // untouched samples stay flat
    CHECK(hf->HoleCount() == 1u);   // and the cut survived the cook
    CHECK(hf->IsHole(20, 21));

    RemoveTree();
}

TEST_CASE("heightfield.pipeline: builder ProductType is HeightfieldSource (the cook-stamp contract)")
{
    // The cook stamps the cooked instance with builder->ProductType(); returning the runtime
    // Heightfield (not HeightfieldSource) makes ReadObject build the wrong type and the factory's
    // Cast<HeightfieldSource> fail, and the resource never binds.
    CHECK(pipeline::HeightfieldAssetBuilder{}.ProductType() ==
          &foundation::heightfield::HeightfieldSource::StaticType());
}
