// DFFontAtlasBaker diagnostics: bake real Roboto glyphs and inspect where the MSDF baker places
// them in their cell, versus the known-good coverage (stb pack) path. This CONFIRMS the reported
// symptom (descenders like g/q clipped, glyphs mis-registered) by measuring the baked cell's ink
// distribution instead of eyeballing it on screen. Also a regression guard once the baker is fixed.

#include <doctest/doctest.h>
#include <cstdio>

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.distancefield;
import draconic.fonts.distancefield.baker;
import draconic.fonts.ttf;

using namespace draconic::foundation;
using namespace draconic::fonts;

namespace
{
    String AssetPath(const char* rel)
    {
        String p;
        for (const char* s = DRACONIC_FONTS_ASSET_DIR; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        for (const char* s = rel; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        return p;
    }

    IFont* LoadRoboto()
    {
        TrueTypeFontParser parser;
        const String path = AssetPath("/roboto/Roboto-Regular.ttf");
        Result<IFont*, FontLoadResult> parsed =
            parser.ParseFromFile(path.AsView(), FontLoadOptions::Default());
        return parsed.HasValue() ? parsed.Value() : nullptr;
    }

    struct InkStats
    {
        i32 total = 0;
        i32 topHalf = 0;
        i32 bottomHalf = 0;
        i32 minRow = -1; // first cell row (from top) with ink
        i32 maxRow = -1; // last cell row with ink
        i32 cellH = 0;
        i32 cellW = 0;
    };

    // Reconstruct the glyph's "inside" mask over its atlas cell and summarize its vertical
    // distribution. `stride` = bytes/pixel (1 for coverage R8, 4 for MSDF RGBA8); for MSDF the
    // "inside" test is median(r,g,b) > 0.5 (the shader's decode), for coverage it's alpha > 0.5.
    InkStats Analyze(const IFontAtlas* atlas, i32 stride, i32 codepoint)
    {
        InkStats s;
        AtlasRegion reg;
        if (!atlas->TryGetRegion(codepoint, reg))
            return s;
        const Span<const u8> px = atlas->PixelData();
        const u32 W = atlas->Width();
        s.cellH = reg.height;
        s.cellW = reg.width;
        const i32 halfway = reg.height / 2;
        for (i32 row = 0; row < reg.height; ++row)
        {
            for (i32 col = 0; col < reg.width; ++col)
            {
                const usize idx = (static_cast<usize>(reg.y + row) * W + (reg.x + col)) *
                                  static_cast<usize>(stride);
                if (idx + static_cast<usize>(stride) > px.Size())
                    continue;
                bool inside = false;
                if (stride == 1)
                    inside = px[idx] > 127;
                else
                {
                    const u8 r = px[idx + 0], g = px[idx + 1], b = px[idx + 2];
                    const u8 med = Max(Min(r, g), Min(Max(r, g), b)); // median of 3
                    inside = med > 127;
                }
                if (inside)
                {
                    ++s.total;
                    if (row < halfway)
                        ++s.topHalf;
                    else
                        ++s.bottomHalf;
                    if (s.minRow < 0)
                        s.minRow = row;
                    s.maxRow = row;
                }
            }
        }
        return s;
    }

    void Print(const char* label, i32 cp, const InkStats& s)
    {
        std::printf("  [%s '%c'] cell=%dx%d ink=%d top=%d bottom=%d rows=[%d..%d]\n", label,
                    static_cast<char>(cp), s.cellW, s.cellH, s.total, s.topHalf, s.bottomHalf,
                    s.minRow, s.maxRow);
    }
}

TEST_CASE("df.baker: MSDF glyph orientation matches the coverage path (not vertically flipped)")
{
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    FontLoadOptions cov;
    cov.pixelHeight = 48.0f;
    cov.atlasWidth = cov.atlasHeight = 1024;
    TrueTypeFontAtlasBaker covBaker;
    Result<IFontAtlas*, FontLoadResult> covR = covBaker.Bake(*font, cov);
    REQUIRE(covR.HasValue());
    IFontAtlas* coverage = covR.Value();

    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.atlasWidth = df.atlasHeight = 1024;
    DFFontAtlasBaker dfBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = dfBaker.Bake(*font, df);
    REQUIRE(dfR.HasValue());
    IFontAtlas* msdf = dfR.Value();

    // 'F' is strongly TOP-heavy (top bar + middle bar + left stem); a vertical flip inverts that.
    const i32 F = static_cast<i32>('F');
    const InkStats covF = Analyze(coverage, 1, F);
    const InkStats dfF = Analyze(msdf, 4, F);
    std::printf("orientation ('F'):\n");
    Print("coverage", F, covF);
    Print("msdf", F, dfF);

    REQUIRE(covF.total > 0);
    REQUIRE(dfF.total > 0);
    // 'F' is thin strokes: a correct decode fills only a fraction of the cell. If the winding is
    // reversed (msdfgen fills the exterior), nearly the WHOLE cell reads inside - guard that.
    CHECK(dfF.total < (dfF.cellW * dfF.cellH) / 2); // < 50% (F is ~15-20%)
    // Known-good coverage is top-heavy; the MSDF cell must agree in sign (else it's flipped).
    CHECK(covF.topHalf > covF.bottomHalf);
    CHECK(dfF.topHalf > dfF.bottomHalf);

    DefaultAllocator().Delete(coverage);
    DefaultAllocator().Delete(msdf);
    DefaultAllocator().Delete(font);
}

TEST_CASE("df.baker: descender glyphs keep their tail (g/q not clipped like a)")
{
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.atlasWidth = df.atlasHeight = 1024;
    DFFontAtlasBaker dfBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = dfBaker.Bake(*font, df);
    REQUIRE(dfR.HasValue());
    IFontAtlas* msdf = dfR.Value();

    // A descender glyph must have ink in the LOWER portion of its cell (the tail). If the baker
    // clips or mis-registers it, the bottom of the cell is empty and 'g' reads like 'a'.
    const i32 g = static_cast<i32>('g');
    const i32 o = static_cast<i32>('o'); // no descender - reference
    const InkStats dfG = Analyze(msdf, 4, g);
    const InkStats dfO = Analyze(msdf, 4, o);
    std::printf("descender:\n");
    Print("msdf", g, dfG);
    Print("msdf", o, dfO);

    REQUIRE(dfG.total > 0);
    REQUIRE(dfO.total > 0);
    // 'g' has a descender, 'o' does not: the 'g' cell should be taller AND its ink should reach
    // into the bottom third of the cell.
    CHECK(dfG.cellH > dfO.cellH);
    CHECK(dfG.maxRow > (dfG.cellH * 2) / 3);

    DefaultAllocator().Delete(msdf);
    DefaultAllocator().Delete(font);
}

TEST_CASE("df.baker: blank glyphs (space) get an advance-only region")
{
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.firstCodepoint = 32; // include space
    df.lastCodepoint = 126;
    df.atlasWidth = df.atlasHeight = 1024;
    DFFontAtlasBaker dfBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = dfBaker.Bake(*font, df);
    REQUIRE(dfR.HasValue());
    IFontAtlas* atlas = dfR.Value();

    // Space has no outline but MUST carry its advance, or cursor-walk draw paths
    // render "hello world" as "helloworld".
    AtlasRegion space;
    REQUIRE(atlas->TryGetRegion(' ', space));
    CHECK(space.IsEmpty());       // nothing to draw
    CHECK(space.advanceX > 0.0f); // but the cursor steps

    // GetGlyphQuad on it: cursor advances, nothing is emitted.
    f32 cursorX = 10.0f;
    GlyphQuad quad{};
    CHECK_FALSE(atlas->GetGlyphQuad(' ', cursorX, 20.0f, quad));
    CHECK(cursorX == doctest::Approx(10.0f + space.advanceX));

    // The advance matches the font's own metric for space, rescaled from the font's
    // parse size to the 48px bake.
    const f32 toBakeScale = 48.0f / font->PixelHeight();
    CHECK(space.advanceX ==
          doctest::Approx(font->GetGlyphInfo(' ').advanceWidth * toBakeScale).epsilon(0.02));

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
}
