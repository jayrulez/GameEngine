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
import foundation.fonts.bidi;
import foundation.fonts.distancefield.baker;

using namespace foundation::core;
using namespace foundation::fonts;

namespace
{
    String FontPath(const char* rel)
    {
        String p = foundation::vfs::FindDataRoot(); // the data root; rel names the font from there
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
    IFont* font = Load("/Engine/Fonts/roboto/Roboto-Regular.ttf");
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
    IFont* font = Load("/Samples/Fonts/dejavu/DejaVuSansMono.ttf");
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
    IFont* font = Load("/Engine/Fonts/roboto/Roboto-Regular.ttf", 20.0f);
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
    const String path = FontPath("/Engine/Fonts/roboto/Roboto-Regular.ttf");
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

TEST_CASE("harfbuzz: installing again changes nothing, so a repeat never touches the registry")
{
    HarfBuzzShaping::Uninstall();
    const usize before = TextShaperFactory::Count();
    HarfBuzzShaping::Install();
    HarfBuzzShaping::Install();
    CHECK(TextShaperFactory::Count() == before + 1);
    HarfBuzzShaping::Uninstall();
    CHECK(TextShaperFactory::Count() == before);
}

TEST_CASE("harfbuzz: the glyphs substitutions reach are named, and bakers bake them")
{
    IFont* font = Load("/Engine/Fonts/roboto/Roboto-Regular.ttf");
    REQUIRE(font != nullptr);
    Shaper s;
    Array<GlyphPosition> shaped;
    REQUIRE(s.shaper->ShapeText(*font, u8"fi", shaped).HasValue());
    REQUIRE(shaped.Size() == 1u);
    const i32 ligature = shaped[0].glyphId;

    // Reachable from f and i: the ligature, which no codepoint maps to.
    const i32 range[] = {'f', 'i'};
    Array<i32> reachable;
    s.shaper->CollectReachableGlyphs(*font, Span<const i32>(range, 2), reachable);
    bool found = false;
    for (const i32 g : reachable)
    {
        found = found || g == ligature;
        CHECK(g != font->GetGlyphInfo('f').glyphIndex); // the codepoints' own glyphs are not repeated
    }
    CHECK(found);

    // With HarfBuzz installed, a coverage atlas and a distance-field atlas both hold it.
    HarfBuzzShaping::Install();
    FontLoadOptions options = FontLoadOptions::Default();
    options.pixelHeight = 32.0f;
    Result<IFontAtlas*, FontLoadResult> coverage = FontAtlasBakerFactory::Bake(*font, options, DefaultAllocator());
    REQUIRE(coverage.HasValue());
    CHECK(coverage.Value()->Contains(ligature));
    CHECK(coverage.Value()->GlyphFor('f') != ligature);
    DefaultAllocator().Delete(coverage.Value());

    DistanceFieldFonts::Initialize();
    FontLoadOptions df = FontLoadOptions::DistanceField();
    df.firstCodepoint = 'a';
    df.lastCodepoint = 'z';
    Result<IFontAtlas*, FontLoadResult> field = FontAtlasBakerFactory::Bake(*font, df, DefaultAllocator());
    REQUIRE(field.HasValue());
    CHECK(field.Value()->Contains(ligature));
    DefaultAllocator().Delete(field.Value());
    HarfBuzzShaping::Uninstall();

    // Without it, the basic shaper reaches nothing more, and the atlas holds only the range.
    Result<IFontAtlas*, FontLoadResult> plain = FontAtlasBakerFactory::Bake(*font, options, DefaultAllocator());
    REQUIRE(plain.HasValue());
    CHECK_FALSE(plain.Value()->Contains(ligature));
    DefaultAllocator().Delete(plain.Value());
    DefaultAllocator().Delete(font);
}

TEST_CASE("harfbuzz: through a font service, every glyph of shaped text has its place in the atlas")
{
    HarfBuzzShaping::Install();
    {
        TrueTypeFontService service(DefaultAllocator());
        REQUIRE(service.LoadFont(u8"Roboto", FontPath("/Engine/Fonts/roboto/Roboto-Regular.ttf").AsView()) ==
                FontLoadResult::Success);
        CachedFont* font = service.GetFont(u8"Roboto", 24.0f);
        REQUIRE(font != nullptr);
        Array<GlyphPosition> glyphs;
        REQUIRE(font->shaper->ShapeText(*font->font, u8"office fluff", glyphs).HasValue());
        for (const GlyphPosition& g : glyphs)
        {
            f32 cursor = 0.0f;
            GlyphQuad quad;
            CHECK(font->atlas->GetGlyphQuad(g.glyphId, cursor, 0.0f, quad)); // drawable: the ligatures too
            CHECK(cursor > 0.0f);                                              // and it advances (a space too)
        }
    }
    HarfBuzzShaping::Uninstall();
}

// ============================================================================================
// The widened request: a font list with fallback, a direction, a script, a language.
// ============================================================================================

TEST_CASE("harfbuzz: a character no font before has takes the next font in the list")
{
    IFont* roboto = Load("/Engine/Fonts/roboto/Roboto-Regular.ttf");
    IFont* dejavu = Load("/Samples/Fonts/dejavu/DejaVuSansMono.ttf");
    REQUIRE(roboto != nullptr);
    REQUIRE(dejavu != nullptr);
    IFont* const fonts[2] = {roboto, dejavu};
    Shaper s;
    ShapeRequest request;
    request.text = u8"a \u0633\u0644\u0627\u0645 b"; // Roboto has no Arabic
    request.fonts = Span<IFont* const>(fonts, 2);
    Array<GlyphPosition> glyphs;
    REQUIRE(s.shaper->Shape(request, glyphs).HasValue());
    // a and the space after it in Roboto; the Arabic in DejaVu, with the space after it (a
    // character keeps the run's font when that font has it); b in Roboto again.
    usize arabic = 0;
    for (const GlyphPosition& g : glyphs)
    {
        const bool inDejaVu = g.cluster >= 2 && g.cluster <= 6;
        CHECK(g.font == (inDejaVu ? 1 : 0));
        arabic += (g.cluster >= 2 && g.cluster <= 5) ? 1u : 0u;
    }
    CHECK(arabic == 3u); // joined in DejaVu: three glyphs for four characters
    // Runs follow on, left to right; the Arabic run reads right to left inside itself.
    for (usize i = 1; i < glyphs.Size(); ++i)
        CHECK(glyphs[i].x >= glyphs[i - 1].x);
    CHECK(glyphs.Back().cluster == 7); // b, last

    DefaultAllocator().Delete(dejavu);
    DefaultAllocator().Delete(roboto);
}

TEST_CASE("harfbuzz: a right-to-left request reads right to left, and a language picks its forms")
{
    IFont* roboto = Load("/Engine/Fonts/roboto/Roboto-Regular.ttf");
    IFont* dejavu = Load("/Samples/Fonts/dejavu/DejaVuSansMono.ttf");
    REQUIRE(roboto != nullptr);
    REQUIRE(dejavu != nullptr);
    Shaper s;
    {
        IFont* const fonts[1] = {roboto};
        ShapeRequest request;
        request.text = u8"abc";
        request.fonts = Span<IFont* const>(fonts, 1);
        request.direction = TextDirection::RightToLeft;
        Array<GlyphPosition> glyphs;
        REQUIRE(s.shaper->Shape(request, glyphs).HasValue());
        REQUIRE(glyphs.Size() == 3u);
        CHECK(glyphs[0].cluster == 2); // c drawn first, leftmost
        CHECK(glyphs[2].cluster == 0);
    }
    {
        // DejaVu draws a Serbian be differently from the Russian one.
        IFont* const fonts[1] = {dejavu};
        ShapeRequest request;
        request.text = u8"\u0431";
        request.fonts = Span<IFont* const>(fonts, 1);
        Array<GlyphPosition> plain;
        REQUIRE(s.shaper->Shape(request, plain).HasValue());
        request.language = u8"sr";
        Array<GlyphPosition> serbian;
        REQUIRE(s.shaper->Shape(request, serbian).HasValue());
        REQUIRE(plain.Size() == 1u);
        REQUIRE(serbian.Size() == 1u);
        CHECK(serbian[0].glyphId != plain[0].glyphId);
        CHECK(plain[0].glyphId == dejavu->GetGlyphInfo(0x0431).glyphIndex);
        // A script given outright is used for the whole text.
        request.language = {};
        request.script = ScriptTag("Cyrl");
        Array<GlyphPosition> cyrillic;
        REQUIRE(s.shaper->Shape(request, cyrillic).HasValue());
        CHECK(cyrillic.Size() == 1u);
    }
    DefaultAllocator().Delete(dejavu);
    DefaultAllocator().Delete(roboto);
}

TEST_CASE("harfbuzz: the basic shaper takes the same request: fallback by font, and a mirrored line")
{
    IFont* roboto = Load("/Engine/Fonts/roboto/Roboto-Regular.ttf");
    IFont* dejavu = Load("/Samples/Fonts/dejavu/DejaVuSansMono.ttf");
    REQUIRE(roboto != nullptr);
    REQUIRE(dejavu != nullptr);
    IFont* const fonts[2] = {roboto, dejavu};
    TrueTypeTextShaper basic;
    ShapeRequest request;
    request.text = u8"a\u0633b";
    request.fonts = Span<IFont* const>(fonts, 2);
    Array<GlyphPosition> glyphs;
    REQUIRE(basic.Shape(request, glyphs).HasValue());
    REQUIRE(glyphs.Size() == 3u);
    CHECK(glyphs[0].font == 0);
    CHECK(glyphs[1].font == 1);
    CHECK(glyphs[1].cluster == 1);
    CHECK(glyphs[2].font == 0);
    request.direction = TextDirection::RightToLeft;
    Array<GlyphPosition> mirrored;
    REQUIRE(basic.Shape(request, mirrored).HasValue());
    REQUIRE(mirrored.Size() == 3u);
    CHECK(mirrored[0].cluster == 2); // b leftmost
    CHECK(mirrored[0].x == doctest::Approx(0.0f));
    CHECK(mirrored[2].x + mirrored[2].advance == doctest::Approx(glyphs[2].x + glyphs[2].advance));
    DefaultAllocator().Delete(dejavu);
    DefaultAllocator().Delete(roboto);
}

// ============================================================================================
// Bidi: a right-to-left paragraph with Latin inside lays out, hit tests and wraps in visual order.
// ============================================================================================

TEST_CASE("bidi: the resolver gives Arabic odd levels and finds a paragraph's direction")
{
    IBidiResolver& resolver = BidiAlgorithm::Resolver();
    const u32 ltr[7] = {'a', 'b', ' ', 0x0633, 0x0644, ' ', 'c'};
    BidiLevels levels;
    resolver.Resolve(Span<const u32>(ltr, 7), TextDirection::Auto, levels);
    REQUIRE(levels.levels.Size() == 7u);
    CHECK(levels.paragraphLevel == 0);
    CHECK(levels.levels[0] == 0);
    CHECK(levels.levels[3] == 1);
    CHECK(levels.levels[4] == 1);
    CHECK(levels.levels[6] == 0);
    const u32 rtl[5] = {0x0633, ' ', 'a', 'b', 0x0644};
    resolver.Resolve(Span<const u32>(rtl, 5), TextDirection::Auto, levels);
    CHECK(levels.paragraphLevel == 1); // its first strong character is Arabic
    CHECK(levels.levels[2] == 2);       // the Latin inside, one level up
}

TEST_CASE("bidi: a right-to-left paragraph with Latin inside reads right to left and hit tests")
{
    IFont* font = Load("/Samples/Fonts/dejavu/DejaVuSansMono.ttf");
    REQUIRE(font != nullptr);
    BidiAlgorithm::Install();
    Shaper s;
    Array<GlyphPosition> glyphs;
    // salaam, a space, then "world": an Arabic paragraph.
    REQUIRE(s.shaper->ShapeText(*font, u8"\u0633\u0644\u0627\u0645 world", glyphs).HasValue());
    f32 arabicLeft = 1e9f, latinRight = -1e9f, right = 0.0f;
    const GlyphPosition* seen = nullptr;
    for (const GlyphPosition& g : glyphs)
    {
        if (g.cluster <= 3)
            arabicLeft = Min(arabicLeft, g.x);
        if (g.cluster >= 5)
            latinRight = Max(latinRight, g.x + g.advance);
        if (g.cluster == 0)
            seen = &g;
        right = Max(right, g.x + g.advance);
        if (g.cluster <= 3)
            CHECK(g.rightToLeft);
    }
    CHECK(latinRight <= arabicLeft + 0.01f); // the Latin on the left, the Arabic on the right
    REQUIRE(seen != nullptr);
    CHECK(seen->x + seen->advance == doctest::Approx(right)); // the first character rightmost
    // "world" reads left to right inside: w before d.
    f32 wx = 0.0f, dx = 0.0f;
    for (const GlyphPosition& g : glyphs)
    {
        if (g.cluster == 5)
            wx = g.x;
        if (g.cluster == 9)
            dx = g.x;
    }
    CHECK(wx < dx);
    // The caret before the paragraph is at its right edge; a hit on the right half of its first
    // character puts the caret before it, on the left half after it.
    CHECK(s.shaper->GetCursorPosition(*font, AsSpan(glyphs), 0) == doctest::Approx(right));
    CHECK(s.shaper->HitTest(*font, AsSpan(glyphs), seen->x + seen->advance * 0.9f, 0.0f).InsertionIndex() == 0);
    CHECK(s.shaper->HitTest(*font, AsSpan(glyphs), seen->x + seen->advance * 0.1f, 0.0f).InsertionIndex() == 1);
    BidiAlgorithm::Uninstall();
    DefaultAllocator().Delete(font);
}

TEST_CASE("bidi: a wrapped right-to-left paragraph reorders each line, its first words on the right of the first")
{
    IFont* font = Load("/Samples/Fonts/dejavu/DejaVuSansMono.ttf", 20.0f);
    REQUIRE(font != nullptr);
    BidiAlgorithm::Install();
    Shaper s;
    // Three Arabic words; a width that holds two.
    const char8_t* text = u8"\u0633\u0644\u0627\u0645 \u0633\u0644\u0627\u0645 \u0633\u0644\u0627\u0645";
    Array<GlyphPosition> one;
    REQUIRE(s.shaper->ShapeText(*font, u8"\u0633\u0644\u0627\u0645 ", one).HasValue());
    f32 word = 0.0f;
    for (const GlyphPosition& g : one)
        word += g.advance;
    Array<GlyphPosition> glyphs;
    f32 height = 0.0f;
    REQUIRE(s.shaper->ShapeTextWrapped(*font, text, word * 2.2f, glyphs, height).IsOk());
    const f32 lineH = font->Metrics().lineHeight;
    CHECK(height == doctest::Approx(lineH * 2.0f));
    // The first word (clusters 0..4) is on the first line, to the right of the second word.
    f32 firstLeft = 1e9f, secondRight = -1e9f;
    for (const GlyphPosition& g : glyphs)
    {
        if (g.cluster <= 3)
        {
            CHECK(g.y == doctest::Approx(0.0f));
            firstLeft = Min(firstLeft, g.x);
        }
        if (g.cluster >= 5 && g.cluster <= 8)
        {
            CHECK(g.y == doctest::Approx(0.0f));
            secondRight = Max(secondRight, g.x + g.advance);
        }
        if (g.cluster >= 10)
            CHECK(g.y == doctest::Approx(lineH));
    }
    CHECK(secondRight <= firstLeft + 0.01f);
    BidiAlgorithm::Uninstall();
    DefaultAllocator().Delete(font);
}
