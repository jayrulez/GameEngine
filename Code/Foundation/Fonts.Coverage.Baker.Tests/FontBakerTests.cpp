// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Drives the editor-time baking pipeline (FontBaker::Bake) over the bundled
// Roboto asset and confirms the produced BakedFont and BakedFontAtlas carry
// usable data.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // the engine data root (FindDataRoot / DataPath)
import foundation.fonts;
import foundation.fonts.coverage;
import foundation.fonts.coverage.baker;

using namespace foundation::core;
using namespace foundation::fonts;

namespace
{
    // "<data root>" + rel (rel starts with '/'): the bundled fonts are found the way
    // every executable finds engine data: the Data/.dataroot walk from the test binary.
    String AssetPath(const char* rel)
    {
        String p = foundation::vfs::FindDataRoot(); // the data root; rel names the font from there
        REQUIRE_FALSE(p.IsEmpty());
        for (const char* s = rel; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        return p;
    }

    // Read the bundled Roboto font bytes off disk.
    bool ReadFontBytes(Array<u8>& out)
    {
        FileStream fs(AssetPath("/Engine/Fonts/roboto/Roboto-Regular.ttf"), FileMode::Read);
        if (!fs.IsValid())
            return false;
        const i64 length = fs.Size();
        if (length <= 0)
            return false;
        out.Resize(static_cast<usize>(length));
        return fs.Read(out.Data(), static_cast<u64>(length)) == static_cast<u64>(length);
    }
}

TEST_CASE("importer: bake produces glyphs, metrics, atlas regions")
{
    Array<u8> bytes;
    REQUIRE(ReadFontBytes(bytes));

    Result<BakedFontData*, FontLoadResult> result =
        FontBaker::Bake(Span<const u8>(bytes.Data(), bytes.Size()), FontLoadOptions::Default(),
                           DefaultAllocator());
    REQUIRE(result.HasValue());
    BakedFontData* baked = result.Value();

    CHECK_FALSE(baked->font->FamilyName().IsEmpty());
    CHECK(baked->font->Metrics().ascent > 0);
    CHECK(baked->font->Metrics().descent < 0);
    CHECK(baked->font->Metrics().lineHeight > 0);
    CHECK(baked->font->PixelHeight() == FontLoadOptions::Default().pixelHeight);

    CHECK(baked->font->HasGlyph(static_cast<i32>('A')));
    CHECK(baked->font->HasGlyph(static_cast<i32>('z')));
    CHECK(baked->font->HasGlyph(static_cast<i32>('0')));
    CHECK(baked->atlas->Contains(baked->atlas->GlyphFor(static_cast<i32>('A'))));
    CHECK(baked->atlas->Contains(baked->atlas->GlyphFor(static_cast<i32>('z'))));
    CHECK(baked->atlas->Contains(baked->atlas->GlyphFor(static_cast<i32>('0'))));
    // A space keeps its glyph (it advances) but has nothing to draw.
    CHECK(baked->font->GetGlyphInfo(static_cast<i32>(' ')).advanceWidth > 0);
    AtlasRegion space;
    CHECK(baked->atlas->TryGetRegion(baked->atlas->GlyphFor(static_cast<i32>(' ')), space));
    CHECK(space.IsEmpty());
    CHECK(space.advanceX > 0);

    CHECK(baked->font->GetGlyphInfo(static_cast<i32>('A')).advanceWidth > 0);

    DefaultAllocator().Delete(baked);
}

TEST_CASE("importer: baked atlas matches requested dimensions")
{
    Array<u8> bytes;
    REQUIRE(ReadFontBytes(bytes));

    FontLoadOptions opts = FontLoadOptions::Default();
    opts.atlasWidth = 256;
    opts.atlasHeight = 256;
    opts.firstCodepoint = static_cast<i32>('A');
    opts.lastCodepoint = static_cast<i32>('Z');

    Result<BakedFontData*, FontLoadResult> result =
        FontBaker::Bake(Span<const u8>(bytes.Data(), bytes.Size()), opts, DefaultAllocator());
    REQUIRE(result.HasValue());
    BakedFontData* baked = result.Value();

    CHECK(baked->atlas->Width() == 256);
    CHECK(baked->atlas->Height() == 256);
    CHECK(baked->atlas->PixelData().Size() == 256u * 256u);

    DefaultAllocator().Delete(baked);
}

TEST_CASE("importer: bake fails cleanly on garbage bytes")
{
    Array<u8> junk;
    junk.Resize(256);
    for (usize i = 0; i < junk.Size(); ++i)
        junk[i] = 0xAB;

    Result<BakedFontData*, FontLoadResult> result =
        FontBaker::Bake(Span<const u8>(junk.Data(), junk.Size()), FontLoadOptions::Default(),
                           DefaultAllocator());
    CHECK_FALSE(result.HasValue());
}

TEST_CASE("importer: TakeOwnership nulls the BakedFontData fields")
{
    Array<u8> bytes;
    REQUIRE(ReadFontBytes(bytes));

    Result<BakedFontData*, FontLoadResult> result =
        FontBaker::Bake(Span<const u8>(bytes.Data(), bytes.Size()), FontLoadOptions::Default(),
                           DefaultAllocator());
    REQUIRE(result.HasValue());
    BakedFontData* bakedData = result.Value();

    BakedFont* takenFont = nullptr;
    BakedFontAtlas* takenAtlas = nullptr;
    bakedData->TakeOwnership(takenFont, takenAtlas);
    CHECK(takenFont != nullptr);
    CHECK(takenAtlas != nullptr);
    CHECK(bakedData->font == nullptr);
    CHECK(bakedData->atlas == nullptr);

    DefaultAllocator().Delete(bakedData); // must not touch the taken objects
    DefaultAllocator().Delete(takenFont);
    DefaultAllocator().Delete(takenAtlas);
}
