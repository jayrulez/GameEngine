// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The HarfBuzz shaper (F3 of Specs/vg-and-gui-lab.md) through ITextShaper: an fi ligature is one
// glyph over a shared cluster of two characters and hit tests to the right offsets; Arabic joins
// into fewer glyphs than characters; wrapping keeps a ligature whole; a font with no file falls
// back to the basic shaper; and installing it changes what a font service hands out.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.harfbuzz;

using namespace foundation::core;
using namespace foundation::fonts;

namespace
{
    String FontPath(const char* rel)
    {
        String p = foundation::vfs::DataPath(foundation::vfs::FindDataRoot(), u8"Assets/fonts");
        for (const char* s = rel; *s != '\0'; ++s)
            p.PushBack(static_cast<utf8char>(static_cast<unsigned char>(*s)));
        return p;
    }

    IFont* Load(const char* rel, f32 pixelHeight = 32.0f)
    {
        TrueTypeFonts::Initialize();
        FontLoadOptions options = FontLoadOptions::Default();
        options.pixelHeight = pixelHeight;
        Result<IFont*, FontLoadResult> parsed =
            FontParserFactory::ParseFromFile(FontPath(rel), options, DefaultAllocator());
        return parsed.HasValue() ? parsed.Value() : nullptr;
    }

    Span<const GlyphPosition> AsSpan(const Array<GlyphPosition>& a)
    {
        return Span<const GlyphPosition>(a.Data(), a.Size());
    }

    struct Shaper
    {
        ITextShaper* shaper = HarfBuzzShaping::CreateShaper(DefaultAllocator());
        ~Shaper() { DefaultAllocator().Delete(shaper); }
    };
}

TEST_CASE("harfbuzz: fi is one glyph over two characters, and hits and carets go by its cluster")
{
    IFont* font = Load("/roboto/Roboto-Regular.ttf");
    REQUIRE(font != nullptr);
    Shaper s;
    Array<GlyphPosition> glyphs;
    REQUIRE(s.shaper->ShapeText(*font, u8"office", glyphs).HasValue());
    // o, f, the fi ligature, c, e: five glyphs for six characters.
    REQUIRE(glyphs.Size() == 5u);
    CHECK(glyphs[2].cluster == 2);
    CHECK(glyphs[2].clusterLength == 2);
    CHECK(glyphs[3].cluster == 4);
    CHECK(glyphs[2].glyphId != font->GetGlyphInfo('f').glyphIndex);
    CHECK(glyphs[2].glyphId != font->GetGlyphInfo('i').glyphIndex);
    // Glyph ids are the font's; the ones a codepoint maps to match the basic shaper's.
    CHECK(glyphs[0].glyphId == font->GetGlyphInfo('o').glyphIndex);
    CHECK(glyphs[3].x == doctest::Approx(glyphs[2].x + glyphs[2].advance));

    // The trailing half of the ligature puts the caret after both its characters.
    const GlyphPosition& fi = glyphs[2];
    CHECK(s.shaper->HitTest(*font, AsSpan(glyphs), fi.x + fi.advance * 0.9f, 0.0f).InsertionIndex() == 4);
    CHECK(s.shaper->HitTest(*font, AsSpan(glyphs), fi.x + fi.advance * 0.1f, 0.0f).InsertionIndex() == 2);
    CHECK(s.shaper->GetCursorPosition(*font, AsSpan(glyphs), 3) == doctest::Approx(fi.x)); // inside it

    DefaultAllocator().Delete(font);
}

TEST_CASE("harfbuzz: Arabic joins, so it has fewer glyphs than characters, and reads right to left")
{
    IFont* font = Load("/dejavu/DejaVuSansMono.ttf");
    REQUIRE(font != nullptr);
    Shaper s;
    Array<GlyphPosition> glyphs;
    // salaam: seen, lam, alef, meem; lam and alef join into one glyph.
    REQUIRE(s.shaper->ShapeText(*font, u8"سلام", glyphs).HasValue());
    CHECK(glyphs.Size() == 3u);
    // Right to left: the first glyph drawn (leftmost) is the last character's.
    REQUIRE_FALSE(glyphs.IsEmpty());
    CHECK(glyphs[0].cluster == 3);
    CHECK(glyphs.Back().cluster == 0);
    // The joined forms are not the isolated forms a codepoint maps to.
    CHECK(glyphs.Back().glyphId != font->GetGlyphInfo(0x0633).glyphIndex);
    for (const GlyphPosition& g : glyphs)
        CHECK(g.advance > 0.0f);

    DefaultAllocator().Delete(font);
}

