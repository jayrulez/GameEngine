// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DistanceFieldFontAtlasBaker diagnostics: bake real Roboto glyphs and inspect where the MSDF baker places
// them in their cell, versus the known-good coverage (stb pack) path. This CONFIRMS the reported
// symptom (descenders like g/q clipped, glyphs mis-registered) by measuring the baked cell's ink
// distribution instead of eyeballing it on screen. Also a regression guard once the baker is fixed.

#include <doctest/doctest.h>
#include <cstdio>
#include "Core/Prelude.h" // <new> for container instantiation under GCC modules

import foundation.core;
import foundation.vfs; // the engine data root (FindDataRoot / DataPath)
import foundation.fonts;
import foundation.fonts.distancefield;
import foundation.fonts.distancefield.baker;
import foundation.fonts.truetype;

using namespace foundation::core;
using namespace foundation::fonts;

namespace
{
    // "<data root>/Assets/fonts" + rel (rel starts with '/'): the bundled fonts are found the way
    // every executable finds engine data - the Data/.dataroot walk from the test binary.
    String AssetPath(const char* rel)
    {
        String p = foundation::vfs::DataPath(foundation::vfs::FindDataRoot(), u8"Assets/fonts");
        REQUIRE_FALSE(p.IsEmpty());
        for (const char* s = rel; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        return p;
    }

    IFont* LoadRoboto()
    {
        TrueTypeFontParser parser;
        const String path = AssetPath("/roboto/Roboto-Regular.ttf");
        Result<IFont*, FontLoadResult> parsed =
            parser.ParseFromFile(path.AsView(), FontLoadOptions::Default(), DefaultAllocator());
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
        if (!atlas->TryGetRegion(atlas->GlyphFor(codepoint), reg))
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
    Result<IFontAtlas*, FontLoadResult> covR = covBaker.Bake(*font, cov, DefaultAllocator());
    REQUIRE(covR.HasValue());
    IFontAtlas* coverage = covR.Value();

    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.atlasWidth = df.atlasHeight = 1024;
    DistanceFieldFontAtlasBaker distanceFieldBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = distanceFieldBaker.Bake(*font, df, DefaultAllocator());
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
    DistanceFieldFontAtlasBaker distanceFieldBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = distanceFieldBaker.Bake(*font, df, DefaultAllocator());
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
    DistanceFieldFontAtlasBaker distanceFieldBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = distanceFieldBaker.Bake(*font, df, DefaultAllocator());
    REQUIRE(dfR.HasValue());
    IFontAtlas* atlas = dfR.Value();

    // Space has no outline but MUST carry its advance, or cursor-walk draw paths
    // render "hello world" as "helloworld".
    AtlasRegion space;
    REQUIRE(atlas->TryGetRegion(atlas->GlyphFor(' '), space));
    CHECK(space.IsEmpty());       // nothing to draw
    CHECK(space.advanceX > 0.0f); // but the cursor steps

    // GetGlyphQuad on it: cursor advances, nothing is emitted.
    f32 cursorX = 10.0f;
    GlyphQuad quad{};
    CHECK_FALSE(atlas->GetGlyphQuad(atlas->GlyphFor(' '), cursorX, 20.0f, quad));
    CHECK(cursorX == doctest::Approx(10.0f + space.advanceX));

    // The advance matches the font's own metric for space, rescaled from the font's
    // parse size to the 48px bake.
    const f32 toBakeScale = 48.0f / font->PixelHeight();
    CHECK(space.advanceX ==
          doctest::Approx(font->GetGlyphInfo(' ').advanceWidth * toBakeScale).epsilon(0.02));

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
}

TEST_CASE("df.baker: alpha holds the true distance, round where the multi-channel field is square")
{
    // Text effects read alpha: an outline or a glow past the edge needs the true distance, whose
    // contours round a corner, where the median of the three channels keeps it square.
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.atlasWidth = df.atlasHeight = 1024;
    DistanceFieldFontAtlasBaker distanceFieldBaker;
    Result<IFontAtlas*, FontLoadResult> dfR = distanceFieldBaker.Bake(*font, df, DefaultAllocator());
    REQUIRE(dfR.HasValue());
    IFontAtlas* atlas = dfR.Value();

    // 'I' is one rectangle: four outer corners and nothing else.
    AtlasRegion reg;
    REQUIRE(atlas->TryGetRegion(atlas->GlyphFor('I'), reg));
    const Span<const u8> px = atlas->PixelData();
    const u32 width = atlas->Width();
    i32 medianInside = 0, alphaInside = 0, agree = 0;
    i32 medianGrown = 0, alphaGrown = 0;
    // 0.5 is the edge; 0.15 is 1.4 texels out with the baker's 4-texel range.
    const u8 grownAt = static_cast<u8>(0.15f * 255.0f);
    for (i32 row = 0; row < reg.height; ++row)
    {
        for (i32 col = 0; col < reg.width; ++col)
        {
            const u8* p = px.Data() + (static_cast<usize>(reg.y + row) * width + (reg.x + col)) * 4;
            const u8 median = Max(Min(p[0], p[1]), Min(Max(p[0], p[1]), p[2]));
            const bool inMedian = median > 127;
            const bool inAlpha = p[3] > 127;
            medianInside += inMedian ? 1 : 0;
            alphaInside += inAlpha ? 1 : 0;
            agree += inMedian == inAlpha ? 1 : 0;
            medianGrown += median > grownAt ? 1 : 0;
            alphaGrown += p[3] > grownAt ? 1 : 0;
        }
    }
    std::printf("'I' inside: median %d alpha %d agree %d of %d; grown: median %d alpha %d\n", medianInside,
                alphaInside, agree, reg.width * reg.height, medianGrown, alphaGrown);
    REQUIRE(medianInside > 0);
    // The same glyph: the two edges agree but for antialiasing rounding along the outline.
    CHECK(agree >= reg.width * reg.height * 98 / 100);
    // Alpha is a field, not the old constant 255: the cell's margin reads outside.
    CHECK(alphaInside < reg.width * reg.height / 2);
    // Grown past the edge, both cover more than the glyph; the round corners cover less.
    CHECK(alphaGrown > alphaInside);
    CHECK(alphaGrown < medianGrown);

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
}

TEST_CASE("df.baker: a wider distance range spreads the field further and pads the cells for it")
{
    // An effects font bakes a wider field so an outline or a glow reaches further past the edge.
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    DistanceFieldFontAtlasBaker distanceFieldBaker;
    const auto bake = [&](f32 range)
    {
        FontLoadOptions df = FontLoadOptions::DistanceField();
        df.pixelHeight = 48.0f;
        df.atlasWidth = df.atlasHeight = 1024;
        df.distanceFieldRange = range;
        Result<IFontAtlas*, FontLoadResult> baked = distanceFieldBaker.Bake(*font, df, DefaultAllocator());
        REQUIRE(baked.HasValue());
        return baked.Value();
    };
    IFontAtlas* narrow = bake(4.0f);
    IFontAtlas* wide = bake(12.0f);
    CHECK(narrow->DistanceFieldRange() == doctest::Approx(4.0f));
    CHECK(wide->DistanceFieldRange() == doctest::Approx(12.0f));

    AtlasRegion narrowI, wideI;
    REQUIRE(narrow->TryGetRegion(narrow->GlyphFor('I'), narrowI));
    REQUIRE(wide->TryGetRegion(wide->GlyphFor('I'), wideI));
    // Half the range is 6 texels, past the default padding of 4: two more each side.
    CHECK(wideI.width == narrowI.width + 4);
    CHECK(wideI.offsetX == doctest::Approx(narrowI.offsetX - 2.0f));

    // Along the middle row, from the stem out, alpha falls a third as fast: the texels that still
    // read above zero past the edge are about three times as many.
    const auto reach = [](const IFontAtlas* atlas, const AtlasRegion& reg)
    {
        const Span<const u8> px = atlas->PixelData();
        const i32 row = reg.y + reg.height / 2;
        i32 count = 0;
        for (i32 col = 0; col < reg.width; ++col)
        {
            const u8 a = px[(static_cast<usize>(row) * atlas->Width() + (reg.x + col)) * 4 + 3];
            count += a > 0 && a < 128 ? 1 : 0;
        }
        return count;
    };
    const i32 narrowReach = reach(narrow, narrowI);
    const i32 wideReach = reach(wide, wideI);
    std::printf("outside texels reading the field: range 4 %d, range 12 %d\n", narrowReach, wideReach);
    CHECK(wideReach >= narrowReach * 5 / 2);

    DefaultAllocator().Delete(narrow);
    DefaultAllocator().Delete(wide);
    DefaultAllocator().Delete(font);
}

TEST_CASE("df.baker: the atlas is sized to its glyphs, every glyph inside it and none overlapping")
{
    // The asset's atlas size is a maximum. A Latin set at 48 px fills a fraction of 1024 x 1024,
    // and every texel of the atlas ships in the cooked font, so the baker crops to what it packs.
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.firstCodepoint = 32;
    df.lastCodepoint = 255;
    df.atlasWidth = df.atlasHeight = 1024;
    DistanceFieldFontAtlasBaker baker;
    Result<IFontAtlas*, FontLoadResult> baked = baker.Bake(*font, df, DefaultAllocator());
    REQUIRE(baked.HasValue());
    IFontAtlas* atlas = baked.Value();

    const u32 W = atlas->Width();
    const u32 H = atlas->Height();
    CHECK(W <= df.atlasWidth);
    CHECK(H <= df.atlasHeight);
    CHECK(static_cast<u64>(W) * H * 2 <= static_cast<u64>(df.atlasWidth) * df.atlasHeight);
    CHECK(W % 4 == 0); // block-compressible later
    CHECK(H % 4 == 0);
    CHECK(atlas->PixelData().Size() == static_cast<usize>(W) * H * 4);

    // Every printable ASCII glyph with an outline is in the atlas (none dropped), and no two
    // glyphs share a texel.
    Array<AtlasRegion> drawn;
    for (i32 cp = df.firstCodepoint; cp <= df.lastCodepoint; ++cp)
    {
        AtlasRegion r;
        const bool has = atlas->TryGetRegion(atlas->GlyphFor(cp), r);
        if (cp >= 33 && cp <= 126)
            CHECK(has);
        if (!has || r.IsEmpty())
            continue;
        CHECK(static_cast<u32>(r.x) + r.width <= W);
        CHECK(static_cast<u32>(r.y) + r.height <= H);
        drawn.PushBack(r);
    }
    CHECK(drawn.Size() > 90);
    usize overlaps = 0;
    for (usize i = 0; i < drawn.Size(); ++i)
        for (usize j = i + 1; j < drawn.Size(); ++j)
        {
            const AtlasRegion& a = drawn[i];
            const AtlasRegion& b = drawn[j];
            if (a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height)
                ++overlaps;
        }
    CHECK(overlaps == 0);

    // The white texel solid-colour draws sample is white, in its own cell clear of every glyph.
    const Float2 uv = atlas->WhitePixelUV();
    const u32 wx = static_cast<u32>(uv.x * static_cast<f32>(W));
    const u32 wy = static_cast<u32>(uv.y * static_cast<f32>(H));
    REQUIRE(wx < W);
    REQUIRE(wy < H);
    const Span<const u8> px = atlas->PixelData();
    const usize at = (static_cast<usize>(wy) * W + wx) * 4;
    CHECK(px[at + 0] == 255);
    CHECK(px[at + 1] == 255);
    CHECK(px[at + 2] == 255);
    CHECK(px[at + 3] == 255);
    for (const AtlasRegion& r : drawn)
        CHECK_FALSE((wx >= r.x && wx < static_cast<u32>(r.x) + r.width && wy >= r.y &&
                     wy < static_cast<u32>(r.y) + r.height));

    DefaultAllocator().Delete(atlas);
    DefaultAllocator().Delete(font);
}

TEST_CASE("df.baker: glyphs that cannot fit the maximum fail the bake instead of vanishing")
{
    // Dropping a glyph draws that character as nothing, with no word why; the cook must say so.
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    DistanceFieldFontAtlasBaker baker;

    FontLoadOptions tooSmall = FontLoadOptions::DistanceField();
    tooSmall.pixelHeight = 48.0f;
    tooSmall.firstCodepoint = 32;
    tooSmall.lastCodepoint = 126;
    tooSmall.atlasWidth = tooSmall.atlasHeight = 128; // room for a handful of 48 px cells
    Result<IFontAtlas*, FontLoadResult> crowded = baker.Bake(*font, tooSmall, DefaultAllocator());
    REQUIRE_FALSE(crowded.HasValue());
    CHECK(crowded.Error() == FontLoadResult::AtlasPackingFailed);

    FontLoadOptions tooNarrow = tooSmall;
    tooNarrow.atlasWidth = 16; // narrower than one cell
    tooNarrow.atlasHeight = 4096;
    Result<IFontAtlas*, FontLoadResult> narrow = baker.Bake(*font, tooNarrow, DefaultAllocator());
    REQUIRE_FALSE(narrow.HasValue());
    CHECK(narrow.Error() == FontLoadResult::AtlasPackingFailed);

    DefaultAllocator().Delete(font);
}

TEST_CASE("df baker: baking is deterministic across runs (parallel-bake regression net)")
{
    // The MSDF generation fans out across worker threads; the pack order and every
    // baked byte must still be scheduling-independent. Two bakes of the same font +
    // options must agree exactly - regions and pixels both.
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.pixelHeight = 48.0f;
    df.firstCodepoint = 32;
    df.lastCodepoint = 255;
    df.atlasWidth = df.atlasHeight = 1024;

    DistanceFieldFontAtlasBaker baker;
    Result<IFontAtlas*, FontLoadResult> first = baker.Bake(*font, df, DefaultAllocator());
    Result<IFontAtlas*, FontLoadResult> second = baker.Bake(*font, df, DefaultAllocator());
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    IFontAtlas* a = first.Value();
    IFontAtlas* b = second.Value();

    CHECK(a->Width() == b->Width());
    CHECK(a->Height() == b->Height());

    usize regionCount = 0;
    for (i32 cp = df.firstCodepoint; cp <= df.lastCodepoint; ++cp)
    {
        AtlasRegion ra, rb;
        const bool ha = a->TryGetRegion(a->GlyphFor(cp), ra);
        const bool hb = b->TryGetRegion(b->GlyphFor(cp), rb);
        CHECK(ha == hb);
        if (ha && hb)
        {
            ++regionCount;
            CHECK(ra.x == rb.x);
            CHECK(ra.y == rb.y);
            CHECK(ra.width == rb.width);
            CHECK(ra.height == rb.height);
            CHECK(ra.advanceX == rb.advanceX);
        }
    }
    CHECK(regionCount > 90); // the printable-ASCII range alone is ~95 glyphs

    const Span<const u8> pa = a->PixelData();
    const Span<const u8> pb = b->PixelData();
    REQUIRE(pa.Size() == pb.Size());
    CHECK(MemCompare(pa.Data(), pb.Data(), pa.Size()) == 0);

    DefaultAllocator().Delete(a);
    DefaultAllocator().Delete(b);
    DefaultAllocator().Delete(font);
}

TEST_CASE("distancefield: Initialize is safe to call from many threads at once (the font cook does)")
{
    // FontAssetBuilder calls this per build on job workers. A plain null check let two of them
    // each create and register a baker: the second leaked and the factory list held both.
    JobSystem jobs(DefaultAllocator(), 4);
    jobs.ParallelFor(64, [](u32) { DistanceFieldFonts::Initialize(); }, 1);
    CHECK(DistanceFieldFonts::IsInitialized());
    // Shutdown re-arms it: a later Initialize creates exactly one baker again.
    DistanceFieldFonts::Shutdown();
    CHECK_FALSE(DistanceFieldFonts::IsInitialized());
    jobs.ParallelFor(64, [](u32) { DistanceFieldFonts::Initialize(); }, 1);
    CHECK(DistanceFieldFonts::IsInitialized());
    DistanceFieldFonts::Shutdown();
}
