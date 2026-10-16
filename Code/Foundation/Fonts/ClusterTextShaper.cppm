// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - :cluster_text_shaper partition
//
// ClusterTextShaper: the shaper UI helpers (hit testing, caret positions, selection rectangles)
// over GlyphPosition clusters, shared by every shaper backend. Clusters count characters, so they
// hold for any N to M mapping a shaper makes; a backend implements only the shaping. Moved out of
// the stb shaper (Fonts.TrueType) when the HarfBuzz backend joined it (F3 of vg-and-gui-lab.md).

module;
#include "Core/Prelude.h"

export module foundation.fonts:cluster_text_shaper;

import foundation.core;
import :types;
import :interfaces;
import :text_util; // DecodeCodepoint
import :bidi;

using namespace foundation::core;

export namespace foundation::fonts
{
    class ClusterTextShaper : public ITextShaper
    {
    public:
        /// One line: the text's paragraph levels from the installed bidi resolver (none: one run in
        /// the request's direction), its runs placed in visual order, each shaped by ShapeRun.
        [[nodiscard]] Result<f32> Shape(const ShapeRequest& request, Array<GlyphPosition>& outPositions) override
        {
            outPositions.Clear();
            Array<u32> codepoints;
            Array<usize> bytes;
            DecodeWithOffsets(request.text, codepoints, bytes);
            BidiLevels levels;
            const bool resolved = ResolveLevels(request.direction, codepoints, 0, codepoints.Size(), levels);
            return ShapeLine(request, codepoints, bytes, 0, codepoints.Size(), resolved ? &levels : nullptr,
                             request.startX, request.startY, outPositions);
        }

    protected:
        /// How a run reads: from its script (Auto), or as bidi resolved it.
        enum class RunDirection : u8
        {
            Auto,
            LeftToRight,
            RightToLeft,
        };

        /// Shape characters [start, end) of the request's text as one run, appending its glyphs
        /// from (x, y) in drawing order (a right-to-left run's last character first), each glyph
        /// marked rightToLeft when it reads so. Returns the run's advance.
        virtual Result<f32> ShapeRun(const ShapeRequest& request, const Array<u32>& codepoints,
                                     const Array<usize>& byteOffsets, usize start, usize end, RunDirection direction,
                                     f32 x, f32 y, Array<GlyphPosition>& out) = 0;

        /// The levels of characters [start, end) (one paragraph) from the installed resolver, the
        /// levels indexed from `start`; false with none installed.
        static bool ResolveLevels(TextDirection base, const Array<u32>& codepoints, usize start, usize end,
                                  BidiLevels& out)
        {
            IBidiResolver* resolver = BidiResolverFactory::Resolver();
            if (resolver == nullptr || end <= start)
                return false;
            resolver->Resolve(Span<const u32>(codepoints.Data() + start, end - start), base, out);
            return out.levels.Size() == end - start;
        }

        /// Characters [start, end) laid out on one line from (x, y): with levels (indexed from
        /// `levelBase`), in the visual order of their runs; without, as one run in the request's
        /// direction.
        Result<f32> ShapeLine(const ShapeRequest& request, const Array<u32>& codepoints, const Array<usize>& bytes,
                              usize start, usize end, const BidiLevels* levels, f32 x, f32 y,
                              Array<GlyphPosition>& out, usize levelBase = 0)
        {
            if (levels == nullptr)
            {
                const RunDirection direction = request.direction == TextDirection::LeftToRight   ? RunDirection::LeftToRight
                                               : request.direction == TextDirection::RightToLeft ? RunDirection::RightToLeft
                                                                                                 : RunDirection::Auto;
                return ShapeRun(request, codepoints, bytes, start, end, direction, x, y, out);
            }
            Array<BidiRun> runs;
            ReorderLine(*levels,
                        Span<const u32>(codepoints.Data() + levelBase, codepoints.Size() - levelBase),
                        start - levelBase, end - levelBase, runs);
            const f32 startX = x;
            for (const BidiRun& run : runs)
            {
                const Result<f32> advance = ShapeRun(request, codepoints, bytes, levelBase + run.start, levelBase + run.end,
                                                     run.RightToLeft() ? RunDirection::RightToLeft
                                                                       : RunDirection::LeftToRight,
                                                     x, y, out);
                if (!advance.HasValue())
                    return advance;
                x += advance.Value();
            }
            return x - startX;
        }

