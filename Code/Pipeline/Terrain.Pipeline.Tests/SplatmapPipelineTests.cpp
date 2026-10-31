// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SplatmapAsset cook (top-K model): the two-stream ("pixels" weights and "indices") round-trip
// through SplatmapAssetBuilder and SplatWeightsFactory with the product guid equal to the source
// guid (the ref-id parity the terrain resolution rests on), the refused weights-only source, the
// no-sidecar cook (all zero is pure base, no seeding), the PNG import (a fixed-layer raster
// converted to top-K), and the cook-stamp ProductType contract.
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
import foundation.terrain.resource;
import texture.pipeline;
import terrain.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::terrain;
namespace image = foundation::image;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_splatpipe_db/splat.rasset");
        FileDelete(u8"scratch_splatpipe_db/splat.pixels.bin");
        FileDelete(u8"scratch_splatpipe_db/splat.indices.bin");
        RemoveDirectory(u8"scratch_splatpipe_db");
    }
}

TEST_CASE("terrain.pipeline: SplatmapAsset cooks BOTH rasters and restores them identically")
{
    RegisterSplatmapAsset();
    RegisterSplatmapResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_splatpipe_db", DefaultAllocator());

    // Author a painted top-K raster the cook will carry.
    RefPtr<SplatWeights> authored = MakeRef<SplatWeights>(DefaultAllocator(), 16, 16);
    (void)PaintTopK(*authored, 0.5f, 0.5f, 0.3f, 0.3f, /*palette*/ 5u, 1.0f);
    REQUIRE(authored->WeightOfLayer(8, 8, 5) > 0);

    Guid splatId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                                foundation::core::BinarySerializerFactory(),
                                                u8".rasset");
        // Cook in place into one instance (product guid equals source guid): the source sidecars
        // feed the builder, which writes the cooked SplatWeightsSource and both streams back.
        auto* inst = db.RootGroup()->CreateInstance(u8"splat", SplatWeightsSource::StaticType());
        splatId = inst->Id();
        REQUIRE(inst->WriteData(kSplatStream, SplatWeightsSource::WeightBlob(*authored)).IsOk());
        REQUIRE(
            inst->WriteData(kSplatIndexStream, SplatWeightsSource::IndexBlob(*authored)).IsOk());

        SplatmapAsset sa;
        sa.width = 16;
        sa.height = 16;
        SplatmapAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = inst;
        REQUIRE(builder.Build(sa, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                            foundation::core::BinarySerializerFactory(),
                                            u8".rasset");
    SplatWeightsFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<SplatWeights> loaded = manager.Bind<SplatWeights>(splatId); // bind by the source guid
    REQUIRE(loaded);
    CHECK(loaded->Width() == 16);
    CHECK(loaded->Height() == 16);
    CHECK(loaded->WeightOfLayer(8, 8, 5) == authored->WeightOfLayer(8, 8, 5));
    CHECK(loaded->SlotIndex(8, 8, 0) == authored->SlotIndex(8, 8, 0));
    CHECK(loaded->BaseWeight(0, 0) == 255); // untouched corner: pure base

    RemoveTree();
}

TEST_CASE("terrain.pipeline: no sidecars cook an all-zero raster (pure base - no seeding)")
{
    RegisterSplatmapAsset();
    RegisterSplatmapResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_splatpipe_db", DefaultAllocator());

    Guid splatId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                                foundation::core::BinarySerializerFactory(),
                                                u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"splat", SplatWeightsSource::StaticType());
        splatId = inst->Id(); // no sidecars written
        SplatmapAsset sa;
        sa.width = 8;
        sa.height = 8;

        SplatmapAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = inst;
        REQUIRE(builder.Build(sa, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                            foundation::core::BinarySerializerFactory(),
                                            u8".rasset");
    SplatWeightsFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<SplatWeights> loaded = manager.Bind<SplatWeights>(splatId);
    REQUIRE(loaded);
    CHECK(loaded->Width() == 8);
    CHECK(loaded->BaseWeight(4, 4) == 255); // pure base everywhere, no layer-0 seeding
    CHECK(loaded->SlotWeight(4, 4, 0) == 0);

    RemoveTree();
}

