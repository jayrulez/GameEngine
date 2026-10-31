// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full terrain asset pipeline: author a TerrainAsset (referencing a cooked heightfield and layers),
// cook it with TerrainAssetBuilder into an output content DB, then load the Terrain through the
// manager and confirm it resolves the shared heightfield and carries the layers and flags (a
// reference pass-through).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.heightfield;
import foundation.heightfield.resource;
import foundation.terrain.resource;
import terrain.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::terrain;
namespace hf = foundation::heightfield;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_terrainpipe_db/hf.rasset");
        FileDelete(u8"scratch_terrainpipe_db/hf.heights.bin");
        FileDelete(u8"scratch_terrainpipe_db/terrain.rasset");
        FileDelete(u8"scratch_terrainpipe_db/terrain.palette.bin");
        FileDelete(u8"scratch_terrainpipe_db/terrain.palette.normal.bin");
        FileDelete(u8"scratch_terrainpipe_db/terrain.palette.orm.bin");
        FileDelete(u8"scratch_terrainpipe_db/terrain.palette.height.bin");
        FileDelete(u8"scratch_terrainpipe_db/terrain.palette.mask.bin");
        RemoveDirectory(u8"scratch_terrainpipe_db");
    }
}

TEST_CASE("terrain.pipeline: TerrainAsset cooks to a Terrain that resolves the shared heightfield")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainAsset();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_terrainpipe_db", DefaultAllocator());

    Guid heightfieldId;
    Guid terrainId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        // A cooked heightfield the terrain will reference.
        auto* hfInst = db.RootGroup()->CreateInstance(u8"hf", hf::HeightfieldSource::StaticType());
        heightfieldId = hfInst->Id();
        RefPtr<hf::Heightfield> grid =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        hf::HeightfieldSource hfSrc;
        hf::HeightfieldSource::FromHeightfield(*grid, hfSrc);
        REQUIRE(hfInst->WriteObject(hfSrc).IsOk());
        REQUIRE(
            hfInst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid)).IsOk());
        REQUIRE(hfInst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid)).IsOk()); // the cooked form's second stream

        // Cook the terrain asset through the builder.
        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();
        TerrainAsset asset;
        asset.heightfieldId = heightfieldId;
        asset.weightsId = Guid{123, 456};                // the top-K weights (id pass-through)
        asset.baseAlbedoId = Guid{111, 222};             // the base layer albedo
        asset.baseTileScale = 16.0f;
        asset.paletteAlbedoIds.PushBack(Guid{321, 654}); // non-nil so the albedo ref binds
        asset.paletteAlbedoIds.PushBack(Guid{});
        asset.paletteTileScales.PushBack(4.0f);
        asset.paletteTileScales.PushBack(8.0f);
        asset.castShadows = false;

        TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = tInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    hf::HeightfieldFactory heightfieldFactory(DefaultAllocator());
    TerrainFactory terrainFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&heightfieldFactory);
    manager.AddFactory(&terrainFactory);

    Proxy<TerrainResource> terrain = manager.Bind<TerrainResource>(terrainId);
    REQUIRE(terrain);
    CHECK(terrain->castShadows == false);
    CHECK(terrain->PaletteCount() == 2u);
    CHECK(terrain->base.tileScale == doctest::Approx(16.0f));
    CHECK(terrain->palette[0].tileScale == doctest::Approx(4.0f));
    CHECK(terrain->palette[1].tileScale == doctest::Approx(8.0f));
    REQUIRE(terrain->heightfield);
    CHECK(terrain->heightfield->Size() == 65);
    // Splat ids round-trip through the cook, the source and the factory to a bound ref (a non-nil id
    // binds a proxy even with no texture factory; GPU resolution and blending are tested elsewhere).
    CHECK(terrain->weights.IsBound());            // weightsId carried
    CHECK(terrain->base.albedo.IsBound());        // base albedo carried
    CHECK(terrain->palette[0].albedo.IsBound());  // non-nil palette albedo carried
    CHECK_FALSE(terrain->palette[1].albedo.IsBound()); // nil id stays unbound
    // The factory stamps each ref's source id (product guid equals source guid); the editor resolves
    // the heightfield asset from this ref chain (no reverse lookup).
    CHECK(terrain->heightfield.id == heightfieldId);
    CHECK(terrain->weights.id == Guid{123, 456});
    CHECK(terrain->base.albedo.id == Guid{111, 222});
    CHECK(terrain->palette[0].albedo.id == Guid{321, 654});

    RemoveTree();
}

