// Ported from Sedulous.Fonts.Tests: Rectangle/AtlasRegion/GlyphInfo/FontMetrics/
// FontLoadOptions tests (the GPU/TTF-free type coverage).
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.fonts;

using namespace draconic::foundation;
using namespace draconic::fonts;

TEST_CASE("fonts.rect: construction, bounds, contains, FromBounds")
{
    const draconic::fonts::Rectangle def;
    CHECK(def.x == 0);
    CHECK(def.y == 0);
    CHECK(def.width == 0);
    CHECK(def.height == 0);
    CHECK(def.IsEmpty());

    const draconic::fonts::Rectangle r(10, 20, 100, 50);
    CHECK(r.x == 10);
    CHECK(r.width == 100);
    CHECK_FALSE(r.IsEmpty());
    CHECK(r.Left() == 10);
    CHECK(r.Top() == 20);
    CHECK(r.Right() == 110);
    CHECK(r.Bottom() == 70);

    CHECK(r.Contains(50, 40));
    CHECK(r.Contains(10, 20));        // top-left inclusive
    CHECK_FALSE(r.Contains(110, 70)); // bottom-right exclusive
    CHECK_FALSE(r.Contains(5, 40));
    CHECK_FALSE(r.Contains(50, 100));

    const draconic::fonts::Rectangle b = draconic::fonts::Rectangle::FromBounds(10, 20, 110, 70);
    CHECK(b.x == 10);
    CHECK(b.y == 20);
    CHECK(b.width == 100);
    CHECK(b.height == 50);
}

TEST_CASE("fonts.atlasRegion: construction + UVs")
{
    const AtlasRegion def;
    CHECK(def.width == 0);
    CHECK(def.advanceX == 0);
    CHECK(def.IsEmpty());

    const AtlasRegion region(10, 20, 32, 48, 2.0f, -5.0f, 30.0f);
    CHECK(region.x == 10);
    CHECK(region.height == 48);
    CHECK(region.offsetY == -5.0f);
    CHECK(region.advanceX == 30.0f);
    CHECK_FALSE(region.IsEmpty());

    const AtlasRegion uvRegion(64, 128, 32, 48, 0, 0, 0);
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    uvRegion.GetUVs(512, 512, u0, v0, u1, v1);
    CHECK(Abs(u0 - 0.125f) < 0.0001f);
    CHECK(Abs(v0 - 0.25f) < 0.0001f);
    CHECK(Abs(u1 - 0.1875f) < 0.0001f);
    CHECK(Abs(v1 - 0.34375f) < 0.0001f);
}

TEST_CASE("fonts.glyph: GlyphInfo defaults + GlyphQuad dimensions")
{
    const GlyphInfo info;
    CHECK(info.codepoint == 0);
    CHECK(info.glyphIndex == 0);
    CHECK(info.advanceWidth == 0);
    CHECK(info.leftSideBearing == 0);
    CHECK_FALSE(info.hasBitmap);

    const GlyphQuad quad(10, 20, 30, 50, 0, 0, 1, 1);
    CHECK(quad.Width() == 20);
    CHECK(quad.Height() == 30);

    const GlyphQuad def;
    CHECK(def.x0 == 0);
    CHECK(def.Width() == 0);
    CHECK(def.Height() == 0);
}

TEST_CASE("fonts.metrics: construction, line height, default")
{
    const FontMetrics m(20.0f, -5.0f, 2.0f, 24.0f, 0.5f);
    CHECK(m.ascent == 20.0f);
    CHECK(m.descent == -5.0f);
    CHECK(m.lineGap == 2.0f);
    CHECK(m.pixelHeight == 24.0f);
    CHECK(m.scale == 0.5f);
    CHECK(m.lineHeight == 27.0f); // 20 - (-5) + 2

    const FontMetrics d = FontMetrics::Default();
    CHECK(d.ascent == 0);
    CHECK(d.lineHeight == 0);
    CHECK(d.scale == 1.0f);
}

TEST_CASE("fonts.loadOptions: presets + character count")
{
    auto isPow2 = [](u32 v) { return v > 0 && (v & (v - 1)) == 0; };

    const FontLoadOptions def = FontLoadOptions::Default();
    CHECK(def.pixelHeight > 0);
    CHECK(def.firstCodepoint >= 32);
    CHECK(def.lastCodepoint >= def.firstCodepoint);
    CHECK((def.atlasWidth > 0 && isPow2(def.atlasWidth)));
    CHECK((def.atlasHeight > 0 && isPow2(def.atlasHeight)));
    CHECK(def.oversampleX >= 1);
    CHECK(def.CharacterCount() == 95); // 32..126

    const FontLoadOptions ext = FontLoadOptions::ExtendedLatin();
    CHECK(ext.lastCodepoint >= 255);
    CHECK(ext.atlasWidth >= 512);

    const FontLoadOptions small = FontLoadOptions::Small();
    CHECK(small.pixelHeight == 16.0f);
    CHECK(small.atlasWidth == 256);

    const FontLoadOptions large = FontLoadOptions::Large();
    CHECK(large.pixelHeight == 64.0f);
    CHECK(large.atlasWidth >= 1024);
}

