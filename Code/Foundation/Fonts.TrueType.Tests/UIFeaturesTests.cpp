// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The font-driven shaper UI helpers (hit testing, cursor position, selection
// rects, font decoration metrics) over the bundled Roboto asset. The pure value-type cases live in
// the core types suite.
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

    IFont* LoadRoboto()
    {
        const String path = AssetPath("/Engine/Fonts/roboto/Roboto-Regular.ttf");
        Result<IFont*, FontLoadResult> parsed =
            FontParserFactory::ParseFromFile(path, FontLoadOptions::Default(),
                                             DefaultAllocator());
        return parsed.HasValue() ? parsed.Value() : nullptr;
    }

    Span<const GlyphPosition> AsSpan(const Array<GlyphPosition>& a)
    {
        return Span<const GlyphPosition>(a.Data(), a.Size());
    }
}

TEST_CASE("ttf.ui: HitTest on empty positions")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    const HitTestResult result = shaper.HitTest(*font, AsSpan(positions), 50, 0);
    CHECK(result.characterIndex == 0);
    CHECK_FALSE(result.isInside);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: TruncateToWidth ellipsizes overflow, keeps fitting text")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    const StringView text = u8"A fairly long asset name.png";
    const f32 fullW = font->MeasureString(text);

    // Comfortably wide: returned unchanged.
    CHECK(TruncateToWidth(*font, text, fullW + 20.0f).AsView() == text);

    // Sub-pixel overflow (within the 1px tolerance) is not truncated: a control sized to exactly fit
    // its text can measure a hair short after layout rounding, and must not collapse ("OK" to "...").
    CHECK(TruncateToWidth(*font, text, fullW - 0.5f).AsView() == text);

    // Too narrow: truncated, ends with the ellipsis, and fits the budget.
    const f32 budget = fullW * 0.5f;
    const String cut = TruncateToWidth(*font, text, budget);
    CHECK(cut.Size() < text.Size());
    REQUIRE(cut.Size() >= 3);
    CHECK(StringView(cut.Data() + cut.Size() - 3, 3) == StringView(u8"..."));
    CHECK(font->MeasureString(cut.AsView()) <= budget);

    // A short label no wider than the ellipsis is left alone even in a too-small box: truncating
    // "+" to "..." would make it wider, not narrower.
    const f32 plusW = font->MeasureString(u8"+");
    CHECK(TruncateToWidth(*font, u8"+", plusW * 0.4f).AsView() == StringView(u8"+"));

    // A long string in a box narrower than the ellipsis: just the ellipsis.
    CHECK(TruncateToWidth(*font, text, 1.0f).AsView() == StringView(u8"..."));

    // Empty input stays empty (no measuring).
    CHECK(TruncateToWidth(*font, StringView{}, 100.0f).Size() == 0);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: HitTest before text")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"Hello", 100, 0, positions);

    const HitTestResult result = shaper.HitTest(*font, AsSpan(positions), 50, 0);
    CHECK(result.characterIndex == 0);
    CHECK_FALSE(result.isTrailingHit);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: HitTest middle of a character")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"ABCDE", positions);
    REQUIRE(positions.Size() >= 3);

    const GlyphPosition& pos = positions[2];
    const f32 midX = pos.x + pos.advance * 0.5f;
    const HitTestResult result = shaper.HitTest(*font, AsSpan(positions), midX, 0);
    CHECK(result.characterIndex == 2);
    CHECK(result.isInside);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: HitTest trailing edge")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"AB", positions);
    REQUIRE(positions.Size() >= 1);

    const GlyphPosition& pos = positions[0];
    const f32 trailingX = pos.x + pos.advance * 0.75f;
    const HitTestResult result = shaper.HitTest(*font, AsSpan(positions), trailingX, 0);
    CHECK(result.characterIndex == 0);
    CHECK(result.isTrailingHit);
    CHECK(result.InsertionIndex() == 1);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: GetCursorPosition at start")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"Hello", 100, 0, positions);
    CHECK(shaper.GetCursorPosition(*font, AsSpan(positions), 0) == 100);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: GetCursorPosition at end")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"Hi", 0, 0, positions);
    REQUIRE(positions.Size() >= 2);

    const f32 cursorX =
        shaper.GetCursorPosition(*font, AsSpan(positions), static_cast<i32>(positions.Size()));
    const GlyphPosition& last = positions[positions.Size() - 1];
    CHECK(Abs(cursorX - (last.x + last.advance)) < 0.01f);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: GetCursorPosition in the middle")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"ABC", positions);
    REQUIRE(positions.Size() >= 3);
    CHECK(shaper.GetCursorPosition(*font, AsSpan(positions), 1) == positions[1].x);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: GetSelectionRects empty selection")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"Hello", positions);

    Array<foundation::fonts::Rectangle> rects;
    shaper.GetSelectionRects(*font, AsSpan(positions), SelectionRange(2, 2),
                             font->Metrics().lineHeight, rects);
    CHECK(rects.Size() == 0);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: GetSelectionRects single line")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    TrueTypeTextShaper shaper;
    Array<GlyphPosition> positions;
    (void)shaper.ShapeText(*font, u8"Hello", positions);

    Array<foundation::fonts::Rectangle> rects;
    shaper.GetSelectionRects(*font, AsSpan(positions), SelectionRange(1, 4),
                             font->Metrics().lineHeight, rects);
    REQUIRE(rects.Size() == 1);
    CHECK(rects[0].width > 0);
    CHECK(rects[0].height > 0);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