        /// Characters [start, end) shaped in one font of the request's list, and their UTF-8 bytes.
        struct FontRun
        {
            usize start = 0, end = 0;
            usize byteStart = 0, byteEnd = 0;
            u8 font = 0;
        };

        /// The text's codepoints, and each one's byte offset (with one more: the text's size).
        static void DecodeWithOffsets(StringView text, Array<u32>& codepoints, Array<usize>& byteOffsets)
        {
            codepoints.Clear();
            byteOffsets.Clear();
            usize i = 0;
            while (i < text.Size())
            {
                byteOffsets.PushBack(i);
                codepoints.PushBack(DecodeCodepoint(text, i));
            }
            byteOffsets.PushBack(text.Size());
        }

        /// Runs by font: a character takes the first font of the list that has its glyph (the list
        /// is in order of preference); a neutral one (a space, an ASCII digit or punctuation) stays
        /// in the current run's font when that has it, so runs do not split at spaces; one no font
        /// has stays in the current run.
        static void ItemizeByFont(Span<IFont* const> fonts, const Array<u32>& codepoints,
                                  const Array<usize>& byteOffsets, Array<FontRun>& runs)
        {
            runs.Clear();
            if (codepoints.IsEmpty() || fonts.IsEmpty())
                return;
            const auto has = [&](usize font, u32 cp)
            { return fonts[font] != nullptr && fonts[font]->HasGlyph(static_cast<i32>(cp)); };
            const auto neutral = [](u32 cp)
            { return cp == U' ' || cp == U'\t' || cp == 0xA0 || (cp >= 0x21 && cp <= 0x40) || (cp >= 0x5B && cp <= 0x60) ||
                     (cp >= 0x7B && cp <= 0x7E); };
            FontRun run;
            for (usize i = 0; i < codepoints.Size(); ++i)
            {
                usize font = run.font;
                if (i == 0 || !neutral(codepoints[i]) || !has(font, codepoints[i]))
                {
                    for (usize f = 0; f < fonts.Size() && f < 256; ++f)
                    {
                        if (has(f, codepoints[i]))
                        {
                            font = f;
                            break;
                        }
                    }
                }
                if (i == 0)
                {
                    run.font = static_cast<u8>(font);
                }
                else if (font != run.font)
                {
                    run.end = i;
                    run.byteEnd = byteOffsets[i];
                    runs.PushBack(run);
                    run = FontRun{};
                    run.start = i;
                    run.byteStart = byteOffsets[i];
                    run.font = static_cast<u8>(font);
                }
            }
            run.end = codepoints.Size();
            run.byteEnd = byteOffsets[codepoints.Size()];
            runs.PushBack(run);
        }

        /// Lay glyphs laid out left to right from `startX` out right to left over the same width,
        /// in drawing order (a right-to-left request to a shaper that only goes left to right).
        static void MirrorLine(Array<GlyphPosition>& glyphs, usize first, f32 startX, f32 width)
        {
            for (usize i = first; i < glyphs.Size(); ++i)
                glyphs[i].x = startX + width - (glyphs[i].x - startX) - glyphs[i].advance;
            for (usize a = first, b = glyphs.Size(); a + 1 < b; ++a, --b)
                Swap(glyphs[a], glyphs[b - 1]);
        }