TEST_CASE("terrain.pipeline: per-layer normal + ORM ids round-trip; arrays built on demand")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainAsset();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_terrainpipe_db", DefaultAllocator());

    Guid terrainId;
    u32 sliceSize = 0, mipCount = 0;
    Array<u8> cookedNormal; // the raw cooked normal stream (header + texels)
    bool ormStreamPresent = true;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* hfInst = db.RootGroup()->CreateInstance(u8"hf", hf::HeightfieldSource::StaticType());
        RefPtr<hf::Heightfield> grid =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        hf::HeightfieldSource hfSrc;
        hf::HeightfieldSource::FromHeightfield(*grid, hfSrc);
        REQUIRE(hfInst->WriteObject(hfSrc).IsOk());
        REQUIRE(
            hfInst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid)).IsOk());
        REQUIRE(hfInst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid)).IsOk()); // the cooked form's second stream

        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();
        TerrainAsset asset;
        asset.heightfieldId = hfInst->Id();
        asset.baseAlbedoId = Guid{111, 222};
        asset.baseNormalId = Guid{11, 22}; // base normal and ORM ids (unresolvable: flat/default)
        asset.baseOrmId = Guid{33, 44};
        asset.baseTileScale = 16.0f;
        asset.paletteTextureSize = 64; // keep the cook small/fast
        asset.paletteAlbedoIds.PushBack(Guid{321, 654});
        asset.paletteAlbedoIds.PushBack(Guid{});
        asset.paletteNormalIds.PushBack(Guid{55, 66}); // layer 0 has a normal: array built
        asset.paletteNormalIds.PushBack(Guid{});       // layer 1 nil: default flat slice
        // No ORM ids on any palette layer, so the ORM array must not be written.
        asset.paletteTileScales.PushBack(4.0f);
        asset.paletteTileScales.PushBack(8.0f);

        TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = tInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        // The normal stream exists (a layer used it); the ORM stream is absent.
        UniquePtr<IStream> nrm = tInst->ReadData(kPaletteNormalStream);
        REQUIRE(nrm);
        cookedNormal.Resize(static_cast<usize>(nrm->Size()));
        REQUIRE(nrm->Read(cookedNormal.Data(), static_cast<u64>(cookedNormal.Size())) ==
                cookedNormal.Size());
        ormStreamPresent = static_cast<bool>(tInst->ReadData(kPaletteOrmStream));
    }

    CHECK_FALSE(ormStreamPresent); // no ORM on any layer, so no array (built on demand)

    // The nil palette layer's normal slice defaulted to flat (128,128,255,255).
    REQUIRE(cookedNormal.Size() > sizeof(u32) * 3);
    MemCopy(&sliceSize, cookedNormal.Data(), sizeof(u32));
    MemCopy(&mipCount, cookedNormal.Data() + sizeof(u32), sizeof(u32));
    const usize sliceBytes = TerrainPaletteData::SliceBytes(sliceSize, mipCount);
    const usize slice1 = sizeof(u32) * 3 + sliceBytes * 1; // layer 1 (nil), mip 0, first texel
    CHECK(cookedNormal[slice1 + 0] == 128);
    CHECK(cookedNormal[slice1 + 1] == 128);
    CHECK(cookedNormal[slice1 + 2] == 255);
    CHECK(cookedNormal[slice1 + 3] == 255);

    // Load: refs bound and ids round-trip; paletteData has the normal array but not the ORM array.
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    hf::HeightfieldFactory heightfieldFactory(DefaultAllocator());
    TerrainFactory terrainFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&heightfieldFactory);
    manager.AddFactory(&terrainFactory);

    Proxy<TerrainResource> terrain = manager.Bind<TerrainResource>(terrainId);
    REQUIRE(terrain);
    CHECK(terrain->base.normal.IsBound());
    CHECK(terrain->base.normal.id == Guid{11, 22});
    CHECK(terrain->base.orm.IsBound());
    CHECK(terrain->base.orm.id == Guid{33, 44});
    CHECK(terrain->palette[0].normal.IsBound());
    CHECK(terrain->palette[0].normal.id == Guid{55, 66});
    CHECK_FALSE(terrain->palette[1].normal.IsBound()); // nil id stays unbound
    CHECK_FALSE(terrain->palette[0].orm.IsBound());    // no ORM ids authored
    REQUIRE(terrain->paletteData);
    CHECK(terrain->paletteData->HasNormal());   // normal array present (a layer used it)
    CHECK_FALSE(terrain->paletteData->HasOrm()); // ORM array absent

    RemoveTree();
}

