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
    class TrueTypeTextShaper final : public ITextShaper
    {
    public:
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
                    GlyphPosition(index, codepoint, x, startY, glyphInfo.advanceWidth, glyphInfo));
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
                            lastReflowedCodepoint = pos.glyphId;
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

                outPositions.PushBack(GlyphPosition(index, codepoint, x + kern, y,
                                                    glyphInfo.advanceWidth, glyphInfo));
                x = x + kern + glyphInfo.advanceWidth;
                prevCodepoint = codepoint;
                ++index;
            }

            outTotalHeight = y + lineHeight;
            return Status();
        }

        // === UI support ===
        // All in characters through clusters (GlyphPosition::cluster), so they hold for any
        // shaper's N to M mapping; under this one every cluster is one character and one glyph.

        [[nodiscard]] HitTestResult HitTest(IFont&, Span<const GlyphPosition> positions, f32 x,
                                            f32 /*y*/) override
        {
            if (positions.Size() == 0)
                return HitTestResult(0, false, false);
            if (x < positions[0].x)
                return HitTestResult::OnCluster(positions[0], false, false);

            for (usize i = 0; i < positions.Size(); ++i)
            {
                const GlyphPosition& pos = positions[i];
                const f32 charRight = pos.x + pos.advance;
                if (x >= pos.x && x < charRight)
                {
                    const f32 midpoint = pos.x + pos.advance * 0.5f;
                    return HitTestResult::OnCluster(pos, x >= midpoint, true);
                }
            }
            return HitTestResult::OnCluster(positions[positions.Size() - 1], true, false);
        }

        [[nodiscard]] HitTestResult HitTestWrapped(IFont&, Span<const GlyphPosition> positions,
                                                   f32 x, f32 y, f32 lineHeight) override
        {
            if (positions.Size() == 0)
                return HitTestResult(0, false, false);

            i32 targetLine = static_cast<i32>(y / lineHeight);
            if (targetLine < 0)
                targetLine = 0;

            i32 lineStart = -1;
            i32 lineEnd = -1;
            f32 currentLineY = positions[0].y;
            i32 currentLine = 0;

            for (usize i = 0; i < positions.Size(); ++i)
            {
                const GlyphPosition& pos = positions[i];
                if (i > 0 && pos.y > currentLineY + lineHeight * 0.5f)
                {
                    ++currentLine;
                    currentLineY = pos.y;
                }
                if (currentLine == targetLine)
                {
                    if (lineStart < 0)
                        lineStart = static_cast<i32>(i);
                    lineEnd = static_cast<i32>(i);
                }
                else if (currentLine > targetLine)
                {
                    break;
                }
            }

            if (lineStart < 0)
                return HitTestResult::OnCluster(positions[positions.Size() - 1], true, false, targetLine);

            for (i32 i = lineStart; i <= lineEnd; ++i)
            {
                const GlyphPosition& pos = positions[static_cast<usize>(i)];
                const f32 charRight = pos.x + pos.advance;
                if (x >= pos.x && x < charRight)
                {
                    const f32 midpoint = pos.x + pos.advance * 0.5f;
                    return HitTestResult::OnCluster(pos, x >= midpoint, true, targetLine);
                }
            }

            if (x < positions[static_cast<usize>(lineStart)].x)
                return HitTestResult::OnCluster(positions[static_cast<usize>(lineStart)], false, false, targetLine);
            return HitTestResult::OnCluster(positions[static_cast<usize>(lineEnd)], true, false, targetLine);
        }

        // The caret's x before `characterIndex`: the start of its cluster's first glyph; inside a
        // cluster, the cluster's start; on a character with no glyph (a line break), the end of
        // the glyph before it; past the end, the end of the last glyph.
        [[nodiscard]] f32 GetCursorPosition(IFont&, Span<const GlyphPosition> positions,
                                            i32 characterIndex) override
        {
            if (positions.Size() == 0)
                return 0;
            for (usize i = 0; i < positions.Size(); ++i)
            {
                const GlyphPosition& pos = positions[i];
                if (pos.cluster < characterIndex)
                    continue;
                if (pos.cluster == characterIndex || i == 0)
                    return pos.x;
                const GlyphPosition& prev = positions[i - 1];
                return prev.ClusterEnd() <= characterIndex ? prev.x + prev.advance : ClusterStartX(positions, i - 1);
            }
            const GlyphPosition& last = positions[positions.Size() - 1];
            return characterIndex < last.ClusterEnd() ? ClusterStartX(positions, positions.Size() - 1)
                                                      : last.x + last.advance;
        }

        // The x of the first glyph of glyph `index`'s cluster (a mark shares its base's).
        [[nodiscard]] static f32 ClusterStartX(Span<const GlyphPosition> positions, usize index)
        {
            while (index > 0 && positions[index - 1].cluster == positions[index].cluster)
                --index;
            return positions[index].x;
        }

        // Rectangles over the glyphs whose clusters fall in the character range, one per line.
        void GetSelectionRects(IFont&, Span<const GlyphPosition> positions,
                               SelectionRange selection, f32 lineHeight,
                               Array<Rectangle>& outRects) override
        {
            outRects.Clear();
            if (positions.Size() == 0 || selection.IsEmpty())
                return;

            const i32 start = Min(selection.start, selection.end);
            const i32 end = Max(selection.start, selection.end);
            bool open = false;
            f32 currentLineY = 0;
            f32 rectStartX = 0;
            f32 rectEndX = 0;
            for (const GlyphPosition& pos : positions)
            {
                if (pos.cluster < start || pos.cluster >= end)
                    continue;
                if (open && Abs(pos.y - currentLineY) > lineHeight * 0.5f)
                {
                    if (rectEndX > rectStartX)
                        outRects.PushBack(Rectangle(rectStartX, currentLineY, rectEndX - rectStartX, lineHeight));
                    open = false;
                }
                if (!open)
                {
                    open = true;
                    currentLineY = pos.y;
                    rectStartX = pos.x;
                }
                rectEndX = pos.x + pos.advance;
            }
            if (open && rectEndX > rectStartX)
                outRects.PushBack(Rectangle(rectStartX, currentLineY, rectEndX - rectStartX, lineHeight));
        }
    };
}
