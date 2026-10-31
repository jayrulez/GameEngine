// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VegetationMaskAsset cook: the "densities" stream round-trips through VegetationMaskAssetBuilder
// and VegetationMaskFactory with the product guid equal to the source guid, the no-sidecar cook
// (all zero: nothing grows, no seeding), a size mismatch fails the cook, the PNG import (one plane
// per channel), and
// the cook-stamp ProductType contract.
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
import foundation.vegetation.resource;
import vegetation.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::vegetation;
namespace image = foundation::image;

namespace
{
    void RemoveDb(StringView dir, StringView stem)
    {
        FileDelete(Format(u8"{}/{}.rasset", dir, stem).AsView());
        FileDelete(Format(u8"{}/{}.densities.bin", dir, stem).AsView());
        RemoveDirectory(dir);
    }
}

TEST_CASE("vegetation.pipeline: an embedded mask cooks its planes and restores them identically")
{
    RegisterVegetationMaskAsset();
    RegisterVegetationMaskResourceTypes();
    RemoveDb(u8"scratch_vegmask_db", u8"mask");
    NativeFileSystem outMount(u8"scratch_vegmask_db", DefaultAllocator());
    // Two painted planes the cook will carry.
    RefPtr<VegetationMask> authored = MakeRef<VegetationMask>(DefaultAllocator(), 16, 16, 2);
    (void)PaintMask(*authored, 0, 0.5f, 0.5f, 0.3f, 0.3f, 1.0f);
    (void)PaintMask(*authored, 1, 0.2f, 0.2f, 0.2f, 0.2f, 0.5f);
    REQUIRE(authored->DensityAt(0, 8, 8) == 255);
    REQUIRE(authored->DensityAt(1, 3, 3) > 0);
    Guid maskId;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), outMount,
                                                BinarySerializerFactory(), u8".rasset");
        // Cook in place into one instance (product guid equals source guid): the source sidecar
        // feeds the builder, which writes the cooked VegetationMaskSource and the stream back.
        auto* inst = db.RootGroup()->CreateInstance(u8"mask", VegetationMaskSource::StaticType());
        maskId = inst->Id();
        REQUIRE(inst->WriteData(kVegetationMaskStream, VegetationMaskSource::DensityBlob(*authored))
                    .IsOk());
        VegetationMaskAsset ma;
        ma.width = 16;
        ma.height = 16;
        ma.planeCount = 2;
        VegetationMaskAssetBuilder builder;
        CHECK(builder.AssetType() == &VegetationMaskAsset::StaticType());
        CHECK(builder.ProductType() == &VegetationMaskSource::StaticType()); // the cook stamp
        pipeline::AssetDependencies deps;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = inst;
        builder.ScanDependencies(ma, ctx, deps);
        REQUIRE(deps.sourceStreams.Size() == 1u); // the recipe hash chains the stream
        CHECK(deps.sourceStreams[0] == String(kVegetationMaskStream));
        REQUIRE(builder.Build(ma, ctx).IsOk());
    }
    foundation::content::ContentDatabase db(DefaultAllocator(), outMount, BinarySerializerFactory(),
                                            u8".rasset");
    VegetationMaskFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<VegetationMask> loaded = manager.Bind<VegetationMask>(maskId); // bind by the source guid
    REQUIRE(loaded);
    CHECK(loaded->Width() == 16);
    CHECK(loaded->Height() == 16);
    CHECK(loaded->PlaneCount() == 2u);
    for (u32 p = 0; p < 2; ++p)
    {
        for (i32 y = 0; y < 16; ++y)
        {
            for (i32 x = 0; x < 16; ++x)
            {
                CHECK(loaded->DensityAt(p, x, y) == authored->DensityAt(p, x, y));
            }
        }
    }
    RemoveDb(u8"scratch_vegmask_db", u8"mask");
}

