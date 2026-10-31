// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TruncateToWidth (the `text-overflow: ellipsis` primitive): the result never measures wider
// than the box (within the documented 1px snug tolerance), the prefix is codepoint-aligned,
// and the two carve-outs hold (fits -> unchanged; narrower than the ellipsis -> unchanged).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.fonts;

using namespace foundation::core;
using namespace foundation::fonts;

namespace
{
    /// Every codepoint advances 20 (kerning-free), so widths are exact multiples.
    class MonoFont final : public IFont
    {
    public:
        [[nodiscard]] StringView FamilyName() const override { return u8"Mono"; }
        [[nodiscard]] Span<const u8> SourceData() const override { return {}; }
        [[nodiscard]] f32 PixelHeight() const override { return 32.0f; }
        [[nodiscard]] FontMetrics Metrics() const override
        {
            return FontMetrics(24.0f, -8.0f, 4.0f, 32.0f, 1.0f);
        }
        [[nodiscard]] GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            GlyphInfo info;
            info.codepoint = codepoint;
            info.advanceWidth = 20.0f;
            return info;
        }
        [[nodiscard]] f32 GetKerning(i32, i32) const override { return 0.0f; }
        [[nodiscard]] bool HasGlyph(i32) const override { return true; }
        [[nodiscard]] f32 MeasureString(StringView text) const override
        {
            usize codepoints = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if ((static_cast<u8>(text[i]) & 0xC0u) != 0x80u) // count lead bytes only
                {
                    ++codepoints;
                }
            }
            return static_cast<f32>(codepoints) * 20.0f;
        }
        [[nodiscard]] f32 MeasureString(StringView text, Array<GlyphPosition>& outPositions) const override
        {
            outPositions.Clear();
            return MeasureString(text);
        }
    };
}

TEST_CASE("fonts.truncate: the ellipsized text never exceeds the box")
{
    MonoFont font;
    const StringView text = u8"abcdefghij"; // 200 wide
    for (f32 maxWidth = 0.0f; maxWidth <= 220.0f; maxWidth += 7.0f)
    {
        const String out = TruncateToWidth(font, text, maxWidth);
        const f32 w = font.MeasureString(out.AsView());
        if (maxWidth >= 200.0f - 1.0f)
        {
            CHECK(out.AsView() == text); // fits: unchanged
        }
        else if (maxWidth < 60.0f)
        {
            CHECK(out.AsView() == StringView(u8"...")); // the ellipsis alone stands in
        }
        else
        {
            CHECK(w <= maxWidth + 1.0f);
            CHECK(out.Size() >= 3);
            CHECK(out.AsView().SubStr(out.Size() - 3, 3) == StringView(u8"..."));
        }
    }
}

TEST_CASE("fonts.truncate: the prefix is codepoint-aligned and short text stays whole")
{
    MonoFont font;
    // Two-byte codepoints: a byte-level cut would split one; the cut lands between them.
    const StringView text = u8"éééééé"; // 6 codepoints = 120 wide
    const String out = TruncateToWidth(font, text, 100.0f); // 100 - 60 = 40 -> two codepoints + ...
    CHECK(out.AsView() == StringView(u8"éé..."));
    CHECK(font.MeasureString(out.AsView()) == doctest::Approx(100.0f));
    // Narrower than the ellipsis itself: replacing "ab" with "..." would widen it, so unchanged.
    CHECK(TruncateToWidth(font, u8"ab", 10.0f).AsView() == StringView(u8"ab"));
    CHECK(TruncateToWidth(font, u8"", 10.0f).AsView() == StringView(u8""));
}
