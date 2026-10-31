// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The TTF backend suites (loader, font, atlas, shaper, service), over the bundled
// Roboto asset under the data root's Engine/Fonts.
#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // the engine data root (FindDataRoot / DataPath)
import foundation.fonts;
import foundation.fonts.truetype;

using namespace foundation::core;
using namespace foundation::fonts;

namespace
{
    // Build a UTF-8 path from the (ASCII) asset dir and a relative suffix.
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

    // Parse the bundled Roboto font with default options. Caller owns it.
    IFont* LoadRoboto()
    {
        const String path = AssetPath("/Engine/Fonts/roboto/Roboto-Regular.ttf");
        Result<IFont*, FontLoadResult> parsed =
            FontParserFactory::ParseFromFile(path, FontLoadOptions::Default(),
                                             DefaultAllocator());
        return parsed.HasValue() ? parsed.Value() : nullptr;
    }
}

// ============================ loader =====================================

TEST_CASE("ttf.loader: parser supports the right extensions")
{
    TrueTypeFontParser parser;
    CHECK(parser.SupportsExtension(u8".ttf"));
    CHECK(parser.SupportsExtension(u8".TTF"));
    CHECK(parser.SupportsExtension(u8".ttc"));
    CHECK(parser.SupportsExtension(u8".otf"));
    CHECK_FALSE(parser.SupportsExtension(u8".woff"));
    CHECK_FALSE(parser.SupportsExtension(u8".png"));
    CHECK_FALSE(parser.SupportsExtension(u8".txt"));
}

TEST_CASE("ttf.loader: baker supports the right extensions")
{
    TrueTypeFontAtlasBaker baker;
    CHECK(baker.SupportsExtension(u8".ttf"));
    CHECK(baker.SupportsExtension(u8".otf"));
    CHECK_FALSE(baker.SupportsExtension(u8".woff"));
}

TEST_CASE("ttf.loader: Initialize/Shutdown wires both factories")
{
    FontParserFactory::Shutdown();
    FontAtlasBakerFactory::Shutdown();
    CHECK_FALSE(TrueTypeFonts::IsInitialized());

    TrueTypeFonts::Initialize();
    CHECK(TrueTypeFonts::IsInitialized());
    CHECK(FontParserFactory::HasParsers());
    CHECK(FontAtlasBakerFactory::HasBakers());

    TrueTypeFonts::Shutdown();
    CHECK_FALSE(TrueTypeFonts::IsInitialized());
}

TEST_CASE("ttf.loader: parser factory errors with no parsers")
{
    FontParserFactory::Shutdown();
    Result<IFont*, FontLoadResult> result =
        FontParserFactory::ParseFromFile(u8"nonexistent.ttf", FontLoadOptions::Default(),
                                         DefaultAllocator());
    CHECK_FALSE(result.HasValue());
    CHECK(result.Error() == FontLoadResult::UnsupportedFormat);
}

// ============================ font =======================================

TEST_CASE("ttf.font: load + metrics")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    CHECK(font->PixelHeight() == FontLoadOptions::Default().pixelHeight);
    CHECK(font->Metrics().ascent > 0);
    CHECK(font->Metrics().descent < 0); // descent is typically negative
    CHECK(font->Metrics().lineHeight > 0);
    CHECK_FALSE(font->FamilyName().IsEmpty());

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.font: glyph info")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    const GlyphInfo infoA = font->GetGlyphInfo(static_cast<i32>('A'));
    CHECK(infoA.codepoint == static_cast<i32>('A'));
    CHECK(infoA.glyphIndex > 0);
    CHECK(infoA.advanceWidth > 0);
    CHECK(infoA.hasBitmap);

    const GlyphInfo spaceInfo = font->GetGlyphInfo(static_cast<i32>(' '));
    CHECK(spaceInfo.advanceWidth > 0);
    CHECK_FALSE(spaceInfo.hasBitmap);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.font: HasGlyph for printable ASCII")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    CHECK(font->HasGlyph(static_cast<i32>('A')));
    CHECK(font->HasGlyph(static_cast<i32>('z')));
    CHECK(font->HasGlyph(static_cast<i32>('0')));
    CHECK(font->HasGlyph(static_cast<i32>('!')));

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.font: MeasureString")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    const f32 width = font->MeasureString(u8"Hello World");
    CHECK(width > 0);
    CHECK(font->MeasureString(u8"Hello World, this is longer!") > width);
    CHECK(font->MeasureString(u8"") == 0);
    CHECK(font->MeasureString(u8"W") > font->MeasureString(u8"i"));

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