TEST_CASE("terrain.pipeline: a pixels-only sidecar (no indices) FAILS the cook - no migration")
{
    RegisterSplatmapAsset();
    RegisterSplatmapResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_splatpipe_db", DefaultAllocator());

    // A source carrying only the weight raster is a broken (or pre-top-K) source: the cook
    // refuses it instead of guessing a layout. Re-seed / re-paint the splatmap.
    Array<u8> weights;
    weights.Resize(8 * 8 * 4, u8{100});
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                                foundation::core::BinarySerializerFactory(),
                                                u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"splat", SplatWeightsSource::StaticType());
        REQUIRE(inst->WriteData(kSplatStream,
                                Span<const byte>{reinterpret_cast<const byte*>(weights.Data()),
                                                 weights.Size()})
                    .IsOk());

        SplatmapAsset sa;
        sa.width = 8;
        sa.height = 8;
        SplatmapAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.source = inst;
        ctx.output = inst;
        CHECK_FALSE(builder.Build(sa, ctx).IsOk());
    }

    RemoveTree();
}

TEST_CASE("terrain.pipeline: PNG import decodes as a fixed-layer raster (R = base, G/B/A = palette)")
{
    RegisterSplatmapAsset();
    RegisterSplatmapResourceTypes();
    FileDelete(u8"scratch_splatimg/splat.png");
    RemoveDirectory(u8"scratch_splatimg");
    FileDelete(u8"scratch_splatimg_db/s.rasset");
    FileDelete(u8"scratch_splatimg_db/s.pixels.bin");
    FileDelete(u8"scratch_splatimg_db/s.indices.bin");
    RemoveDirectory(u8"scratch_splatimg_db");
    REQUIRE(CreateDirectory(u8"scratch_splatimg"));

    // Imported PNGs are fixed-layer rasters (R = the base share, G/B/A = palette 0..2): a
    // solid (128, 127, 0, 0) image is half base and half palette-0 after conversion.
    image::Image authored(4, 2, image::PixelFormat::RGBA8);
    Span<u8> ap = authored.PixelDataMut();
    for (usize t = 0; t < 8; ++t)
    {
        ap[t * 4 + 0] = 128;
        ap[t * 4 + 1] = 127;
        ap[t * 4 + 2] = 0;
        ap[t * 4 + 3] = 0;
    }
    REQUIRE(image::io::SaveImage(authored, u8"scratch_splatimg/splat.png",
                                 image::io::ImageFileFormat::PNG)
                .IsOk());

    NativeFileSystem outMount(u8"scratch_splatimg_db", DefaultAllocator());
    Guid splatId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                                foundation::core::BinarySerializerFactory(),
                                                u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"s", SplatWeightsSource::StaticType());
        splatId = inst->Id();
        SplatmapAsset sa;
        sa.fileName = foundation::vfs::SourcePath(u8"splat.png");
        SplatmapAssetBuilder builder;
        NativeFileSystem srcMount(u8"scratch_splatimg", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(sa, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount,
                                            foundation::core::BinarySerializerFactory(),
                                            u8".rasset");
    SplatWeightsFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<SplatWeights> loaded = manager.Bind<SplatWeights>(splatId);
    REQUIRE(loaded);
    CHECK(loaded->Width() == 4); // the PNG's native size (no resampling)
    CHECK(loaded->Height() == 2);
    // 128/255 base and 127/255 palette-0, within quantum.
    CHECK(loaded->WeightOfLayer(1, 1, 0) >= 126);
    CHECK(loaded->WeightOfLayer(1, 1, 0) <= 128);
    CHECK(loaded->BaseWeight(1, 1) >= 126);
    CHECK(loaded->BaseWeight(1, 1) <= 129);

    FileDelete(u8"scratch_splatimg/splat.png");
    RemoveDirectory(u8"scratch_splatimg");
    FileDelete(u8"scratch_splatimg_db/s.rasset");
    FileDelete(u8"scratch_splatimg_db/s.pixels.bin");
    FileDelete(u8"scratch_splatimg_db/s.indices.bin");
    RemoveDirectory(u8"scratch_splatimg_db");
}

