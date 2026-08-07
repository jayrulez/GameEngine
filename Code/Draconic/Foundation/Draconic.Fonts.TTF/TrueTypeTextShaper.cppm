// Draconic::FontsTTF - draconic.fonts.ttf:text_shaper partition
//
// Basic left-to-right text shaper + UI helpers (hit testing, cursor/selection
// geometry, word wrapping) over any IFont. Ported faithfully from
// Sedulous.Fonts.TTF/TrueTypeTextShaper.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.ttf:text_shaper;

import draconic.foundation;
import draconic.fonts;

using namespace draconic::foundation;

export namespace draconic::fonts
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
                            lastReflowedCodepoint = pos.codepoint;
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

        [[nodiscard]] HitTestResult HitTest(IFont&, Span<const GlyphPosition> positions, f32 x,
                                            f32 /*y*/) override
        {
            if (positions.Size() == 0)
                return HitTestResult(0, false, false);
            if (x < positions[0].x)
                return HitTestResult(0, false, false);

            for (usize i = 0; i < positions.Size(); ++i)
            {
                const GlyphPosition& pos = positions[i];
                const f32 charRight = pos.x + pos.advance;
                if (x >= pos.x && x < charRight)
                {
                    const f32 midpoint = pos.x + pos.advance * 0.5f;
                    return HitTestResult(static_cast<i32>(i), x >= midpoint, true);
                }
            }
            return HitTestResult(static_cast<i32>(positions.Size() - 1), true, false);
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
                return HitTestResult(static_cast<i32>(positions.Size() - 1), true, false,
                                     targetLine);

            for (i32 i = lineStart; i <= lineEnd; ++i)
            {
                const GlyphPosition& pos = positions[static_cast<usize>(i)];
                const f32 charRight = pos.x + pos.advance;
                if (x >= pos.x && x < charRight)
                {
                    const f32 midpoint = pos.x + pos.advance * 0.5f;
                    return HitTestResult(i, x >= midpoint, true, targetLine);
                }
            }

            if (x < positions[static_cast<usize>(lineStart)].x)
                return HitTestResult(lineStart, false, false, targetLine);
            return HitTestResult(lineEnd, true, false, targetLine);
        }

        [[nodiscard]] f32 GetCursorPosition(IFont&, Span<const GlyphPosition> positions,
                                            i32 characterIndex) override
        {
            if (positions.Size() == 0)
                return 0;
            if (characterIndex <= 0)
                return positions[0].x;
            if (characterIndex >= static_cast<i32>(positions.Size()))
            {
                const GlyphPosition& last = positions[positions.Size() - 1];
                return last.x + last.advance;
            }
            return positions[static_cast<usize>(characterIndex)].x;
        }

        void GetSelectionRects(IFont&, Span<const GlyphPosition> positions,
                               SelectionRange selection, f32 lineHeight,
                               Array<Rectangle>& outRects) override
        {
            outRects.Clear();
            if (positions.Size() == 0 || selection.IsEmpty())
                return;

            const i32 startIdx = Max(0, selection.start);
            const i32 endIdx = Min(static_cast<i32>(positions.Size()), selection.end);
            if (startIdx >= endIdx)
                return;

            const f32 rectHeight = lineHeight;
            f32 currentLineY = positions[static_cast<usize>(startIdx)].y;
            f32 rectStartX = positions[static_cast<usize>(startIdx)].x;
            f32 rectEndX = rectStartX;

            for (i32 i = startIdx; i < endIdx; ++i)
            {
                const GlyphPosition& pos = positions[static_cast<usize>(i)];
                if (Abs(pos.y - currentLineY) > lineHeight * 0.5f)
                {
                    if (rectEndX > rectStartX)
                        outRects.PushBack(
                            Rectangle(rectStartX, currentLineY, rectEndX - rectStartX, rectHeight));
                    currentLineY = pos.y;
                    rectStartX = pos.x;
                }
                rectEndX = pos.x + pos.advance;
            }

            if (rectEndX > rectStartX)
                outRects.PushBack(
                    Rectangle(rectStartX, currentLineY, rectEndX - rectStartX, rectHeight));
        }
    };
}
