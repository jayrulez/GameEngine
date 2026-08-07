// Ported from Sedulous.Fonts.Tests/BakedFontTests.bf - pure-data exercises of
// BakedFont + BakedFontAtlas (no rasterizer / no TTF).
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.baked;

using namespace draconic::foundation;
using namespace draconic::fonts;

TEST_CASE("baked.font: metrics round-trip")
{
    BakedFont font;
    font.SetFamilyName(u8"Hand Rolled");
    font.SetPixelHeight(24);
    font.SetMetrics(FontMetrics(20.0f, -5.0f, 2.0f, 24.0f, 0.0625f));

    CHECK(font.FamilyName() == StringView(u8"Hand Rolled"));
    CHECK(font.PixelHeight() == 24);
    CHECK(font.Metrics().ascent == 20.0f);
    CHECK(font.Metrics().descent == -5.0f);
    CHECK(font.Metrics().lineGap == 2.0f);
    CHECK(font.Metrics().lineHeight == 27.0f); // ascent - descent + lineGap
    CHECK(font.Metrics().pixelHeight == 24);
    CHECK(font.Metrics().scale == 0.0625f);
}

TEST_CASE("baked.font: glyph table")
{
    BakedFont font;

    GlyphInfo infoA;
    infoA.codepoint = static_cast<i32>('A');
    infoA.glyphIndex = 17;
    infoA.advanceWidth = 12.5f;
    infoA.leftSideBearing = 0.5f;
    infoA.boundingBox = draconic::fonts::Rectangle(0, -10, 12, 10);
    infoA.hasBitmap = true;
    font.SetGlyph(static_cast<i32>('A'), infoA);

    CHECK(font.HasGlyph(static_cast<i32>('A')));
    CHECK_FALSE(font.HasGlyph(static_cast<i32>('Z')));

    const GlyphInfo readback = font.GetGlyphInfo(static_cast<i32>('A'));
    CHECK(readback.advanceWidth == 12.5f);
    CHECK(readback.glyphIndex == 17);
    CHECK(readback.hasBitmap);

    const GlyphInfo missing = font.GetGlyphInfo(static_cast<i32>('Z'));
    CHECK(missing.advanceWidth == 0);
    CHECK_FALSE(missing.hasBitmap);
}

TEST_CASE("baked.font: kerning")
{
    BakedFont font;
    font.SetKerning(static_cast<i32>('A'), static_cast<i32>('V'), -2.5f);
    font.SetKerning(static_cast<i32>('T'), static_cast<i32>('o'), -1.0f);

    CHECK(font.GetKerning(static_cast<i32>('A'), static_cast<i32>('V')) == -2.5f);
    CHECK(font.GetKerning(static_cast<i32>('T'), static_cast<i32>('o')) == -1.0f);
    CHECK(font.GetKerning(static_cast<i32>('V'), static_cast<i32>('A')) == 0); // reversed never set
    CHECK(font.GetKerning(static_cast<i32>('X'), static_cast<i32>('Y')) == 0); // unknown
}

TEST_CASE("baked.font: MeasureString uses kerning")
{
    BakedFont font;
    GlyphInfo a;
    a.advanceWidth = 10.0f;
    GlyphInfo b;
    b.advanceWidth = 10.0f;
    font.SetGlyph(static_cast<i32>('A'), a);
    font.SetGlyph(static_cast<i32>('B'), b);
    font.SetKerning(static_cast<i32>('A'), static_cast<i32>('B'), -3.0f);

    // "AB": advance(A) + kerning(A,B) + advance(B) = 10 + (-3) + 10 = 17.
    CHECK(font.MeasureString(u8"AB") == 17.0f);
    // "BA" has no kerning entry -> 20.
    CHECK(font.MeasureString(u8"BA") == 20.0f);
}