TEST_CASE("terrain.pipeline: builder ProductType is the SERIALIZED cooked form (the cook-stamp contract)")
{
    // The cook driver stamps the cooked instance with builder->ProductType() and the runtime
    // ReadObject reconstructs it, so ProductType must be the serialized form (...Source), not the
    // runtime product. Returning the product makes ReadObject build the wrong type, the factory's
    // Cast<...Source> fails, and the resource never binds. Pins the contract the round-trips above
    // rely on.
    CHECK(TerrainAssetBuilder{}.ProductType() == &TerrainSource::StaticType());
    CHECK(SplatmapAssetBuilder{}.ProductType() == &SplatWeightsSource::StaticType());
}

TEST_CASE("terrain.pipeline: the palette cook decodes albedos through the SOURCE db (two-DB shape)")
{
    // The production cook shape: ctx.db is the cooked db (whose texture instances hold cooked
    // products, not TextureAsset envelopes) and ctx.sourceDb is the source db. The palette cook
    // must decode through the source db: resolving the albedo against the cooked db casts to
    // the wrong type and silently whites out every slice.
    pipeline::RegisterTerrainAsset();
    pipeline::RegisterTextureAsset();
    RegisterTerrainResourceTypes();
    FileDelete(u8"scratch_palette_src/albedo.png");
    RemoveDirectory(u8"scratch_palette_src");
    FileDelete(u8"scratch_palette_srcdb/albedo.rasset");
    FileDelete(u8"scratch_palette_srcdb/terrain.rasset");
    RemoveDirectory(u8"scratch_palette_srcdb");
    FileDelete(u8"scratch_palette_cookdb/terrain.rasset");
    FileDelete(u8"scratch_palette_cookdb/terrain.palette.bin");
    RemoveDirectory(u8"scratch_palette_cookdb");
    REQUIRE(CreateDirectory(u8"scratch_palette_src"));

    // A solid orange source image the slice must carry (white is the failure fallback).
    image::Image authored(8, 8, image::PixelFormat::RGBA8);
    Span<u8> ap = authored.PixelDataMut();
    for (usize t = 0; t < 64; ++t)
    {
        ap[t * 4 + 0] = 200;
        ap[t * 4 + 1] = 120;
        ap[t * 4 + 2] = 30;
        ap[t * 4 + 3] = 255;
    }
    REQUIRE(image::io::SaveImage(authored, u8"scratch_palette_src/albedo.png",
                                 image::io::ImageFileFormat::PNG)
                .IsOk());

    NativeFileSystem srcDbMount(u8"scratch_palette_srcdb", DefaultAllocator());
    NativeFileSystem cookDbMount(u8"scratch_palette_cookdb", DefaultAllocator());
    Guid terrainId;
    {
        foundation::content::ContentDatabase sourceDb(foundation::core::DefaultAllocator(), 
            srcDbMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        // The albedo source envelope (its fileName names the png above).
        auto* albedoInst =
            sourceDb.RootGroup()->CreateInstance(u8"albedo", pipeline::TextureAsset::StaticType());
        pipeline::TextureAsset texSrc;
        texSrc.fileName = foundation::vfs::SourcePath(u8"albedo.png");
        REQUIRE(albedoInst->WriteObject(texSrc).IsOk());

        // The cooked db carries no TextureAsset for that guid, as in production.
        foundation::content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), 
            cookDbMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* product =
            cookedDb.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = product->Id();

        pipeline::TerrainAsset ta;
        ta.paletteAlbedoIds.PushBack(albedoInst->Id());
        ta.paletteTileScales.PushBack(4.0f);
        ta.paletteTextureSize = 64;

        pipeline::TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8"scratch_palette_src", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = product;
        ctx.db = &cookedDb;      // cooked-products view: has no TextureAsset
        ctx.sourceDb = &sourceDb; // where the albedo envelope and png actually live
        REQUIRE(builder.Build(ta, ctx).IsOk());
    }

    // The cooked palette sidecar's slice must carry the orange, at every sampled mip.
    {
        foundation::content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), 
            cookDbMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        foundation::content::Instance* inst = cookedDb.GetInstance(terrainId);
        REQUIRE(inst != nullptr);
        UniquePtr<IStream> stream = inst->ReadData(kPaletteStream);
        REQUIRE(static_cast<bool>(stream));
        u32 header[3] = {};
        REQUIRE(stream->Read(header, sizeof(header)) == sizeof(header));
        CHECK(header[2] == 1u); // one slice
        Array<u8> texels;
        const usize bytes = TerrainPaletteData::SliceBytes(header[0], header[1]);
        texels.Resize(bytes);
        REQUIRE(stream->Read(texels.Data(), bytes) == bytes);
        CHECK(texels[0] == 200); // mip 0 texel 0 is the authored orange, not the white fallback
        CHECK(texels[1] == 120);
        CHECK(texels[2] == 30);
        const usize lastTexel = bytes - 4; // the 1x1 tail mip
        CHECK(texels[lastTexel + 0] == 200);
        CHECK(texels[lastTexel + 1] == 120);
    }

    FileDelete(u8"scratch_palette_src/albedo.png");
    RemoveDirectory(u8"scratch_palette_src");
    FileDelete(u8"scratch_palette_srcdb/albedo.rasset");
    FileDelete(u8"scratch_palette_srcdb/terrain.rasset");
    RemoveDirectory(u8"scratch_palette_srcdb");
    FileDelete(u8"scratch_palette_cookdb/terrain.rasset");
    FileDelete(u8"scratch_palette_cookdb/terrain.palette.bin");
    RemoveDirectory(u8"scratch_palette_cookdb");
}