    public:
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
                    return HitTestResult::OnCluster(pos, TrailingHalf(pos, x), true);
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
                    return HitTestResult::OnCluster(pos, TrailingHalf(pos, x), true, targetLine);
                }
            }

            if (x < positions[static_cast<usize>(lineStart)].x)
                return HitTestResult::OnCluster(positions[static_cast<usize>(lineStart)], false, false, targetLine);
            return HitTestResult::OnCluster(positions[static_cast<usize>(lineEnd)], true, false, targetLine);
        }

        // The caret's x before `characterIndex`, whatever order the glyphs are drawn in: at the
        // leading edge of its cluster (left, or right in a right-to-left run); inside a cluster, at
        // the cluster's; on a character with no glyph (a line break) or past the end, at the
        // trailing edge of the glyph logically before it.
        [[nodiscard]] f32 GetCursorPosition(IFont&, Span<const GlyphPosition> positions,
                                            i32 characterIndex) override
        {
            if (positions.Size() == 0)
                return 0;
            const GlyphPosition* exact = nullptr;
            const GlyphPosition* inside = nullptr;
            const GlyphPosition* before = nullptr;
            const GlyphPosition* first = &positions[0];
            for (const GlyphPosition& pos : positions)
            {
                if (pos.cluster == characterIndex && exact == nullptr)
                    exact = &pos;
                else if (pos.cluster < characterIndex && characterIndex < pos.ClusterEnd() && inside == nullptr)
                    inside = &pos;
                if (pos.ClusterEnd() <= characterIndex && (before == nullptr || pos.ClusterEnd() > before->ClusterEnd()))
                    before = &pos;
                if (pos.cluster < first->cluster)
                    first = &pos;
            }
            if (exact != nullptr)
                return LeadingEdge(*exact);
            if (inside != nullptr)
                return LeadingEdge(*inside);
            if (before != nullptr)
                return TrailingEdge(*before);
            return LeadingEdge(*first);
        }

        // Where a glyph's cluster starts and ends on screen: left and right, or right and left in
        // a right-to-left run.
        [[nodiscard]] static f32 LeadingEdge(const GlyphPosition& g) { return g.rightToLeft ? g.x + g.advance : g.x; }
        [[nodiscard]] static f32 TrailingEdge(const GlyphPosition& g) { return g.rightToLeft ? g.x : g.x + g.advance; }
        // Whether `x` falls in the half of the glyph its cluster ends in.
        [[nodiscard]] static bool TrailingHalf(const GlyphPosition& g, f32 x)
        {
            const f32 midpoint = g.x + g.advance * 0.5f;
            return g.rightToLeft ? x < midpoint : x >= midpoint;
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

    /// The process-wide choice of text shaper: a backend registers how to make one, the one
    /// registered last wins (a fuller shaper over the basic one), and font services ask here for
    /// the shaper each font gets. Null with none registered: the service uses its own default.
    class TextShaperFactory
    {
    public:
        using Creator = ITextShaper* (*)(IAllocator& allocator);

        static void Register(Creator creator)
        {
            if (creator == nullptr)
                return;
            Unregister(creator);
            Store().PushBack(creator);
        }
        static void Unregister(Creator creator)
        {
            Array<Creator>& store = Store();
            for (usize i = 0; i < store.Size(); ++i)
                if (store[i] == creator)
                {
                    store.RemoveAt(i);
                    return;
                }
        }
        /// A shaper from the backend registered last, allocated from `allocator`; null when none is.
        [[nodiscard]] static ITextShaper* Create(IAllocator& allocator)
        {
            const Array<Creator>& store = Store();
            return store.IsEmpty() ? nullptr : store.Back()(allocator);
        }
        [[nodiscard]] static usize Count() { return Store().Size(); }

    private:
        // Defined in FontsImpl.cpp: one registry per process, wherever a backend registers from
        // (a function-local static here would be duplicated per shared library).
        static Array<Creator>& Store();
    };

    /// The glyphs shaping `options`' codepoint range can reach beyond the range's own, asked of the
    /// registered shaper (none registered, or a one-to-one shaper: none). What bakers add, so a
    /// ligature or a joined form the shaper produces has its place in the atlas.
    [[nodiscard]] inline Array<i32> ReachableGlyphs(const IFont& font, const FontLoadOptions& options,
                                                    IAllocator& allocator)
    {
        Array<i32> glyphs;
        ITextShaper* shaper = TextShaperFactory::Create(allocator);
        if (shaper == nullptr)
            return glyphs;
        Array<i32> codepoints;
        for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            codepoints.PushBack(cp);
        shaper->CollectReachableGlyphs(font, Span<const i32>(codepoints.Data(), codepoints.Size()), glyphs);
        allocator.Delete(shaper);
        return glyphs;
    }
}