TEST_CASE("baked.font: MeasureString with glyph positions")
{
    BakedFont font;
    GlyphInfo a;
    a.advanceWidth = 10.0f;
    GlyphInfo b;
    b.advanceWidth = 10.0f;
    font.SetGlyph(static_cast<i32>('A'), a);
    font.SetGlyph(static_cast<i32>('B'), b);
    font.SetKerning(static_cast<i32>('A'), static_cast<i32>('B'), -3.0f);

    Array<GlyphPosition> positions;
    const f32 total = font.MeasureString(u8"AB", positions);

    CHECK(total == 17.0f);
    CHECK(positions.Size() == 2);
    CHECK(positions[0].x == 0);
    CHECK(positions[0].codepoint == static_cast<i32>('A'));
    CHECK(positions[1].x == 7.0f); // first advance + kerning
    CHECK(positions[1].codepoint == static_cast<i32>('B'));
}

TEST_CASE("baked.atlas: SetPixels takes ownership")
{
    BakedFontAtlas atlas;
    Array<u8> pixels;
    pixels.Resize(64 * 64);
    for (usize i = 0; i < pixels.Size(); ++i)
        pixels[i] = static_cast<u8>(i & 0xFF);

    atlas.SetPixels(64, 64, Move(pixels));

    CHECK(atlas.Width() == 64);
    CHECK(atlas.Height() == 64);
    CHECK(atlas.PixelData().Size() == 64 * 64);
    CHECK(atlas.PixelData()[0] == 0);
    CHECK(atlas.PixelData()[255] == 255);
}

TEST_CASE("baked.atlas: region table")
{
    BakedFontAtlas atlas;
    Array<u8> px;
    px.Resize(128 * 128);
    atlas.SetPixels(128, 128, Move(px));
    atlas.SetWhitePixelUV(0.99f, 0.99f);

    const AtlasRegion region(16, 32, 12, 14, 1.0f, -10.0f, 13.5f);
    atlas.SetRegion(static_cast<i32>('A'), region);

    CHECK(atlas.Contains(static_cast<i32>('A')));
    CHECK_FALSE(atlas.Contains(static_cast<i32>('Z')));

    AtlasRegion got;
    CHECK(atlas.TryGetRegion(static_cast<i32>('A'), got));
    CHECK(got.x == 16);
    CHECK(got.y == 32);
    CHECK(got.width == 12);
    CHECK(got.height == 14);
    CHECK(got.advanceX == 13.5f);

    const Float2 white = atlas.WhitePixelUV();
    CHECK(white.x == 0.99f);
    CHECK(white.y == 0.99f);
}

TEST_CASE("baked.atlas: GetGlyphQuad advances cursor")
{
    BakedFontAtlas atlas;
    Array<u8> px;
    px.Resize(128 * 128);
    atlas.SetPixels(128, 128, Move(px));
    const AtlasRegion region(16, 32, 12, 14, 1.0f, -10.0f, 13.5f);
    atlas.SetRegion(static_cast<i32>('A'), region);

    f32 cursorX = 100.0f;
    GlyphQuad quad;
    CHECK(atlas.GetGlyphQuad(static_cast<i32>('A'), cursorX, 50.0f, quad));

    CHECK(quad.x0 == 101.0f); // 100 + 1
    CHECK(quad.y0 == 40.0f);  // 50 + -10
    CHECK(quad.x1 == 113.0f); // 101 + 12
    CHECK(quad.y1 == 54.0f);  // 40 + 14
    CHECK(cursorX == 113.5f); // 100 + 13.5
}

TEST_CASE("baked.atlas: GetGlyphQuadAt does not advance")
{
    BakedFontAtlas atlas;
    Array<u8> px;
    px.Resize(64 * 64);
    atlas.SetPixels(64, 64, Move(px));
    atlas.SetRegion(static_cast<i32>('A'), AtlasRegion(0, 0, 16, 16, 0, -16, 18));

    GlyphQuad quad;
    CHECK(atlas.GetGlyphQuadAt(static_cast<i32>('A'), 100.0f, 50.0f, quad));
    CHECK(quad.x0 == 100.0f);
    CHECK(quad.y0 == 34.0f); // 50 + -16
}

TEST_CASE("baked.atlas: missing glyph returns false")
{
    BakedFontAtlas atlas;
    Array<u8> px;
    px.Resize(64 * 64);
    atlas.SetPixels(64, 64, Move(px));

    f32 cursorX = 0;
    GlyphQuad quad;
    CHECK_FALSE(atlas.GetGlyphQuad(static_cast<i32>('?'), cursorX, 0, quad));
    CHECK(cursorX == 0); // unchanged on miss
}