TEST_CASE("vegetation.pipeline: a plane-count change on a painted mask keeps the planes that still exist")
{
    RegisterVegetationMaskAsset();
    RegisterVegetationMaskResourceTypes();
    RemoveDb(u8"scratch_vegmask_planes", u8"mask");
    NativeFileSystem outMount(u8"scratch_vegmask_planes", DefaultAllocator());
    RefPtr<VegetationMask> painted = MakeRef<VegetationMask>(DefaultAllocator(), 8, 8, 2);
    (void)PaintMask(*painted, 0, 0.5f, 0.5f, 0.4f, 0.4f, 1.0f);
    (void)PaintMask(*painted, 1, 0.5f, 0.5f, 0.2f, 0.2f, 1.0f);
    Guid maskId;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), outMount,
                                                BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"mask", VegetationMaskSource::StaticType());
        maskId = inst->Id();
        REQUIRE(inst->WriteData(kVegetationMaskStream, VegetationMaskSource::DensityBlob(*painted))
                    .IsOk());
        VegetationMaskAsset ma; // the author raised the plane count to 3 after painting 2
        ma.width = 8;
        ma.height = 8;
        ma.planeCount = 3;
        VegetationMaskAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = inst;
        REQUIRE(builder.Build(ma, ctx).IsOk());
    }
    foundation::content::ContentDatabase db(DefaultAllocator(), outMount, BinarySerializerFactory(),
                                            u8".rasset");
    VegetationMaskFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<VegetationMask> loaded = manager.Bind<VegetationMask>(maskId);
    REQUIRE(loaded);
    CHECK(loaded->PlaneCount() == 3u);
    CHECK(loaded->DensityAt(0, 4, 4) == painted->DensityAt(0, 4, 4)); // kept
    CHECK(loaded->DensityAt(1, 4, 4) == painted->DensityAt(1, 4, 4)); // kept
    CHECK(loaded->DensityAt(2, 4, 4) == 0);                            // fresh
    RemoveDb(u8"scratch_vegmask_planes", u8"mask");
}

TEST_CASE("vegetation.pipeline: no sidecar cooks all-zero planes; a size mismatch fails the cook")
{
    RegisterVegetationMaskAsset();
    RegisterVegetationMaskResourceTypes();
    RemoveDb(u8"scratch_vegmask_empty", u8"mask");
    NativeFileSystem outMount(u8"scratch_vegmask_empty", DefaultAllocator());
    Guid maskId;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), outMount,
                                                BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"mask", VegetationMaskSource::StaticType());
        maskId = inst->Id();
        VegetationMaskAsset ma;
        ma.width = 8;
        ma.height = 4;
        ma.planeCount = 3;
        VegetationMaskAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = inst;
        REQUIRE(builder.Build(ma, ctx).IsOk()); // never painted: all zero, no seeding

        // A stream of the wrong size is a broken source: the cook refuses it.
        const u8 junk[5] = {1, 2, 3, 4, 5};
        REQUIRE(inst->WriteData(kVegetationMaskStream,
                                Span<const byte>{reinterpret_cast<const byte*>(junk), 5})
                    .IsOk());
        CHECK(!builder.Build(ma, ctx).IsOk());
        // Restore the all-zero cook for the load below.
        Array<u8> zero;
        zero.Resize(8u * 4u * 3u, u8{0});
        REQUIRE(inst->WriteData(kVegetationMaskStream,
                                Span<const byte>{reinterpret_cast<const byte*>(zero.Data()),
                                                 zero.Size()})
                    .IsOk());
        REQUIRE(builder.Build(ma, ctx).IsOk());
    }
    foundation::content::ContentDatabase db(DefaultAllocator(), outMount, BinarySerializerFactory(),
                                            u8".rasset");
    VegetationMaskFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<VegetationMask> loaded = manager.Bind<VegetationMask>(maskId);
    REQUIRE(loaded);
    CHECK(loaded->Width() == 8);
    CHECK(loaded->Height() == 4);
    CHECK(loaded->PlaneCount() == 3u);
    for (u32 p = 0; p < 3; ++p)
    {
        CHECK(loaded->DensityAt(p, 3, 2) == 0);
    }
    RemoveDb(u8"scratch_vegmask_empty", u8"mask");
}