TEST_CASE("ttf.ui: font metrics carry decoration metrics")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);

    const TextDecorationMetrics decorations = font->Metrics().decorations;
    CHECK(decorations.underlineThickness >= 1);
    CHECK(decorations.strikethroughThickness >= 1);
    CHECK(decorations.underlinePosition > 0);
    CHECK(decorations.strikethroughPosition < 0);

    DefaultAllocator().Delete(font);
    TrueTypeFonts::Shutdown();
}

// ============================================================================================
// Clusters: the shaper's UI helpers work in characters through GlyphPosition::cluster, checked
// here under the one-to-one stb shaper and over hand-built positions that map characters to
// glyphs N to M.
// ============================================================================================

TEST_CASE("ttf.ui: clusters count characters, not bytes, and a line break has no glyph")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    TrueTypeTextShaper shaper;

    // a, e-acute (2 bytes), the euro sign (3 bytes), b.
    Array<GlyphPosition> positions;
    REQUIRE(shaper.ShapeText(*font, StringView(u8"aé€b"), positions).HasValue());
    REQUIRE(positions.Size() == 4u);
    for (i32 i = 0; i < 4; ++i)
    {
        CHECK(positions[static_cast<usize>(i)].cluster == i);
        CHECK(positions[static_cast<usize>(i)].clusterLength == 1);
        CHECK(positions[static_cast<usize>(i)].xOffset == 0.0f);
    }
    CHECK(positions[1].glyphId == font->GetGlyphInfo(0xE9).glyphIndex);
    CHECK(positions[2].glyphId == font->GetGlyphInfo(0x20AC).glyphIndex);

    // "ab\ncd" wrapped: the break is character 2 and draws nothing.
    Array<GlyphPosition> lines;
    f32 height = 0.0f;
    REQUIRE(shaper.ShapeTextWrapped(*font, StringView(u8"ab\ncd"), 1000.0f, lines, height).IsOk());
    REQUIRE(lines.Size() == 4u);
    CHECK(lines[1].cluster == 1);
    CHECK(lines[2].cluster == 3);
    CHECK(lines[2].y > lines[1].y);

    DefaultAllocator().Delete(font);
}

