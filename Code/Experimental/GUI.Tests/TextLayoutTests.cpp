// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The inline formatter over the bundled Roboto faces: mixed sizes share a baseline, a paragraph of
// three colours wraps inside its width, an icon sits on the baseline, breaks and alignment, and bold
// and italic take a face or synthesize one. Copied with the layout from foundation.ui (E4 of
// Specs/gui-lab-evolution.md).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.vg;
import experimental.gui;

using namespace foundation::core;
using namespace experimental::gui;
namespace fonts = foundation::fonts;

namespace
{
    // Roboto, and Roboto Bold as its own family (as a project loads a bold face).
    struct RobotoService
    {
        fonts::TrueTypeFontService service{DefaultAllocator()};

        RobotoService()
        {
            fonts::TrueTypeFonts::Initialize();
            const String root = foundation::vfs::FindDataRoot();
            REQUIRE(service.LoadFont(u8"Roboto",
                                     foundation::vfs::DataPath(root.AsView(), u8"Assets/fonts/roboto/Roboto-Regular.ttf")
                                         .AsView()) == fonts::FontLoadResult::Success);
            REQUIRE(service.LoadFont(u8"Roboto Bold",
                                     foundation::vfs::DataPath(root.AsView(), u8"Assets/fonts/roboto/Roboto-Bold.ttf")
                                         .AsView()) == fonts::FontLoadResult::Success);
        }
    };

    // The label's font (Roboto 16), the service for runs that set their own.
    TextLayoutStyle Style(RobotoService& fonts, f32 maxWidth = 0.0f)
    {
        TextLayoutStyle style;
        style.Font = fonts.service.GetFont(u8"Roboto", 16.0f);
        style.Fonts = &fonts.service;
        style.Family = String(u8"Roboto");
        style.Size = 16.0f;
        style.MaxWidth = maxWidth;
        return style;
    }
}

TEST_CASE("text layout: runs of two sizes share one baseline, the line as tall as the larger")
{
    RobotoService fonts;
    const TextDocument doc = ParseTextMarkup(u8"small [size=32]BIG[/size] small");
    const TextLayout layout = LayOutText(doc, Style(fonts));
    REQUIRE(layout.Lines.Size() == 1u);
    REQUIRE(layout.GlyphRuns.Size() >= 3u);
    const f32 baseline = layout.Lines[0].Baseline;
    for (const LaidOutGlyphs& run : layout.GlyphRuns)
    {
        CHECK(run.Baseline == doctest::Approx(baseline));
        for (const fonts::GlyphPosition& g : run.Glyphs)
            CHECK(g.y == doctest::Approx(baseline));
    }
    fonts::CachedFont* big = fonts.service.GetFont(u8"Roboto", 32.0f);
    fonts::CachedFont* small = fonts.service.GetFont(u8"Roboto", 16.0f);
    REQUIRE(big != nullptr);
    REQUIRE(small != nullptr);
    CHECK(baseline == doctest::Approx(big->font->Metrics().ascent));
    CHECK(layout.Height > small->font->Metrics().lineHeight * 1.5f);
    // Runs follow on: each starts where the one before it ended.
    for (usize r = 1; r < layout.GlyphRuns.Size(); ++r)
        CHECK(layout.GlyphRuns[r].Glyphs[0].x > layout.GlyphRuns[r - 1].Glyphs[0].x);
}

TEST_CASE("text layout: a paragraph of three colours wraps inside its width and keeps its colours")
{
    RobotoService fonts;
    const TextDocument doc = ParseTextMarkup(
        u8"The [color=#ff0000]guests in red[/color] dance while [color=#00ff00]the ones in green[/color] "
        u8"watch and [color=#0000ff]the band in blue[/color] plays on into the night");
    const TextLayout layout = LayOutText(doc, Style(fonts, 160.0f));
    CHECK(layout.Lines.Size() >= 3u);
    f32 total = 0.0f;
    for (const LaidOutLine& line : layout.Lines)
    {
        CHECK(line.Width <= 160.0f + 0.01f);
        CHECK(line.Top == doctest::Approx(total));
        total += line.Height;
    }
    CHECK(layout.Height == doctest::Approx(total));
    bool red = false, green = false, blue = false;
    for (const LaidOutGlyphs& run : layout.GlyphRuns)
    {
        red = red || run.TextColor.r == 1.0f;
        green = green || run.TextColor.g == 1.0f;
        blue = blue || run.TextColor.b == 1.0f;
        for (const fonts::GlyphPosition& g : run.Glyphs)
            CHECK(g.x + g.advance <= 160.0f + 4.0f); // within the width (a space may hang over)
    }
    CHECK((red && green && blue));
}