// ============================ atlas ======================================

TEST_CASE("ttf.atlas: creation + dimensions")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    Result<IFontAtlas*, FontLoadResult> baked =
        FontAtlasBakerFactory::Bake(*font, FontLoadOptions::Default(), DefaultAllocator());
    REQUIRE(baked.HasValue());
    IFontAtlas* atlas = baked.Value();

    CHECK(atlas->Width() == FontLoadOptions::Default().atlasWidth);
    CHECK(atlas->Height() == FontLoadOptions::Default().atlasHeight);
    CHECK(atlas->PixelData().Size() == static_cast<usize>(atlas->Width()) * atlas->Height());

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.atlas: Contains respects the codepoint range")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    Result<IFontAtlas*, FontLoadResult> baked =
        FontAtlasBakerFactory::Bake(*font, FontLoadOptions::Default(), DefaultAllocator());
    REQUIRE(baked.HasValue());
    IFontAtlas* atlas = baked.Value();

    // The range's codepoints map to their glyphs (by glyph index), the rest to none.
    CHECK(atlas->Contains(atlas->GlyphFor(static_cast<i32>('A'))));
    CHECK(atlas->Contains(atlas->GlyphFor(static_cast<i32>('z'))));
    CHECK(atlas->Contains(atlas->GlyphFor(static_cast<i32>('0'))));
    CHECK(atlas->GlyphFor(static_cast<i32>('A')) == font->GetGlyphInfo('A').glyphIndex);
    CHECK(atlas->GlyphFor(static_cast<i32>('A')) != static_cast<i32>('A'));
    CHECK(atlas->GlyphFor(0) == -1);
    CHECK(atlas->GlyphFor(31) == -1);
    CHECK(atlas->GlyphFor(127) == -1);
    CHECK(atlas->GlyphFor(200) == -1); // extended Latin not in default range
    CHECK_FALSE(atlas->Contains(0));

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.atlas: GetGlyphQuad")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    Result<IFontAtlas*, FontLoadResult> baked =
        FontAtlasBakerFactory::Bake(*font, FontLoadOptions::Default(), DefaultAllocator());
    REQUIRE(baked.HasValue());
    IFontAtlas* atlas = baked.Value();

    f32 cursorX = 0;
    GlyphQuad quad;
    CHECK(atlas->GetGlyphQuad(atlas->GlyphFor(static_cast<i32>('A')), cursorX, 0, quad));
    CHECK(quad.Width() > 0);
    CHECK(quad.Height() > 0);
    CHECK((quad.u0 >= 0 && quad.u0 <= 1));
    CHECK((quad.v0 >= 0 && quad.v0 <= 1));
    CHECK((quad.u1 >= 0 && quad.u1 <= 1));
    CHECK((quad.v1 >= 0 && quad.v1 <= 1));
    CHECK(cursorX > 0);

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

// ============================ shaper =====================================

TEST_CASE("ttf.shaper: ShapeText")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    Result<f32> shaped = shaper.ShapeText(*font, u8"ABC", positions);
    REQUIRE(shaped.HasValue());

    CHECK(positions.Size() == 3);
    CHECK(shaped.Value() > 0);
    CHECK(positions[0].x < positions[1].x);
    CHECK(positions[1].x < positions[2].x);
    // Glyph ids: the font's glyph indices for the characters.
    CHECK(positions[0].glyphId == font->GetGlyphInfo('A').glyphIndex);
    CHECK(positions[1].glyphId == font->GetGlyphInfo('B').glyphIndex);
    CHECK(positions[2].glyphId == font->GetGlyphInfo('C').glyphIndex);
    CHECK(positions[0].glyphId > 0);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.shaper: ShapeText with start position")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    Result<f32> shaped = shaper.ShapeText(*font, u8"AB", 100, 50, positions);
    REQUIRE(shaped.HasValue());

    CHECK(positions.Size() == 2);
    CHECK(positions[0].x == 100);
    CHECK(positions[0].y == 50);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.shaper: ShapeTextWrapped wraps on width")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    f32 totalHeight = 0;

    const f32 maxWidth = font->MeasureString(u8"Hello") + 10.0f;
    Status st = shaper.ShapeTextWrapped(*font, u8"Hello World", maxWidth, positions, totalHeight);
    REQUIRE(st.IsOk());

    CHECK(totalHeight > font->Metrics().lineHeight);
    bool hasSecondLine = false;
    for (const GlyphPosition& pos : positions)
        if (pos.y > 0)
        {
            hasSecondLine = true;
            break;
        }
    CHECK(hasSecondLine);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.shaper: ShapeTextWrapped honors explicit newlines")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    f32 totalHeight = 0;
    Status st = shaper.ShapeTextWrapped(*font, u8"A\nB", 1000, positions, totalHeight);
    REQUIRE(st.IsOk());

    bool foundSecondLine = false;
    for (const GlyphPosition& pos : positions)
        if (pos.y > 0)
        {
            foundSecondLine = true;
            break;
        }
    CHECK(foundSecondLine);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