TEST_CASE("terrain.pipeline: the palette-array cook helpers resize and mip a slice")
{
    // The palette cook wraps these per slice: an 8x8 solid resizes to 4x4 and box-halves to 2x2
    // preserving the color, and SliceBytes covers the whole mip chain.
    Array<u8> red;
    red.Resize(8 * 8 * 4);
    for (usize i = 0; i < 64; ++i)
    {
        red[i * 4 + 0] = 200;
        red[i * 4 + 1] = 10;
        red[i * 4 + 2] = 10;
        red[i * 4 + 3] = 255;
    }
    Array<u8> resized;
    resized.Resize(4 * 4 * 4);
    ResizeRgba8Bilinear(Span<const u8>{red.Data(), red.Size()}, 8, 8,
                        Span<u8>{resized.Data(), resized.Size()}, 4, 4);
    CHECK(resized[0] == 200); // a solid color survives resizing exactly
    CHECK(resized[1] == 10);
    CHECK(resized[63] == 255);

    Array<u8> mip;
    mip.Resize(2 * 2 * 4);
    const u32 halfDim = BoxHalveRgba8(Span<const u8>{resized.Data(), resized.Size()}, 4,
                                      Span<u8>{mip.Data(), mip.Size()});
    CHECK(halfDim == 2u);
    CHECK(mip[0] == 200); // the box filter of a solid is the solid
    CHECK(mip[3] == 255);

    // The chain 4x4, 2x2 and 1x1 at 4 bytes per texel.
    CHECK(TerrainPaletteData::SliceBytes(4, 3) == (16u + 4u + 1u) * 4u);
}

TEST_CASE("terrain.pipeline: the sRGB-aware halve averages in LINEAR space (albedo mips, R3)")
{
    // A 2x2 black/white checker: averaging the sRGB bytes gives 128 (too dark); averaging in
    // linear then re-encoding gives sRGB ~188 (as the texture cook does). The albedo palette
    // array uploads as RGBA8UnormSrgb, so its mips must use the sRGB-aware halve.
    u8 checker[2 * 2 * 4] = {};
    const u32 whiteTexels[2] = {0u, 3u}; // (0,0) and (1,1) white, the others black
    for (u32 t : whiteTexels)
    {
        checker[t * 4 + 0] = 255;
        checker[t * 4 + 1] = 255;
        checker[t * 4 + 2] = 255;
    }
    for (u32 t = 0; t < 4; ++t)
    {
        checker[t * 4 + 3] = 255; // alpha 255 everywhere
    }

    u8 srgbMip[4] = {};
    CHECK(BoxHalveRgba8SrgbAware(Span<const u8>{checker, sizeof(checker)}, 2,
                                 Span<u8>{srgbMip, sizeof(srgbMip)}) == 1u);
    CHECK(srgbMip[0] >= 186); // linear 0.5 encodes to sRGB ~188
    CHECK(srgbMip[0] <= 190);
    CHECK(srgbMip[3] == 255); // alpha is linear: plain average

    u8 plainMip[4] = {};
    CHECK(BoxHalveRgba8(Span<const u8>{checker, sizeof(checker)}, 2,
                        Span<u8>{plainMip, sizeof(plainMip)}) == 1u);
    CHECK(plainMip[0] == 128); // the plain box filter (correct for linear normal/ORM data)
}
