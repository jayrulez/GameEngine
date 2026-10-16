// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The inline formatter (F2 of Specs/vg-and-gui-lab.md) over the bundled Roboto faces: mixed sizes
// share a baseline, a paragraph of three colours wraps inside its width, an icon sits on the
// baseline, breaks and alignment, and bold and italic take a face or synthesize one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.ui;
import foundation.vg;
#include "TestHelpers.h"

using namespace foundation::core;
using namespace foundation::ui;
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

    TextLayoutStyle Style(f32 maxWidth = 0.0f)
    {
        TextLayoutStyle style;
        style.family = String(u8"Roboto");
        style.size = 16.0f;
        style.maxWidth = maxWidth;
        return style;
    }
}

TEST_CASE("text layout: runs of two sizes share one baseline, the line as tall as the larger")
{
    RobotoService fonts;
    const TextDocument doc = ParseTextMarkup(u8"small [size=32]BIG[/size] small");
    const TextLayout layout = LayOutText(doc, Style(), fonts.service);
    REQUIRE(layout.lines.Size() == 1u);
    REQUIRE(layout.glyphRuns.Size() >= 3u);
    const f32 baseline = layout.lines[0].baseline;
    for (const LaidOutGlyphs& run : layout.glyphRuns)
    {
        CHECK(run.baseline == doctest::Approx(baseline));
        for (const fonts::GlyphPosition& g : run.glyphs)
            CHECK(g.y == doctest::Approx(baseline));
    }
    fonts::CachedFont* big = fonts.service.GetFont(u8"Roboto", 32.0f);
    fonts::CachedFont* small = fonts.service.GetFont(u8"Roboto", 16.0f);
    REQUIRE(big != nullptr);
    REQUIRE(small != nullptr);
    CHECK(baseline == doctest::Approx(big->font->Metrics().ascent));
    CHECK(layout.height > small->font->Metrics().lineHeight * 1.5f);
    // Runs follow on: each starts where the one before it ended.
    for (usize r = 1; r < layout.glyphRuns.Size(); ++r)
        CHECK(layout.glyphRuns[r].glyphs[0].x > layout.glyphRuns[r - 1].glyphs[0].x);
}

TEST_CASE("text layout: a paragraph of three colours wraps inside its width and keeps its colours")
{
    RobotoService fonts;
    const TextDocument doc = ParseTextMarkup(
        u8"The [color=#ff0000]guests in red[/color] dance while [color=#00ff00]the ones in green[/color] "
        u8"watch and [color=#0000ff]the band in blue[/color] plays on into the night");
    const TextLayout layout = LayOutText(doc, Style(160.0f), fonts.service);
    CHECK(layout.lines.Size() >= 3u);
    f32 total = 0.0f;
    for (const LaidOutLine& line : layout.lines)
    {
        CHECK(line.width <= 160.0f + 0.01f);
        CHECK(line.top == doctest::Approx(total));
        total += line.height;
    }
    CHECK(layout.height == doctest::Approx(total));
    bool red = false, green = false, blue = false;
    for (const LaidOutGlyphs& run : layout.glyphRuns)
    {
        red = red || run.color.r == 1.0f;
        green = green || run.color.g == 1.0f;
        blue = blue || run.color.b == 1.0f;
        for (const fonts::GlyphPosition& g : run.glyphs)
            CHECK(g.x + g.advance <= 160.0f + 4.0f); // within the width (a space may hang over)
    }
    CHECK((red && green && blue));
}

TEST_CASE("text layout: an icon sits on the baseline as tall as its text is big")
{
    RobotoService fonts;
    const TextDocument doc = ParseTextMarkup(u8"a [size=20][icon=Icons/rose.png][/size] b");
    const TextLayout layout = LayOutText(doc, Style(), fonts.service);
    REQUIRE(layout.icons.Size() == 1u);
    const Rectangle icon = layout.icons[0].rect;
    CHECK(icon.width == doctest::Approx(20.0f));
    CHECK(icon.y + icon.height == doctest::Approx(layout.lines[0].baseline));
    CHECK(layout.icons[0].run == 1);
    // The text after it starts after it.
    CHECK(layout.glyphRuns.Back().glyphs[0].x >= icon.x + icon.width);
}

TEST_CASE("text layout: line breaks, an empty last line, and centring")
{
    RobotoService fonts;
    CHECK(LayOutText(ParseTextMarkup(u8"a\nb"), Style(), fonts.service).lines.Size() == 2u);
    const TextLayout trailing = LayOutText(ParseTextMarkup(u8"a\n"), Style(), fonts.service);
    REQUIRE(trailing.lines.Size() == 2u);
    CHECK(trailing.lines[1].height > 0.0f);

    TextLayoutStyle centred = Style(200.0f);
    centred.align = fonts::TextAlignment::Center;
    const TextLayout layout = LayOutText(ParseTextMarkup(u8"middle"), centred, fonts.service);
    REQUIRE(layout.lines.Size() == 1u);
    CHECK(layout.lines[0].left == doctest::Approx((200.0f - layout.lines[0].width) * 0.5f));
    CHECK(layout.glyphRuns[0].glyphs[0].x == doctest::Approx(layout.lines[0].left));
}