// --- UI value types (ported from UIFeaturesTests.bf) ----------------------

TEST_CASE("fonts.hitTestResult: insertion index")
{
    CHECK(HitTestResult(5, false, true).InsertionIndex() == 5); // leading edge
    CHECK(HitTestResult(5, true, true).InsertionIndex() == 6);  // trailing edge
}

TEST_CASE("fonts.selectionRange: normalization, empty, contains")
{
    SelectionRange range(2, 5);
    CHECK(range.start == 2);
    CHECK(range.end == 5);
    CHECK(range.Length() == 3);

    const SelectionRange reversed(5, 2);
    CHECK(reversed.start == 2); // normalized
    CHECK(reversed.end == 5);

    CHECK(SelectionRange(3, 3).IsEmpty());
    CHECK(SelectionRange(3, 3).Length() == 0);
    CHECK_FALSE(SelectionRange(2, 5).IsEmpty());

    const SelectionRange r(2, 5);
    CHECK_FALSE(r.Contains(1));
    CHECK(r.Contains(2));
    CHECK(r.Contains(4));
    CHECK_FALSE(r.Contains(5)); // end exclusive
}

TEST_CASE("fonts.textDecorationMetrics: defaults + from-font")
{
    const TextDecorationMetrics def;
    CHECK(def.underlineThickness == 1);
    CHECK(def.strikethroughThickness == 1);

    const TextDecorationMetrics m = TextDecorationMetrics::FromFontMetrics(24, 32);
    CHECK(m.underlinePosition > 0);     // below baseline
    CHECK(m.strikethroughPosition < 0); // above baseline
    CHECK(m.underlineThickness >= 1);
    CHECK(m.strikethroughThickness >= 1);
}

// --- scaled views (draconic.fonts:scaled_views) -------------------------------------

namespace
{
    // A fixed 32px "font" with one glyph ('A': advance 20, lsb 2, bbox 1,2,3,4) and
    // kerning A->A = -1.5.
    class StubFont final : public IFont
    {
    public:
        [[nodiscard]] StringView FamilyName() const override { return u8"Stub"; }
        [[nodiscard]] f32 PixelHeight() const override { return 32.0f; }
        [[nodiscard]] FontMetrics Metrics() const override
        {
            return FontMetrics(24.0f, -8.0f, 4.0f, 32.0f, 1.0f);
        }
        [[nodiscard]] GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            GlyphInfo info;
            info.codepoint = codepoint;
            info.glyphIndex = 1;
            info.advanceWidth = 20.0f;
            info.leftSideBearing = 2.0f;
            info.boundingBox = draconic::fonts::Rectangle(1.0f, 2.0f, 3.0f, 4.0f);
            return info;
        }
        [[nodiscard]] f32 GetKerning(i32, i32) const override { return -1.5f; }
        [[nodiscard]] bool HasGlyph(i32) const override { return true; }
        [[nodiscard]] f32 MeasureString(StringView text) const override
        {
            return static_cast<f32>(text.Size()) * 20.0f;
        }
        [[nodiscard]] f32 MeasureString(StringView text,
                                        Array<GlyphPosition>& outPositions) const override
        {
            outPositions.Clear();
            return MeasureString(text);
        }
    };

    // A 128x128 DF "atlas" whose one region sits at texels (10,20,30,40) with
    // offsets (3,-24) and advance 20.
    class StubAtlas final : public IFontAtlas
    {
    public:
        [[nodiscard]] u32 Width() const override { return 128; }
        [[nodiscard]] u32 Height() const override { return 128; }
        [[nodiscard]] Span<const u8> PixelData() const override { return {}; }
        [[nodiscard]] bool Contains(i32) const override { return true; }
        [[nodiscard]] Float2 WhitePixelUV() const override { return Float2(0.5f, 0.5f); }
        [[nodiscard]] AtlasMode Mode() const override { return AtlasMode::DistanceField; }
        [[nodiscard]] f32 DistanceFieldRange() const override { return 4.0f; }
        [[nodiscard]] bool TryGetRegion(i32 codepoint, AtlasRegion& region) const override
        {
            if (codepoint == ' ') // advance-only whitespace region
            {
                region = AtlasRegion(0, 0, 0, 0, 0.0f, 0.0f, 12.0f);
                return true;
            }
            region = AtlasRegion(10, 20, 30, 40, 3.0f, -24.0f, 20.0f);
            return true;
        }
        [[nodiscard]] bool GetGlyphQuad(i32 codepoint, f32& cursorX, f32 cursorY,
                                        GlyphQuad& quad) const override
        {
            AtlasRegion region;
            (void)TryGetRegion(codepoint, region);
            (void)GetGlyphQuadAt(codepoint, cursorX + region.offsetX, cursorY + region.offsetY,
                                 quad);
            cursorX += region.advanceX;
            return true;
        }
        [[nodiscard]] bool GetGlyphQuadAt(i32, f32 x, f32 y, GlyphQuad& quad) const override
        {
            quad = GlyphQuad(x, y, x + 30.0f, y + 40.0f, 0, 0, 1, 1);
            return true;
        }
    };
}