TEST_CASE("terrain.pipeline: no normal/ORM maps -> no arrays (compat)")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainAsset();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_terrainpipe_db", DefaultAllocator());

    Guid terrainId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* hfInst = db.RootGroup()->CreateInstance(u8"hf", hf::HeightfieldSource::StaticType());
        RefPtr<hf::Heightfield> grid =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        hf::HeightfieldSource hfSrc;
        hf::HeightfieldSource::FromHeightfield(*grid, hfSrc);
        REQUIRE(hfInst->WriteObject(hfSrc).IsOk());
        REQUIRE(
            hfInst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid)).IsOk());
        REQUIRE(hfInst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid)).IsOk()); // the cooked form's second stream

        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();
        TerrainAsset asset;
        asset.heightfieldId = hfInst->Id();
        asset.baseAlbedoId = Guid{111, 222};
        asset.paletteTextureSize = 64;
        asset.paletteAlbedoIds.PushBack(Guid{321, 654}); // albedo only, no normal/ORM anywhere
        asset.paletteTileScales.PushBack(4.0f);

        TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = tInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteNormalStream)));
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteOrmStream)));
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    hf::HeightfieldFactory heightfieldFactory(DefaultAllocator());
    TerrainFactory terrainFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&heightfieldFactory);
    manager.AddFactory(&terrainFactory);

    Proxy<TerrainResource> terrain = manager.Bind<TerrainResource>(terrainId);
    REQUIRE(terrain);
    REQUIRE(terrain->paletteData);
    CHECK(terrain->paletteData->IsValid());     // albedo array present
    CHECK_FALSE(terrain->paletteData->HasNormal());
    CHECK_FALSE(terrain->paletteData->HasOrm());
    CHECK_FALSE(terrain->paletteData->HasHeight());
    CHECK_FALSE(terrain->paletteData->HasMask());

    RemoveTree();
}

