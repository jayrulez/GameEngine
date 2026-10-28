// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TerrainEditorPage tests: the cooked terrain's facts (pure), the TerrainAsset blob round-trips a
// snapshot rides (versioned payload, so the top-K fields survive), and the page over its markup
// (Data/Editor/PreviewPage.xml) and a scratch project without a host (no preview): its rows from
// the asset, each edit one undo step, layers added and removed, new splat weights made.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.heightfield;
import foundation.terrain.resource;
import pipeline.core;
import terrain.pipeline;
import editor.terrain;

using namespace foundation::core;
namespace hf = foundation::heightfield;
namespace terrain = foundation::terrain;

TEST_CASE("TerrainFacts says the grid, world size, chunks, layers and shadows of a cooked terrain")
{
    RefPtr<hf::Heightfield> grid = MakeRef<hf::Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 20.0f);
    RefPtr<terrain::TerrainResource> res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get(); // direct in-memory product
    res->castShadows = true;
    res->palette.PushBack(terrain::TerrainResource::Layer{});
    res->palette.PushBack(terrain::TerrainResource::Layer{});

    const Array<editor::TerrainFact> facts = editor::TerrainFacts(*res);
    const auto value = [&](StringView name)
    {
        for (const editor::TerrainFact& fact : facts)
            if (fact.name.AsView() == name)
                return fact.value;
        return String(u8"?");
    };
    CHECK(value(u8"Grid") == String(u8"129 x 129"));
    CHECK(value(u8"World") == String(u8"128 x 128 m"));
    CHECK(value(u8"Chunks") == String(u8"2 x 2 (4)")); // 129 -> 2x2 chunks
    CHECK(value(u8"Paint layers") == String(u8"2"));
    CHECK(value(u8"Casts shadows") == String(u8"Yes"));
}

TEST_CASE("TerrainFacts says when there is no heightfield")
{
    RefPtr<terrain::TerrainResource> res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
    res->castShadows = false;
    const Array<editor::TerrainFact> facts = editor::TerrainFacts(*res);
    REQUIRE_FALSE(facts.IsEmpty());
    CHECK(facts[0].value == String(u8"No heightfield"));
    CHECK(facts.Back().value == String(u8"No"));
}

TEST_CASE("TerrainAsset v2 blob snapshot round-trips its authored fields (the undo path)")
{
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.heightfieldId = Guid{11, 22};
    a.weightsId = Guid{33, 44};
    a.baseAlbedoId = Guid{55, 66};
    a.baseTileScale = 24.0f;
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteAlbedoIds.PushBack(Guid{});
    a.paletteTileScales.PushBack(16.0f);
    a.paletteTileScales.PushBack(48.0f);
    a.paletteTextureSize = 512;
    a.castShadows = false;

    // The page's snapshot shape: a bare BinarySerializer runs at ar.Version() == 0 and would take
    // the LEGACY read path, so snapshots are wrapped in the type's versioned payload.
    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }

    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }

    CHECK(b.heightfieldId == Guid{11, 22});
    CHECK(b.weightsId == Guid{33, 44});
    CHECK(b.baseAlbedoId == Guid{55, 66});
    CHECK(b.baseTileScale == doctest::Approx(24.0f));
    REQUIRE(b.paletteAlbedoIds.Size() == 2u);
    CHECK(b.paletteAlbedoIds[0] == Guid{77, 88});
    REQUIRE(b.paletteTileScales.Size() == 2u);
    CHECK(b.paletteTileScales[1] == doctest::Approx(48.0f));
    CHECK(b.paletteTextureSize == 512);
    CHECK(b.castShadows == false);
}

TEST_CASE("TerrainAsset v3 snapshot round-trips the per-layer normal + ORM ids (the undo path)")
{
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.baseAlbedoId = Guid{55, 66};
    a.baseNormalId = Guid{101, 102};
    a.baseOrmId = Guid{103, 104};
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteAlbedoIds.PushBack(Guid{99, 111});
    a.paletteTileScales.PushBack(16.0f);
    a.paletteTileScales.PushBack(48.0f);
    // A partially-authored palette: layer 0 has a normal but no ORM, layer 1 the reverse. The page
    // grows these arrays lazily (SetPaletteMap), so a snapshot must survive the ragged shape.
    a.paletteNormalIds.PushBack(Guid{201, 202});
    a.paletteNormalIds.PushBack(Guid{});
    a.paletteOrmIds.PushBack(Guid{});
    a.paletteOrmIds.PushBack(Guid{203, 204});

    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }

    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }

    CHECK(b.baseNormalId == Guid{101, 102});
    CHECK(b.baseOrmId == Guid{103, 104});
    REQUIRE(b.paletteNormalIds.Size() == 2u);
    CHECK(b.paletteNormalIds[0] == Guid{201, 202});
    CHECK(b.paletteNormalIds[1] == Guid{});
    REQUIRE(b.paletteOrmIds.Size() == 2u);
    CHECK(b.paletteOrmIds[0] == Guid{});
    CHECK(b.paletteOrmIds[1] == Guid{203, 204});
}