TEST_CASE("fonts.scaledViews: font view scales metrics/advances/kerning, keeps identity")
{
    const StubFont base;
    const ScaledFontView view(base, 16.0f); // half size

    CHECK(view.Scale() == doctest::Approx(0.5f));
    CHECK(view.PixelHeight() == doctest::Approx(16.0f));
    CHECK(view.FamilyName() == StringView(u8"Stub"));
    CHECK(view.HasGlyph('A'));

    const FontMetrics metrics = view.Metrics();
    CHECK(metrics.ascent == doctest::Approx(12.0f));
    CHECK(metrics.descent == doctest::Approx(-4.0f));
    CHECK(metrics.lineGap == doctest::Approx(2.0f));
    CHECK(metrics.lineHeight == doctest::Approx(18.0f)); // ascent - descent + lineGap
    CHECK(metrics.pixelHeight == doctest::Approx(16.0f));

    const GlyphInfo info = view.GetGlyphInfo('A');
    CHECK(info.advanceWidth == doctest::Approx(10.0f));
    CHECK(info.leftSideBearing == doctest::Approx(1.0f));
    CHECK(info.boundingBox.width == doctest::Approx(1.5f));
    CHECK(info.boundingBox.height == doctest::Approx(2.0f));

    CHECK(view.GetKerning('A', 'A') == doctest::Approx(-0.75f));
    CHECK(view.MeasureString(u8"AA") == doctest::Approx(20.0f));

    Array<GlyphPosition> positions;
    const f32 width = view.MeasureString(u8"AA", positions);
    REQUIRE(positions.Size() == 2);
    CHECK(positions[0].x == doctest::Approx(0.0f));
    // Second glyph: advance 10 + kerning -0.75.
    CHECK(positions[1].x == doctest::Approx(9.25f));
    CHECK(width == doctest::Approx(19.25f));
}

TEST_CASE("fonts.scaledViews: atlas view scales geometry, keeps texels + UVs + DF facts")
{
    const StubAtlas base;
    const ScaledFontAtlasView view(base, 0.5f);

    CHECK(view.Width() == 128);
    CHECK(view.Height() == 128);
    CHECK(view.Mode() == AtlasMode::DistanceField);
    CHECK(view.DistanceFieldRange() == doctest::Approx(4.0f));
    CHECK(view.Contains('A'));

    AtlasRegion region;
    REQUIRE(view.TryGetRegion('A', region));
    CHECK(region.x == 10);          // texel rect untouched
    CHECK(region.width == 30);
    CHECK(region.offsetX == doctest::Approx(1.5f));
    CHECK(region.offsetY == doctest::Approx(-12.0f));
    CHECK(region.advanceX == doctest::Approx(10.0f));

    GlyphQuad quad;
    f32 cursorX = 100.0f;
    REQUIRE(view.GetGlyphQuad('A', cursorX, 50.0f, quad));
    CHECK(quad.x0 == doctest::Approx(101.5f));  // 100 + 3*0.5
    CHECK(quad.y0 == doctest::Approx(38.0f));   // 50 - 24*0.5
    CHECK(quad.Width() == doctest::Approx(15.0f));
    CHECK(quad.Height() == doctest::Approx(20.0f));
    CHECK(cursorX == doctest::Approx(110.0f));  // advanced by 20*0.5
    // UVs still index the BASE atlas texels.
    CHECK(quad.u0 == doctest::Approx(10.0f / 128.0f));
    CHECK(quad.v1 == doctest::Approx(60.0f / 128.0f));

    REQUIRE(view.GetGlyphQuadAt('A', 10.0f, 10.0f, quad));
    CHECK(quad.x0 == doctest::Approx(11.5f));
    CHECK(quad.y0 == doctest::Approx(-2.0f)); // 10 - 24*0.5

    // Whitespace: the advance-only region steps the cursor (scaled), emits nothing.
    cursorX = 100.0f;
    CHECK_FALSE(view.GetGlyphQuad(' ', cursorX, 50.0f, quad));
    CHECK(cursorX == doctest::Approx(106.0f)); // 12 * 0.5
    CHECK_FALSE(view.GetGlyphQuadAt(' ', 10.0f, 10.0f, quad));
}