TEST_CASE("terrain.pipeline: per-layer height ids + contrast round-trip; array built on demand")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainAsset();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_terrainpipe_db", DefaultAllocator());

    Guid terrainId;
    u32 sliceSize = 0, mipCount = 0;
    Array<u8> cookedHeight; // the raw cooked height stream (header + texels)
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* hfInst = db.RootGroup()->CreateInstance(u8"hf", hf::HeightfieldSource::StaticType());
        RefPtr<hf::Heightfield> grid =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        hf::HeightfieldSource hfSrc;
        hf::HeightfieldSource::FromHeightfield(*grid, hfSrc);
        REQUIRE(hfInst->WriteObject(hfSrc).IsOk());
        REQUIRE(
            hfInst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid)).IsOk());
        REQUIRE(hfInst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid)).IsOk()); // the cooked form's second stream

        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();
        TerrainAsset asset;
        asset.heightfieldId = hfInst->Id();
        asset.baseAlbedoId = Guid{111, 222};
        asset.baseHeightId = Guid{77, 88};       // base height id (unresolvable: dummy at bind)
        asset.heightBlendContrast = 0.4f;        // authored contrast must survive the cook
        asset.baseTileScale = 16.0f;
        asset.paletteTextureSize = 64;
        asset.paletteAlbedoIds.PushBack(Guid{321, 654});
        asset.paletteAlbedoIds.PushBack(Guid{});
        asset.paletteHeightIds.PushBack(Guid{55, 66}); // layer 0 has a height: array built
        asset.paletteHeightIds.PushBack(Guid{});       // layer 1 nil: default mid-height slice
        asset.paletteTileScales.PushBack(4.0f);
        asset.paletteTileScales.PushBack(8.0f);

        TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = tInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        UniquePtr<IStream> h = tInst->ReadData(kPaletteHeightStream);
        REQUIRE(h);
        cookedHeight.Resize(static_cast<usize>(h->Size()));
        REQUIRE(h->Read(cookedHeight.Data(), static_cast<u64>(cookedHeight.Size())) ==
                cookedHeight.Size());
        // No normal/ORM authored, so those arrays stay absent.
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteNormalStream)));
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteOrmStream)));
    }

    // The nil palette layer's height slice defaulted to mid-height (128,128,128,255).
    REQUIRE(cookedHeight.Size() > sizeof(u32) * 3);
    MemCopy(&sliceSize, cookedHeight.Data(), sizeof(u32));
    MemCopy(&mipCount, cookedHeight.Data() + sizeof(u32), sizeof(u32));
    const usize sliceBytes = TerrainPaletteData::SliceBytes(sliceSize, mipCount);
    const usize slice1 = sizeof(u32) * 3 + sliceBytes * 1; // layer 1 (nil), mip 0, first texel
    CHECK(cookedHeight[slice1 + 0] == 128);
    CHECK(cookedHeight[slice1 + 1] == 128);
    CHECK(cookedHeight[slice1 + 2] == 128);
    CHECK(cookedHeight[slice1 + 3] == 255);

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    hf::HeightfieldFactory heightfieldFactory(DefaultAllocator());
    TerrainFactory terrainFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&heightfieldFactory);
    manager.AddFactory(&terrainFactory);

    Proxy<TerrainResource> terrain = manager.Bind<TerrainResource>(terrainId);
    REQUIRE(terrain);
    CHECK(terrain->base.height.IsBound());
    CHECK(terrain->base.height.id == Guid{77, 88});
    CHECK(terrain->palette[0].height.IsBound());
    CHECK(terrain->palette[0].height.id == Guid{55, 66});
    CHECK_FALSE(terrain->palette[1].height.IsBound()); // nil id stays unbound
    CHECK(terrain->heightBlendContrast == doctest::Approx(0.4f)); // contrast survived the cook to the resource
    REQUIRE(terrain->paletteData);
    CHECK(terrain->paletteData->HasHeight());     // height array present (a layer used it)
    CHECK_FALSE(terrain->paletteData->HasNormal());
    CHECK_FALSE(terrain->paletteData->HasOrm());

    RemoveTree();
}

TEST_CASE("terrain.pipeline: per-layer mask ids round-trip; array built on demand, OPAQUE default")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainAsset();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_terrainpipe_db", DefaultAllocator());

    Guid terrainId;
    u32 sliceSize = 0, mipCount = 0;
    Array<u8> cookedMask; // the raw cooked mask stream (header + texels)
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* hfInst = db.RootGroup()->CreateInstance(u8"hf", hf::HeightfieldSource::StaticType());
        RefPtr<hf::Heightfield> grid =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        hf::HeightfieldSource hfSrc;
        hf::HeightfieldSource::FromHeightfield(*grid, hfSrc);
        REQUIRE(hfInst->WriteObject(hfSrc).IsOk());
        REQUIRE(
            hfInst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid)).IsOk());
        REQUIRE(hfInst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid)).IsOk()); // the cooked form's second stream

        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();
        TerrainAsset asset;
        asset.heightfieldId = hfInst->Id();
        asset.baseAlbedoId = Guid{111, 222};
        asset.paletteTextureSize = 64;
        asset.paletteAlbedoIds.PushBack(Guid{321, 654});
        asset.paletteAlbedoIds.PushBack(Guid{});
        asset.paletteMaskIds.PushBack(Guid{91, 92}); // layer 0 has a mask: array built
        asset.paletteMaskIds.PushBack(Guid{});       // layer 1 nil: default opaque slice
        asset.paletteTileScales.PushBack(4.0f);
        asset.paletteTileScales.PushBack(8.0f);

        TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = tInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        UniquePtr<IStream> m = tInst->ReadData(kPaletteMaskStream);
        REQUIRE(m);
        cookedMask.Resize(static_cast<usize>(m->Size()));
        REQUIRE(m->Read(cookedMask.Data(), static_cast<u64>(cookedMask.Size())) == cookedMask.Size());
        // No normal/ORM/height authored, so those arrays stay absent.
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteNormalStream)));
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteHeightStream)));
    }

    // The nil palette layer's mask slice defaulted to opaque (255,255,255,255), unlike height's
    // mid default: a maskless layer must contribute at full weight.
    REQUIRE(cookedMask.Size() > sizeof(u32) * 3);
    MemCopy(&sliceSize, cookedMask.Data(), sizeof(u32));
    MemCopy(&mipCount, cookedMask.Data() + sizeof(u32), sizeof(u32));
    const usize sliceBytes = TerrainPaletteData::SliceBytes(sliceSize, mipCount);
    const usize slice1 = sizeof(u32) * 3 + sliceBytes * 1; // layer 1 (nil), mip 0, first texel
    CHECK(cookedMask[slice1 + 0] == 255);
    CHECK(cookedMask[slice1 + 1] == 255);
    CHECK(cookedMask[slice1 + 2] == 255);
    CHECK(cookedMask[slice1 + 3] == 255);

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    hf::HeightfieldFactory heightfieldFactory(DefaultAllocator());
    TerrainFactory terrainFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&heightfieldFactory);
    manager.AddFactory(&terrainFactory);

    Proxy<TerrainResource> terrain = manager.Bind<TerrainResource>(terrainId);
    REQUIRE(terrain);
    CHECK(terrain->palette[0].mask.IsBound());
    CHECK(terrain->palette[0].mask.id == Guid{91, 92});
    CHECK_FALSE(terrain->palette[1].mask.IsBound()); // nil id stays unbound
    REQUIRE(terrain->paletteData);
    CHECK(terrain->paletteData->HasMask()); // mask array present (a layer used it)
    CHECK_FALSE(terrain->paletteData->HasHeight());

    RemoveTree();
}