TEST_CASE("ttf.ui: hit tests, carets and selections across a line break go by cluster")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    TrueTypeTextShaper shaper;
    Array<GlyphPosition> lines;
    f32 height = 0.0f;
    REQUIRE(shaper.ShapeTextWrapped(*font, StringView(u8"ab\ncd"), 1000.0f, lines, height).IsOk());
    const f32 lineH = font->Metrics().lineHeight;
    const GlyphPosition b = lines[1];
    const GlyphPosition c = lines[2];

    // The trailing half of b puts the caret before the break; the leading half of c after it.
    CHECK(shaper.HitTestWrapped(*font, AsSpan(lines), b.x + b.advance * 0.9f, 0.1f * lineH, lineH).InsertionIndex() ==
          2);
    CHECK(shaper.HitTestWrapped(*font, AsSpan(lines), c.x + c.advance * 0.1f, 1.1f * lineH, lineH).InsertionIndex() ==
          3);
    // Past the end of the first line: before the break.
    CHECK(shaper.HitTestWrapped(*font, AsSpan(lines), 900.0f, 0.1f * lineH, lineH).InsertionIndex() == 2);

    // The caret before the break sits at b's end; after it, at c's start.
    CHECK(shaper.GetCursorPosition(*font, AsSpan(lines), 2) == doctest::Approx(b.x + b.advance));
    CHECK(shaper.GetCursorPosition(*font, AsSpan(lines), 3) == doctest::Approx(c.x));

    // Characters 1 to 4 (b, the break, c): one rectangle on each line.
    Array<foundation::fonts::Rectangle> rects;
    shaper.GetSelectionRects(*font, AsSpan(lines), SelectionRange(1, 4), lineH, rects);
    REQUIRE(rects.Size() == 2u);
    CHECK(rects[0].x == doctest::Approx(b.x));
    CHECK(rects[0].width == doctest::Approx(b.advance));
    CHECK(rects[1].x == doctest::Approx(c.x));

    DefaultAllocator().Delete(font);
}

TEST_CASE("ttf.ui: a cluster of several characters, or of several glyphs, is one caret stop")
{
    TrueTypeFonts::Initialize();
    IFont* font = LoadRoboto();
    REQUIRE(font != nullptr);
    TrueTypeTextShaper shaper;

    // What a real shaper may produce for "x" + a two-character ligature + a base and its mark:
    // glyph 0 is character 0; glyph 1 covers characters 1 and 2; glyphs 2 and 3 share
    // characters 3 and 4.
    Array<GlyphPosition> positions;
    const auto glyph = [&](i32 cluster, i32 length, f32 x, f32 advance)
    {
        GlyphPosition g;
        g.cluster = cluster;
        g.clusterLength = length;
        g.x = x;
        g.advance = advance;
        positions.PushBack(g);
    };
    glyph(0, 1, 0.0f, 10.0f);
    glyph(1, 2, 10.0f, 20.0f);
    glyph(3, 2, 30.0f, 10.0f);
    glyph(3, 2, 40.0f, 0.0f); // the mark: placed by its offsets, no advance

    // The trailing half of the ligature puts the caret after both its characters.
    CHECK(shaper.HitTest(*font, AsSpan(positions), 25.0f, 0.0f).InsertionIndex() == 3);
    CHECK(shaper.HitTest(*font, AsSpan(positions), 12.0f, 0.0f).InsertionIndex() == 1);
    // Past the end: after the last cluster, both its characters.
    CHECK(shaper.HitTest(*font, AsSpan(positions), 500.0f, 0.0f).InsertionIndex() == 5);
    // A caret inside a cluster stands at the cluster's start; after the last one, at the end.
    CHECK(shaper.GetCursorPosition(*font, AsSpan(positions), 2) == doctest::Approx(10.0f));
    CHECK(shaper.GetCursorPosition(*font, AsSpan(positions), 3) == doctest::Approx(30.0f));
    CHECK(shaper.GetCursorPosition(*font, AsSpan(positions), 4) == doctest::Approx(30.0f)); // the base's, not the mark's
    CHECK(shaper.GetCursorPosition(*font, AsSpan(positions), 5) == doctest::Approx(40.0f));
    // Selecting the ligature's first character selects its glyph.
    Array<foundation::fonts::Rectangle> rects;
    shaper.GetSelectionRects(*font, AsSpan(positions), SelectionRange(1, 2), 10.0f, rects);
    REQUIRE(rects.Size() == 1u);
    CHECK(rects[0].x == doctest::Approx(10.0f));
    CHECK(rects[0].width == doctest::Approx(20.0f));

    DefaultAllocator().Delete(font);
}