// ============================ the service: sizes =========================

// A coverage family is baked at each size asked for; drawn from one bake, every label would come
// out at the bake's size whatever its font-size.
TEST_CASE("ttf.service: a coverage family answers each size with its own bake, cached")
{
    TrueTypeFonts::Initialize();
    TrueTypeFontService service(DefaultAllocator());
    REQUIRE(service.LoadFont(u8"Roboto", AssetPath("/Engine/Fonts/roboto/Roboto-Regular.ttf")) == FontLoadResult::Success);

    CachedFont* small = service.GetFont(u8"Roboto", 16.0f);
    CachedFont* large = service.GetFont(u8"Roboto", 40.0f);
    CachedFont* huge = service.GetFont(u8"Roboto", 72.0f); // past the first atlas: it grows
    REQUIRE(small != nullptr);
    REQUIRE(large != nullptr);
    REQUIRE(huge != nullptr);
    CHECK(small->font->PixelHeight() == doctest::Approx(16.0f));
    CHECK(large->font->PixelHeight() == doctest::Approx(40.0f));
    CHECK(huge->font->PixelHeight() == doctest::Approx(72.0f));
    CHECK(huge->font->Metrics().ascent > large->font->Metrics().ascent);
    CHECK(large->font->Metrics().ascent > small->font->Metrics().ascent);

    // Cached: the same size again is the same font, and a fraction rounds to the whole pixel.
    CHECK(service.GetFont(u8"Roboto", 40.0f) == large);
    CHECK(service.GetFont(u8"Roboto", 40.3f) == large);
    CHECK(service.GetAtlasTexture(u8"Roboto", 40.0f) == service.GetAtlasTexture(large));
    // An unknown family falls back to the default family's first bake.
    CachedFont* fallback = service.GetFont(u8"Nope", 24.0f);
    REQUIRE(fallback != nullptr);
    CHECK(fallback->font->PixelHeight() == doctest::Approx(FontLoadOptions::ExtendedLatin().pixelHeight));
    // Which is why HasFamily is asked, not GetFont.
    CHECK(service.HasFamily(u8"Roboto"));
    CHECK_FALSE(service.HasFamily(u8"Nope"));
}

TEST_CASE("ttf.service: a family loaded from memory bakes its other sizes from the kept bytes")
{
    TrueTypeFonts::Initialize();
    const String path = AssetPath("/Engine/Fonts/roboto/Roboto-Regular.ttf");
    Array<u8> bytes;
    {
        FileStream file(path.AsView(), FileMode::Read);
        REQUIRE(file.IsValid());
        bytes.Resize(static_cast<usize>(file.Size()));
        REQUIRE(file.Read(bytes.Data(), bytes.Size()) == bytes.Size());
    }
    TrueTypeFontService service(DefaultAllocator());
    REQUIRE(service.LoadFontFromMemory(u8"Roboto", Span<const u8>(bytes.Data(), bytes.Size())) ==
            FontLoadResult::Success);
    bytes.Clear(); // the service keeps its own copy
    CachedFont* big = service.GetFont(u8"Roboto", 30.0f);
    REQUIRE(big != nullptr);
    CHECK(big->font->PixelHeight() == doctest::Approx(30.0f));
}
