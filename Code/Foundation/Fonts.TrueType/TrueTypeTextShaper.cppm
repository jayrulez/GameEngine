// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:text_shaper partition
//
// Basic left-to-right text shaper + UI helpers (hit testing, cursor/selection
// geometry, word wrapping) over any IFont. Ported faithfully from
// Sedulous.Fonts.TTF/TrueTypeTextShaper.bf.

module;
#include "Core/Prelude.h"

export module foundation.fonts.truetype:text_shaper;

import foundation.core;
import foundation.fonts;

using namespace foundation::core;

export namespace foundation::fonts
{
    // The basic shaper: one glyph per character with the font's kerning (stb), over any IFont. Its UI
    // helpers are ClusterTextShaper's.
    class TrueTypeTextShaper final : public ClusterTextShaper
    {
    public:
        // Each font run shaped as ShapeText does; no script or language changes what it draws. A
        // right-to-left request lays the line out mirrored (no joining, no reordering of runs).
        [[nodiscard]] Result<f32> Shape(const ShapeRequest& request, Array<GlyphPosition>& outPositions) override
        {
            outPositions.Clear();
            Array<u32> codepoints;
            Array<usize> bytes;
            DecodeWithOffsets(request.text, codepoints, bytes);
            Array<FontRun> runs;
            ItemizeByFont(request.fonts, codepoints, bytes, runs);
            f32 x = request.startX;
            Array<GlyphPosition> run;
            for (const FontRun& r : runs)
            {
                IFont* font = request.fonts[r.font];
                if (font == nullptr)
                    continue;
                const Result<f32> advance =
                    ShapeText(*font, request.text.SubStr(r.byteStart, r.byteEnd - r.byteStart), x, request.startY, run);
                if (!advance.HasValue())
                    return advance;
                for (GlyphPosition& g : run)
                {
                    g.cluster += static_cast<i32>(r.start);
                    g.font = r.font;
                    outPositions.PushBack(g);
                }
                x += advance.Value();
            }
            if (request.direction == TextDirection::RightToLeft)
                MirrorLine(outPositions, 0, request.startX, x - request.startX);
            return x - request.startX;
        }

        // One glyph per character: nothing beyond what the codepoints map to.
        void CollectReachableGlyphs(const IFont&, Span<const i32>, Array<i32>& outGlyphs) override
        {
            outGlyphs.Clear();
        }

        [[nodiscard]] Result<f32> ShapeText(IFont& font, StringView text,
                                            Array<GlyphPosition>& outPositions) override
        {
            return ShapeText(font, text, 0, 0, outPositions);
        }

        [[nodiscard]] Result<f32> ShapeText(IFont& font, StringView text, f32 startX, f32 startY,
                                            Array<GlyphPosition>& outPositions) override
        {
            outPositions.Clear();
            f32 x = startX;
            i32 prevCodepoint = 0;
            i32 index = 0;
            usize i = 0;
            while (i < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, i));
                const GlyphInfo glyphInfo = font.GetGlyphInfo(codepoint);
                if (prevCodepoint != 0)
                    x += font.GetKerning(prevCodepoint, codepoint);
                outPositions.PushBack(
                    GlyphPosition(index, glyphInfo.glyphIndex, x, startY, glyphInfo.advanceWidth, glyphInfo));
                x += glyphInfo.advanceWidth;
                prevCodepoint = codepoint;
                ++index;
            }
            return x - startX;
        }

        [[nodiscard]] Status ShapeTextWrapped(IFont& font, StringView text, f32 maxWidth,
                                              Array<GlyphPosition>& outPositions,
                                              f32& outTotalHeight) override
        {
            outPositions.Clear();
            outTotalHeight = 0;

            const f32 lineHeight = font.Metrics().lineHeight;
            f32 x = 0;
            f32 y = 0;
            i32 prevCodepoint = 0;
            i32 index = 0;
            i32 lineStartIdx = 0;
            i32 lastSpaceIdx = -1;

            usize it = 0;
            while (it < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, it));

                if (codepoint == static_cast<i32>('\n'))
                {
                    y += lineHeight;
                    x = 0;
                    prevCodepoint = 0;
                    lineStartIdx = index + 1;
                    lastSpaceIdx = -1;
                    ++index;
                    continue;
                }
                if (codepoint == static_cast<i32>('\r'))
                {
                    ++index;
                    continue;
                }

                const GlyphInfo glyphInfo = font.GetGlyphInfo(codepoint);

                f32 kern = 0;
                if (prevCodepoint != 0)
                    kern = font.GetKerning(prevCodepoint, codepoint);

                const f32 newX = x + kern + glyphInfo.advanceWidth;

                if (codepoint == static_cast<i32>(' '))
                    lastSpaceIdx = static_cast<i32>(outPositions.Size());

                if (newX > maxWidth && x > 0)
                {
                    if (lastSpaceIdx >= lineStartIdx && lastSpaceIdx >= 0)
                    {
                        // Wrap at the last space - reflow glyphs after it.
                        y += lineHeight;
                        f32 reflowX = 0;
                        i32 lastReflowedCodepoint = 0;
                        for (i32 j = lastSpaceIdx + 1; j < static_cast<i32>(outPositions.Size());
                             ++j)
                        {
                            GlyphPosition& pos = outPositions[static_cast<usize>(j)];
                            pos.x = reflowX;
                            pos.y = y;
                            reflowX += pos.advance;
                            lastReflowedCodepoint = pos.glyphInfo.codepoint;
                        }
                        kern = (lastReflowedCodepoint != 0)
                                   ? font.GetKerning(lastReflowedCodepoint, codepoint)
                                   : 0;
                        x = reflowX;
                        lineStartIdx = lastSpaceIdx + 1;
                    }
                    else
                    {
                        // No break opportunity - hard wrap.
                        y += lineHeight;
                        x = 0;
                        kern = 0;
                        lineStartIdx = index;
                    }
                    lastSpaceIdx = -1;
                }

                outPositions.PushBack(GlyphPosition(index, glyphInfo.glyphIndex, x + kern, y,
                                                    glyphInfo.advanceWidth, glyphInfo));
                x = x + kern + glyphInfo.advanceWidth;
                prevCodepoint = codepoint;
                ++index;
            }

            outTotalHeight = y + lineHeight;
            return Status();
        }

    };
}
