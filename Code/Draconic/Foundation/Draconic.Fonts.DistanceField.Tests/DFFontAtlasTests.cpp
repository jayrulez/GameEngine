#include <doctest/doctest.h>

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.distancefield;

using namespace draconic::foundation;
using namespace draconic::fonts;

TEST_CASE("DFFontAtlas: mode returns DistanceField")
{
    DFFontAtlas atlas;
    CHECK(atlas.Mode() == AtlasMode::DistanceField);
}

TEST_CASE("DFFontAtlas: pixel range default")
{
    DFFontAtlas atlas;
    CHECK(atlas.DistanceFieldRange() == doctest::Approx(4.0f));
}

TEST_CASE("DFFontAtlas: set and retrieve region")
{
    DFFontAtlas atlas;
    Array<u8> pixels(16 * 16 * 4);
    atlas.SetPixels(16, 16, Move(pixels));

    AtlasRegion region(2, 3, 8, 10, 1.0f, -2.0f, 9.0f);
    atlas.SetRegion(static_cast<i32>('A'), region);

    CHECK(atlas.Contains(static_cast<i32>('A')));
    CHECK_FALSE(atlas.Contains(static_cast<i32>('B')));

    AtlasRegion out{};
    CHECK(atlas.TryGetRegion(static_cast<i32>('A'), out));
    CHECK(out.x == 2);
    CHECK(out.width == 8);
    CHECK(out.advanceX == doctest::Approx(9.0f));
}

TEST_CASE("DFFontAtlas: glyph quad generation")
{
    DFFontAtlas atlas;
    Array<u8> pixels(64 * 64 * 4);
    atlas.SetPixels(64, 64, Move(pixels));

    AtlasRegion region(4, 4, 10, 12, 1.0f, -10.0f, 11.0f);
    atlas.SetRegion(static_cast<i32>('X'), region);

    f32 cursorX = 10.0f;
    GlyphQuad quad{};
    CHECK(atlas.GetGlyphQuad(static_cast<i32>('X'), cursorX, 20.0f, quad));
    CHECK(cursorX == doctest::Approx(10.0f + 11.0f));
    CHECK(quad.x0 == doctest::Approx(10.0f + 1.0f));
    CHECK(quad.y0 == doctest::Approx(20.0f + (-10.0f)));
}

TEST_CASE("DFFontAtlas: advance-only region steps the cursor without emitting a quad")
{
    DFFontAtlas atlas;
    Array<u8> pixels(64 * 64 * 4);
    atlas.SetPixels(64, 64, Move(pixels));

    atlas.SetRegion(static_cast<i32>(' '), AtlasRegion(0, 0, 0, 0, 0.0f, 0.0f, 13.0f));

    f32 cursorX = 10.0f;
    GlyphQuad quad{};
    CHECK_FALSE(atlas.GetGlyphQuad(static_cast<i32>(' '), cursorX, 20.0f, quad));
    CHECK(cursorX == doctest::Approx(23.0f)); // advanced despite drawing nothing

    CHECK_FALSE(atlas.GetGlyphQuadAt(static_cast<i32>(' '), 10.0f, 20.0f, quad));
}