TEST_CASE("TerrainAsset snapshot with no maps round-trips empty normal/ORM arrays (never populated)")
{
    // With no maps authored, the arrays must stay empty across a snapshot so the page's lazy-grow
    // picker path (SetPaletteMap) stays the only thing that ever sizes them.
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.baseAlbedoId = Guid{55, 66};
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteTileScales.PushBack(16.0f);

    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }
    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }
    CHECK(b.baseNormalId == Guid{});
    CHECK(b.baseOrmId == Guid{});
    CHECK(b.paletteNormalIds.IsEmpty());
    CHECK(b.paletteOrmIds.IsEmpty());
}

TEST_CASE("TerrainAsset v4 snapshot round-trips per-layer height ids + contrast (the undo path)")
{
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.baseAlbedoId = Guid{55, 66};
    a.baseHeightId = Guid{131, 132};
    a.heightBlendContrast = 0.4f;
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteAlbedoIds.PushBack(Guid{99, 111});
    a.paletteTileScales.PushBack(16.0f);
    a.paletteTileScales.PushBack(48.0f);
    // A ragged height palette: layer 0 has a height map, layer 1 does not (the page grows the array
    // lazily via SetPaletteMap, so a snapshot must survive the ragged shape).
    a.paletteHeightIds.PushBack(Guid{211, 212});
    a.paletteHeightIds.PushBack(Guid{});

    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }

    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }

    CHECK(b.baseHeightId == Guid{131, 132});
    CHECK(b.heightBlendContrast == doctest::Approx(0.4f));
    REQUIRE(b.paletteHeightIds.Size() == 2u);
    CHECK(b.paletteHeightIds[0] == Guid{211, 212});
    CHECK(b.paletteHeightIds[1] == Guid{});
}

TEST_CASE("TerrainAsset snapshot with no height maps keeps the arrays empty + the default contrast")
{
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.baseAlbedoId = Guid{55, 66};
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteTileScales.PushBack(16.0f);

    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }
    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }
    CHECK(b.baseHeightId == Guid{});
    CHECK(b.paletteHeightIds.IsEmpty());
    CHECK(b.heightBlendContrast == doctest::Approx(0.25f)); // default preserved
}

TEST_CASE("TerrainAsset v5 snapshot round-trips the per-layer coverage mask ids (the undo path)")
{
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.baseAlbedoId = Guid{55, 66};
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteAlbedoIds.PushBack(Guid{99, 111});
    a.paletteTileScales.PushBack(16.0f);
    a.paletteTileScales.PushBack(48.0f);
    // A ragged mask palette: layer 0 has a mask, layer 1 does not (the page grows the array lazily).
    a.paletteMaskIds.PushBack(Guid{221, 222});
    a.paletteMaskIds.PushBack(Guid{});

    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }
    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }

    REQUIRE(b.paletteMaskIds.Size() == 2u);
    CHECK(b.paletteMaskIds[0] == Guid{221, 222});
    CHECK(b.paletteMaskIds[1] == Guid{});
}

TEST_CASE("TerrainAsset snapshot with no coverage masks keeps the array empty (never populated)")
{
    pipeline::RegisterTerrainAsset();
    pipeline::TerrainAsset a;
    a.baseAlbedoId = Guid{55, 66};
    a.paletteAlbedoIds.PushBack(Guid{77, 88});
    a.paletteTileScales.PushBack(16.0f);

    MemoryStream out;
    {
        BinarySerializer wr(out, SerializeMode::Write);
        BeginVersionedPayload(wr, pipeline::TerrainAsset::StaticType());
        a.Serialize(wr);
        REQUIRE(wr.IsOk());
    }
    MemoryStream in;
    (void)in.Write(out.Bytes().Data(), out.Bytes().Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    pipeline::TerrainAsset b;
    {
        BinarySerializer rd(in, SerializeMode::Read);
        BeginVersionedPayload(rd, pipeline::TerrainAsset::StaticType());
        b.Serialize(rd);
        REQUIRE(rd.IsOk());
    }
    CHECK(b.paletteMaskIds.IsEmpty());
}