TEST_CASE("vegetation.pipeline: PNG import decodes one density plane per channel at native size")
{
    RegisterVegetationMaskAsset();
    RegisterVegetationMaskResourceTypes();
    FileDelete(u8"scratch_vegmask_img/mask.png");
    RemoveDirectory(u8"scratch_vegmask_img");
    RemoveDb(u8"scratch_vegmask_img_db", u8"m");
    REQUIRE(CreateDirectory(u8"scratch_vegmask_img"));
    image::Image authored(4, 2, image::PixelFormat::RGBA8);
    Span<u8> ap = authored.PixelDataMut();
    for (usize t = 0; t < 8; ++t)
    {
        ap[t * 4 + 0] = 200;
        ap[t * 4 + 1] = 100;
        ap[t * 4 + 2] = static_cast<u8>(t * 10);
        ap[t * 4 + 3] = 255;
    }
    REQUIRE(image::io::SaveImage(authored, u8"scratch_vegmask_img/mask.png",
                                 image::io::ImageFileFormat::PNG)
                .IsOk());
    NativeFileSystem outMount(u8"scratch_vegmask_img_db", DefaultAllocator());
    Guid maskId;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), outMount,
                                                BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"m", VegetationMaskSource::StaticType());
        maskId = inst->Id();
        VegetationMaskAsset ma;
        ma.fileName = foundation::vfs::SourcePath(u8"mask.png");
        VegetationMaskAssetBuilder builder;
        NativeFileSystem srcMount(u8"scratch_vegmask_img", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        pipeline::AssetDependencies deps;
        builder.ScanDependencies(ma, ctx, deps);
        CHECK(deps.sourceStreams.IsEmpty()); // an import chains the file, not a stream
        REQUIRE(builder.Build(ma, ctx).IsOk());
    }
    foundation::content::ContentDatabase db(DefaultAllocator(), outMount, BinarySerializerFactory(),
                                            u8".rasset");
    VegetationMaskFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<VegetationMask> loaded = manager.Bind<VegetationMask>(maskId);
    REQUIRE(loaded);
    CHECK(loaded->Width() == 4); // the PNG's native size
    CHECK(loaded->Height() == 2);
    CHECK(loaded->PlaneCount() == 4u);
    CHECK(loaded->DensityAt(0, 1, 1) == 200); // R
    CHECK(loaded->DensityAt(1, 1, 1) == 100); // G
    CHECK(loaded->DensityAt(2, 3, 1) == 70);  // B = texel index x 10
    CHECK(loaded->DensityAt(3, 0, 0) == 255); // A

    // The importer's plan names the asset type the stored selection resolves.
    VegetationMaskFileImporter importer;
    CHECK(importer.Accepts(u8"png"));
    CHECK(!importer.Accepts(u8"exr"));
    CHECK(importer.Label() == u8"Vegetation Mask");

    FileDelete(u8"scratch_vegmask_img/mask.png");
    RemoveDirectory(u8"scratch_vegmask_img");
    RemoveDb(u8"scratch_vegmask_img_db", u8"m");
}

TEST_CASE("vegetation.pipeline: the RGBA -> planes helper bounds-checks its input")
{
    const u8 rgba[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    RefPtr<VegetationMask> two = VegetationMaskAssetBuilder::MaskFromRgba(
        Span<const u8>{rgba, 8}, 2, 1, DefaultAllocator());
    REQUIRE(two.Get() != nullptr);
    CHECK(two->PlaneCount() == 4u);
    CHECK(two->DensityAt(0, 1, 0) == 5);
    CHECK(two->DensityAt(3, 0, 0) == 4);
    CHECK(VegetationMaskAssetBuilder::MaskFromRgba(Span<const u8>{rgba, 8}, 4, 1, DefaultAllocator())
              ->IsEmpty()); // short buffer
}