TEST_CASE("harfbuzz: wrapping breaks after spaces, keeps the ligature whole, and starts lines at 0")
{
    IFont* font = Load("/roboto/Roboto-Regular.ttf", 20.0f);
    REQUIRE(font != nullptr);
    Shaper s;
    Array<GlyphPosition> one;
    REQUIRE(s.shaper->ShapeText(*font, u8"office office", one).HasValue());
    const f32 word = one[4].x + one[4].advance; // "office"
    Array<GlyphPosition> glyphs;
    f32 height = 0.0f;
    REQUIRE(s.shaper->ShapeTextWrapped(*font, u8"office office\noffice", word * 1.5f, glyphs, height).IsOk());
    // Three lines: the first word (and its space), the second, then the paragraph after the break.
    const f32 lineH = font->Metrics().lineHeight;
    CHECK(height == doctest::Approx(lineH * 3.0f));
    usize ligatures = 0;
    for (const GlyphPosition& g : glyphs)
    {
        if (g.clusterLength == 2)
            ++ligatures;
        CHECK(g.x + g.advance <= word * 1.5f + 0.5f);
    }
    CHECK(ligatures == 3u);
    CHECK(glyphs[0].x == doctest::Approx(0.0f));
    // The second "office" starts its own line at x 0, its clusters still counting from the text.
    bool sawSecond = false;
    for (const GlyphPosition& g : glyphs)
    {
        if (g.cluster == 7)
        {
            sawSecond = true;
            CHECK(g.x == doctest::Approx(0.0f));
            CHECK(g.y == doctest::Approx(lineH));
        }
        if (g.cluster == 14) // after the break
            CHECK(g.y == doctest::Approx(lineH * 2.0f));
    }
    CHECK(sawSecond);

    DefaultAllocator().Delete(font);
}

TEST_CASE("harfbuzz: a font with no file is shaped by the basic shaper")
{
    // A hand-built font: no source data, one glyph per character, glyph id from its table.
    class TableFont final : public IFont
    {
    public:
        [[nodiscard]] StringView FamilyName() const override { return u8"Table"; }
        [[nodiscard]] Span<const u8> SourceData() const override { return {}; }
        [[nodiscard]] FontMetrics Metrics() const override { return FontMetrics(10.0f, -3.0f, 0.0f, 13.0f, 1.0f); }
        [[nodiscard]] f32 PixelHeight() const override { return 13.0f; }
        [[nodiscard]] GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            GlyphInfo info;
            info.codepoint = codepoint;
            info.glyphIndex = codepoint + 1000;
            info.advanceWidth = 7.0f;
            return info;
        }
        [[nodiscard]] f32 GetKerning(i32, i32) const override { return 0.0f; }
        [[nodiscard]] bool HasGlyph(i32) const override { return true; }
        [[nodiscard]] f32 MeasureString(StringView text) const override { return 7.0f * static_cast<f32>(text.Size()); }
        [[nodiscard]] f32 MeasureString(StringView text, Array<GlyphPosition>& out) const override
        {
            out.Clear();
            return MeasureString(text);
        }
    };
    TableFont font;
    Shaper s;
    Array<GlyphPosition> glyphs;
    REQUIRE(s.shaper->ShapeText(font, u8"fi", glyphs).HasValue());
    REQUIRE(glyphs.Size() == 2u); // no ligature without the font's tables
    CHECK(glyphs[0].glyphId == 'f' + 1000);
    CHECK(glyphs[1].x == doctest::Approx(7.0f));
}

TEST_CASE("harfbuzz: installed, it is the shaper a font service hands out")
{
    const String path = FontPath("/roboto/Roboto-Regular.ttf");
    HarfBuzzShaping::Install();
    CHECK(HarfBuzzShaping::IsInstalled());
    {
        TrueTypeFontService service(DefaultAllocator());
        REQUIRE(service.LoadFont(u8"Roboto", path.AsView()) == FontLoadResult::Success);
        CachedFont* font = service.GetFont(u8"Roboto", 24.0f);
        REQUIRE(font != nullptr);
        REQUIRE(font->shaper != nullptr);
        Array<GlyphPosition> glyphs;
        REQUIRE(font->shaper->ShapeText(*font->font, u8"fi", glyphs).HasValue());
        CHECK(glyphs.Size() == 1u); // the ligature: HarfBuzz shaped it
    }
    HarfBuzzShaping::Uninstall();
    CHECK_FALSE(HarfBuzzShaping::IsInstalled());
    {
        TrueTypeFontService service(DefaultAllocator());
        REQUIRE(service.LoadFont(u8"Roboto", path.AsView()) == FontLoadResult::Success);
        CachedFont* font = service.GetFont(u8"Roboto", 24.0f);
        REQUIRE(font != nullptr);
        Array<GlyphPosition> glyphs;
        REQUIRE(font->shaper->ShapeText(*font->font, u8"fi", glyphs).HasValue());
        CHECK(glyphs.Size() == 2u); // the basic shaper again
    }
}
