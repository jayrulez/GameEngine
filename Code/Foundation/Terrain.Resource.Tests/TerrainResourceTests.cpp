// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The cooked terrain resource: a TerrainSource (top-K: base, palette and weightsId) serialize
// round-trip, and a full content-DB build through the manager that resolves the referenced
// heightfield (proving the shared Ref) while the texture refs stay unbound (no GPU factory
// headless).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.heightfield;
import foundation.heightfield.resource;
import foundation.terrain.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::terrain;
namespace hf = foundation::heightfield;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_terrain_res_db/hf.rasset");
        FileDelete(u8"scratch_terrain_res_db/hf.heights.bin");
        FileDelete(u8"scratch_terrain_res_db/terrain.rasset");
        RemoveDirectory(u8"scratch_terrain_res_db");
    }
}

TEST_CASE("terrain resource: TerrainSource v2 round-trips base + palette + weights")
{
    TerrainSource a;
    a.heightfieldId = Guid(1, 2);
    a.weightsId = Guid(3, 4);
    a.baseAlbedoId = Guid(5, 6);
    a.baseTileScale = 16.0f;
    a.paletteAlbedoIds.PushBack(Guid(7, 8));
    a.paletteAlbedoIds.PushBack(Guid(9, 10));
    a.paletteTileScales.PushBack(4.0f);
    a.paletteTileScales.PushBack(8.0f);
    a.castShadows = false;

    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, TerrainSource::StaticType()); // write at the current version
        a.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    TerrainSource b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, TerrainSource::StaticType());
        b.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(b.heightfieldId == Guid(1, 2));
    CHECK(b.weightsId == Guid(3, 4));
    CHECK(b.baseAlbedoId == Guid(5, 6));
    CHECK(b.baseTileScale == doctest::Approx(16.0f));
    CHECK(b.paletteAlbedoIds.Size() == 2u);
    CHECK(b.paletteAlbedoIds[1] == Guid(9, 10));
    CHECK(b.paletteTileScales[0] == doctest::Approx(4.0f));
    CHECK(b.castShadows == false);
}

TEST_CASE("terrain resource: builds through the manager and resolves the shared heightfield")
{
    hf::RegisterHeightfieldResourceTypes();
    RegisterTerrainResourceTypes();
    RemoveTree();
    NativeFileSystem mount(u8"scratch_terrain_res_db", DefaultAllocator());

    Guid heightfieldId;
    Guid terrainId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        // A cooked heightfield.
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

        // A terrain referencing that heightfield, a base and two (texture-less) palette layers.
        auto* tInst = db.RootGroup()->CreateInstance(u8"terrain", TerrainSource::StaticType());
        terrainId = tInst->Id();
        TerrainSource tSrc;
        tSrc.heightfieldId = heightfieldId;
        tSrc.baseTileScale = 16.0f;
        tSrc.paletteAlbedoIds.PushBack(Guid{}); // nil albedo (no GPU factory here)
        tSrc.paletteAlbedoIds.PushBack(Guid{});
        tSrc.paletteTileScales.PushBack(4.0f);
        tSrc.paletteTileScales.PushBack(8.0f);
        tSrc.castShadows = false;
        REQUIRE(tInst->WriteObject(tSrc).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
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
    // The shared heightfield resolved through the same manager.
    REQUIRE(terrain->heightfield);
    CHECK(terrain->heightfield->Size() == 65);
    CHECK(terrain->heightfield->GetHeightAt(0.0f, 0.0f) == doctest::Approx(0.0f));

    RemoveTree();
}