TEST_CASE("terrain.pipeline: re-cooking WITHOUT a removed map DELETES its stale sidecar")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainAsset();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_terrainpipe_db", DefaultAllocator());

    Guid terrainId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* hfInst = db.RootGroup()->CreateInstance(u8"hf", hf::HeightfieldSource::StaticType());
        RefPtr<hf::Heightfield> grid =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        hf::HeightfieldSource hfSrc;
        hf::HeightfieldSource::FromHeightfield(*grid, hfSrc);
        REQUIRE(hfInst->WriteObject(hfSrc).IsOk());
        REQUIRE(
            hfInst->WriteData(hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid)).IsOk());
        REQUIRE(hfInst->WriteData(hf::kHoleStream, hf::HeightfieldSource::HoleBlob(*grid)).IsOk()); // the cooked form's second stream

        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();

        TerrainAsset withMask;
        withMask.heightfieldId = hfInst->Id();
        withMask.baseAlbedoId = Guid{111, 222};
        withMask.paletteTextureSize = 64;
        withMask.paletteAlbedoIds.PushBack(Guid{321, 654});
        withMask.paletteMaskIds.PushBack(Guid{91, 92}); // a mask: sidecar written
        withMask.paletteTileScales.PushBack(4.0f);

        TerrainAssetBuilder builder;
        NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = tInst;
        REQUIRE(builder.Build(withMask, ctx).IsOk());
        REQUIRE(static_cast<bool>(tInst->ReadData(kPaletteMaskStream))); // sidecar present

        // Re-cook the same instance with the mask removed: the stale sidecar must be deleted.
        TerrainAsset noMask;
        noMask.heightfieldId = hfInst->Id();
        noMask.baseAlbedoId = Guid{111, 222};
        noMask.paletteTextureSize = 64;
        noMask.paletteAlbedoIds.PushBack(Guid{321, 654});
        noMask.paletteTileScales.PushBack(4.0f);
        REQUIRE(builder.Build(noMask, ctx).IsOk());
        CHECK_FALSE(static_cast<bool>(tInst->ReadData(kPaletteMaskStream))); // sidecar gone
    }

    // The reloaded product must not report a mask (no stale array leaks into the runtime).
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    hf::HeightfieldFactory heightfieldFactory(DefaultAllocator());
    TerrainFactory terrainFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&heightfieldFactory);
    manager.AddFactory(&terrainFactory);

    Proxy<TerrainResource> terrain = manager.Bind<TerrainResource>(terrainId);
    REQUIRE(terrain);
    REQUIRE(terrain->paletteData);
    CHECK(terrain->paletteData->IsValid());       // albedo still there
    CHECK_FALSE(terrain->paletteData->HasMask()); // but the removed mask does not leak

    RemoveTree();
}