TEST_CASE("text layout: bold takes the bold face, italic without one is synthesized")
{
    RobotoService fonts;
    TextRun bold;
    bold.bold = true;
    const ResolvedRunFont b = ResolveRunFont(bold, Style(), fonts.service);
    REQUIRE(b.font != nullptr);
    CHECK_FALSE(b.syntheticBold);
    CHECK(b.font != fonts.service.GetFont(u8"Roboto", 16.0f));
    TextRun italic;
    italic.italic = true;
    const ResolvedRunFont i = ResolveRunFont(italic, Style(), fonts.service);
    CHECK(i.syntheticItalic);
    CHECK_FALSE(i.syntheticBold);
    TextRun both;
    both.bold = true;
    both.italic = true;
    const ResolvedRunFont bi = ResolveRunFont(both, Style(), fonts.service);
    CHECK_FALSE(bi.syntheticBold); // the bold face, slanted
    CHECK(bi.syntheticItalic);
}

TEST_CASE("text layout: underlines and links cover their runs")
{
    RobotoService fonts;
    const TextLayout layout =
        LayOutText(ParseTextMarkup(u8"see [u][link=hint]the hint[/link][/u] here"), Style(), fonts.service);
    REQUIRE(layout.links.Size() == 2u); // "the " and "hint"
    REQUIRE(layout.underlines.Size() == 2u);
    const Rectangle first = layout.links[0].rect;
    CHECK(layout.LinkAt(Float2{first.x + 1.0f, first.y + first.height * 0.5f}) == 1);
    CHECK(layout.LinkAt(Float2{0.5f, first.y + first.height * 0.5f}) == -1); // "see"
    CHECK(layout.underlines[0].rect.y > layout.lines[0].baseline); // below the baseline
    // The underline runs on under the space between the words, and stops at the last letter.
    CHECK(layout.underlines[0].rect.x + layout.underlines[0].rect.width ==
          doctest::Approx(layout.underlines[1].rect.x));
}

// ============================================================================================
// The RichText control over the formatter.
// ============================================================================================

TEST_CASE("rich text: measures wrapped at its width and draws every run")
{
    RobotoService fonts;
    UIContext ctx{DefaultAllocator()};
    ctx.SetFontService(&fonts.service);
    auto root = MakeRef<RootView>(DefaultAllocator());
    tests::Init(ctx, root.Get(), 400, 300);
    auto text = MakeRef<RichText>(DefaultAllocator(),
                                  StringView(u8"The [color=#ff0000]guests in red[/color] dance while [b]the band[/b] "
                                             u8"plays on into the night and the masks turn"));
    text->FontFamily.SetValue(String(u8"Roboto"));
    root->AddView(text.Get());
    tests::LayoutPass(ctx, root.Get());
    // Measured loosely at a width: wrapped there, as tall as its lines.
    text->Measure(BoxConstraints::Loose(160.0f, 1000.0f));
    CHECK(text->Layout().lines.Size() >= 3u);
    CHECK(text->MeasuredSize.x <= 160.0f + 0.01f);
    CHECK(text->MeasuredSize.y == doctest::Approx(text->Layout().height));

    foundation::vg::VGContext canvas;
    ctx.DrawRootView(root.Get(), canvas);
    CHECK(canvas.GetBatch().commands.Size() > 0u);
}

TEST_CASE("rich text: a click on a link raises OnLink with its id; a click elsewhere does not")
{
    RobotoService fonts;
    UIContext ctx{DefaultAllocator()};
    ctx.SetFontService(&fonts.service);
    auto root = MakeRef<RootView>(DefaultAllocator());
    tests::Init(ctx, root.Get(), 400, 300);
    auto text = MakeRef<RichText>(DefaultAllocator(), StringView(u8"go [link=door]open the door[/link] now"));
    text->FontFamily.SetValue(String(u8"Roboto"));
    root->AddView(text.Get());
    tests::LayoutPass(ctx, root.Get());
    String heard;
    text->OnLink.Add([&heard](RichText*, StringView id) { heard = String(id); });

    REQUIRE_FALSE(text->Layout().links.IsEmpty());
    const Rectangle link = text->Layout().links[0].rect;
    const f32 x = text->Bounds.x + link.x + link.width * 0.5f;
    const f32 y = text->Bounds.y + link.y + link.height * 0.5f;
    CHECK(text->CursorAt(Float2{link.x + 1.0f, link.y + 1.0f}) == CursorType::Hand);
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, x, y, 0);
    ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, x, y);
    CHECK(heard == StringView(u8"door"));

    heard.Clear();
    const f32 before = text->Bounds.x + 2.0f; // on "go"
    ctx.GetInputManager()->ProcessMouseDown(MouseButton::Left, before, y, 0);
    ctx.GetInputManager()->ProcessMouseUp(MouseButton::Left, before, y);
    CHECK(heard.IsEmpty());
}

TEST_CASE("rich text: malformed markup is drawn as written, and runs set directly replace it")
{
    RobotoService fonts;
    auto text = MakeRef<RichText>(DefaultAllocator(), StringView(u8"a [blink]b"));
    CHECK(text->Document().PlainText() == StringView(u8"a [blink]b"));
    CHECK(text->Document().diagnostics.Size() == 1u);

    Array<TextRun> runs;
    TextRun run;
    run.text = String(u8"set");
    run.bold = true;
    runs.PushBack(run);
    text->SetRuns(Move(runs));
    REQUIRE(text->Document().runs.Size() == 1u);
    CHECK(text->Document().runs[0].bold);
    text->SetMarkup(u8"[i]again[/i]");
    REQUIRE(text->Document().runs.Size() == 1u);
    CHECK(text->Document().runs[0].italic);
}