TEST_CASE("text layout: an icon sits on the baseline as tall as its text is big")
{
    RobotoService fonts;
    const TextDocument doc = ParseTextMarkup(u8"a [size=20][icon=Icons/rose.png][/size] b");
    const TextLayout layout = LayOutText(doc, Style(fonts));
    REQUIRE(layout.Icons.Size() == 1u);
    const Rectangle icon = layout.Icons[0].Rect;
    CHECK(icon.width == doctest::Approx(20.0f));
    CHECK(icon.y + icon.height == doctest::Approx(layout.Lines[0].Baseline));
    CHECK(layout.Icons[0].Run == 1);
    // The text after it starts after it.
    CHECK(layout.GlyphRuns.Back().Glyphs[0].x >= icon.x + icon.width);
}

TEST_CASE("text layout: line breaks, an empty last line, and centring")
{
    RobotoService fonts;
    CHECK(LayOutText(ParseTextMarkup(u8"a\nb"), Style(fonts)).Lines.Size() == 2u);
    const TextLayout trailing = LayOutText(ParseTextMarkup(u8"a\n"), Style(fonts));
    REQUIRE(trailing.Lines.Size() == 2u);
    CHECK(trailing.Lines[1].Height > 0.0f);

    TextLayoutStyle centred = Style(fonts, 200.0f);
    centred.Align = TextHAlign::Center;
    const TextLayout layout = LayOutText(ParseTextMarkup(u8"middle"), centred);
    REQUIRE(layout.Lines.Size() == 1u);
    CHECK(layout.Lines[0].Left == doctest::Approx((200.0f - layout.Lines[0].Width) * 0.5f));
    CHECK(layout.GlyphRuns[0].Glyphs[0].x == doctest::Approx(layout.Lines[0].Left));
}

TEST_CASE("text layout: bold takes the bold face, italic without one is synthesized")
{
    RobotoService fonts;
    TextRun bold;
    bold.Bold = true;
    const ResolvedRunFont b = ResolveRunFont(bold, Style(fonts));
    REQUIRE(b.Font != nullptr);
    CHECK_FALSE(b.SyntheticBold);
    CHECK(b.Font != fonts.service.GetFont(u8"Roboto", 16.0f));
    TextRun italic;
    italic.Italic = true;
    const ResolvedRunFont i = ResolveRunFont(italic, Style(fonts));
    CHECK(i.SyntheticItalic);
    CHECK_FALSE(i.SyntheticBold);
    TextRun both;
    both.Bold = true;
    both.Italic = true;
    const ResolvedRunFont bi = ResolveRunFont(both, Style(fonts));
    CHECK_FALSE(bi.SyntheticBold); // the bold face, slanted
    CHECK(bi.SyntheticItalic);
}

TEST_CASE("text layout: underlines and links cover their runs")
{
    RobotoService fonts;
    const TextLayout layout =
        LayOutText(ParseTextMarkup(u8"see [u][link=hint]the hint[/link][/u] here"), Style(fonts));
    REQUIRE(layout.Links.Size() == 2u); // "the " and "hint"
    REQUIRE(layout.Underlines.Size() == 2u);
    const Rectangle first = layout.Links[0].Rect;
    CHECK(layout.LinkAt(Float2{first.x + 1.0f, first.y + first.height * 0.5f}) == 1);
    CHECK(layout.LinkAt(Float2{0.5f, first.y + first.height * 0.5f}) == -1); // "see"
    CHECK(layout.Underlines[0].Rect.y > layout.Lines[0].Baseline); // below the baseline
    // The underline runs on under the space between the words, and stops at the last letter.
    CHECK(layout.Underlines[0].Rect.x + layout.Underlines[0].Rect.width ==
          doctest::Approx(layout.Underlines[1].Rect.x));
}

TEST_CASE("text layout: plain runs draw in the label's font; with no service a bold run is synthesized in it")
{
    RobotoService fonts;
    TextLayoutStyle style = Style(fonts);
    const TextLayout plain = LayOutText(TextDocument::Plain(u8"plain"), style);
    REQUIRE(plain.GlyphRuns.Size() == 1u);
    CHECK(plain.GlyphRuns[0].Font == style.Font);
    style.Fonts = nullptr;
    TextRun bold;
    bold.Bold = true;
    const ResolvedRunFont b = ResolveRunFont(bold, style);
    CHECK(b.Font == style.Font);
    CHECK(b.SyntheticBold);
    // A size of its own needs the service: without one the run keeps the label's.
    TextRun big;
    big.Size = 32.0f;
    CHECK(ResolveRunFont(big, style).Font == style.Font);
    CHECK(ResolveRunFont(big, style).Size == doctest::Approx(16.0f));
}
