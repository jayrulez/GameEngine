// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_layout partition
//
// The inline formatter: lays a TextDocument's runs into lines. Text breaks after spaces and at line
// breaks, runs of different sizes share a baseline (each line as tall as its tallest run, or as the
// line height says), an icon sits on the baseline as tall as its run's text is big, and each line
// aligns on its own. What comes out is positioned glyphs per run, underlines, icon rectangles and
// link rectangles, in the layout's own coordinates; the widget draws them.
//
// A run that sets no font of its own draws in the style's font (the label's). One that sets a family,
// a size, bold or italic asks the font service; bold and italic take the family's "<family> Bold",
// "<family> Italic" or "<family> Bold Italic" face when one is loaded, and are synthesized when not
// (bold drawn twice a little apart, italic slanted), as browsers do. With no service, such a run
// draws in the style's font, its bold and italic synthesized.
//
// Copied from foundation.ui's :text_layout (E4 of Specs/gui-lab-evolution.md), and the lab's own
// from then on. The lab adds what CSS's text properties need: a line height, letter spacing, a
// clamp on the line count and an ellipsis at a line's end, start or middle (text-overflow), and what
// editing needs: every glyph and line knows the bytes of the plain text it shows, so a caret finds
// its x and a point its offset. A font with no shaper still measures; its pieces come out as text
// to draw unshaped, each character placed by measuring (they take no ellipsis).

module;
#include "Core/Prelude.h"

export module experimental.gui:text_layout;

import foundation.core;  // Array, String, Rectangle, Float2, Max, Min, Abs
import foundation.fonts; // CachedFont, IFontService, GlyphPosition
import :text_runs;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    enum class TextHAlign
    {
        Left,
        Center,
        Right
    };
    enum class TextVAlign
    {
        Top,
        Middle,
        Bottom
    };
    /// CSS's white-space: whether spaces and line breaks collapse, and whether lines wrap.
    enum class TextWhiteSpace : u8
    {
        Normal,  ///< collapse; wrap
        NoWrap,  ///< collapse; one line unless the text breaks it
        Pre,     ///< keep; break only where the text does
        PreWrap, ///< keep; wrap
    };
    /// CSS's text-overflow, with the lab's start and middle ellipses (for paths).
    enum class TextOverflow : u8
    {
        Clip,
        Ellipsis,      ///< "Long text that…"
        EllipsisStart, ///< "…the end of it"
        EllipsisMiddle ///< "/home/…/file.txt"
    };

    [[nodiscard]] constexpr bool CollapsesSpaces(TextWhiteSpace w) noexcept
    {
        return w == TextWhiteSpace::Normal || w == TextWhiteSpace::NoWrap;
    }
    [[nodiscard]] constexpr bool Wraps(TextWhiteSpace w) noexcept
    {
        return w == TextWhiteSpace::Normal || w == TextWhiteSpace::PreWrap;
    }

    /// Whether `font` draws every character of `text`: its atlas holds a glyph for each (a font with
    /// no atlas is taken at its word). Atlases are baked over a range of codepoints, so a widget's
    /// own marks (an ellipsis, a password's bullet) fall back to ones it has.
    [[nodiscard]] inline bool FontDraws(const fonts::CachedFont* font, core::StringView text)
    {
        if (font == nullptr || font->font == nullptr)
            return false;
        if (font->atlas == nullptr)
            return true;
        for (usize i = 0; i < text.Size();)
        {
            const i32 glyph = font->atlas->GlyphFor(static_cast<i32>(core::DecodeUtf8(text, i)));
            if (glyph <= 0 || !font->atlas->Contains(glyph))
                return false;
        }
        return true;
    }
    /// The ellipsis `font` draws: U+2026, or three full stops.
    [[nodiscard]] inline core::StringView EllipsisFor(const fonts::CachedFont* font)
    {
        return FontDraws(font, core::StringView(u8"\u2026")) ? core::StringView(u8"\u2026") : core::StringView(u8"...");
    }

    /// What a run takes when its markup leaves a style unset, and how the lines go.
    struct TextLayoutStyle
    {
        fonts::CachedFont* Font = nullptr;    ///< the label's font: plain runs draw in it
        fonts::IFontService* Fonts = nullptr; ///< resolves a run's own family, size, bold and italic
        core::String Family;                  ///< the label's family as the service names it
        f32 Size = 16.0f;                     ///< the label's size in pixels
        Color TextColor = Color::White;
        TextHAlign Align = TextHAlign::Left;
        bool Wrap = true;
        f32 MaxWidth = 0.0f;      ///< The width lines wrap at, align in and end in an ellipsis past; 0: unbounded.
        f32 LineHeight = 0.0f;    ///< Each line's height in pixels; 0: its fonts' own.
        f32 LetterSpacing = 0.0f; ///< Added after every character, in pixels.
        u32 MaxLines = 0;         ///< The most lines kept (line-clamp); 0: all.
        TextOverflow Overflow = TextOverflow::Clip;
    };

    /// A run's glyphs on one line: positions in layout coordinates, y on the baseline.
    struct LaidOutGlyphs
    {
        fonts::CachedFont* Font = nullptr;
        Array<fonts::GlyphPosition> Glyphs;
        /// Each glyph's bytes of the plain text, parallel to Glyphs (an ellipsis has none of its own:
        /// it covers what it stands for).
        Array<usize> GlyphStart;
        Array<usize> GlyphEnd;
        Color TextColor = Color::White;
        f32 Size = 0.0f;
        bool SyntheticBold = false;   ///< no bold face: draw twice, `Size / 24` apart
        bool SyntheticItalic = false; ///< no italic face: slant about the baseline
        bool Unshaped = false;        ///< its font has no shaper: draw TextStart..TextEnd at X as text (its glyphs only place carets)
        f32 Baseline = 0.0f;
        f32 X = 0.0f;     ///< where its first glyph starts
        f32 Width = 0.0f; ///< its advance
        usize TextStart = 0;
        usize TextEnd = 0;
        usize Line = 0;
        i32 Run = 0; ///< its run, or -1 for an ellipsis
    };

    /// One laid-out line.
    struct LaidOutLine
    {
        f32 Top = 0.0f;
        f32 Height = 0.0f;
        f32 Baseline = 0.0f;
        f32 Left = 0.0f;  ///< after alignment
        f32 Width = 0.0f; ///< without trailing spaces
        usize Start = 0;  ///< its bytes of the plain text, its line break left out
        usize End = 0;
        bool Ellipsized = false; ///< an ellipsis stands for some of its text
    };

    /// A rectangle a part of the text covers, with its run (an icon's, an underline's, a link's).
    struct LaidOutBox
    {
        Rectangle Rect{};
        i32 Run = 0;
        usize Line = 0;
        Color TextColor = Color::White; ///< an underline's colour
    };

    /// A document laid out.
    struct TextLayout
    {
        Array<LaidOutLine> Lines;
        Array<LaidOutGlyphs> GlyphRuns;
        Array<LaidOutBox> Icons;
        Array<LaidOutBox> Underlines;
        Array<LaidOutBox> Links; ///< one per run per line it spans
        f32 Width = 0.0f;  ///< the widest line
        f32 Height = 0.0f; ///< every line
        bool Clamped = false; ///< lines past the clamp were left out

        /// The run of the link under a point, or -1.
        [[nodiscard]] i32 LinkAt(core::Float2 point) const
        {
            for (const LaidOutBox& box : Links)
            {
                if (point.x >= box.Rect.x && point.x < box.Rect.x + box.Rect.width && point.y >= box.Rect.y &&
                    point.y < box.Rect.y + box.Rect.height)
                {
                    return box.Run;
                }
            }
            return -1;
        }

        /// The line a caret at `offset` stands on (at a wrap, the line it starts).
        [[nodiscard]] usize LineOf(usize offset) const
        {
            for (usize i = 0; i < Lines.Size(); ++i)
                if (offset < Lines[i].End || (offset == Lines[i].End && (i + 1 == Lines.Size() || Lines[i + 1].Start > offset)))
                    return i;
            return Lines.IsEmpty() ? 0 : Lines.Size() - 1;
        }
        /// Where a caret at `offset` stands across its line.
        [[nodiscard]] f32 CaretX(usize offset) const { return Lines.IsEmpty() ? 0.0f : CaretXOnLine(offset, LineOf(offset)); }
        /// Where a caret at `offset` stands across line `line` (a wrapped line's end: its own, not
        /// the next line's start).
        [[nodiscard]] f32 CaretXOnLine(usize offset, usize line) const
        {
            if (line >= Lines.Size())
                return 0.0f;
            f32 x = Lines[line].Left;
            bool found = false;
            for (const LaidOutGlyphs& run : GlyphRuns)
            {
                if (run.Line != line || run.Run < 0)
                    continue;
                for (usize g = 0; g < run.Glyphs.Size(); ++g)
                {
                    const fonts::GlyphPosition& glyph = run.Glyphs[g];
                    const usize start = run.GlyphStart[g], end = run.GlyphEnd[g];
                    if (offset >= start && offset < end)
                    {
                        // Inside a ligature: the share of its advance before the offset.
                        const f32 share = end > start ? static_cast<f32>(offset - start) / static_cast<f32>(end - start) : 0.0f;
                        return glyph.x + glyph.advance * share;
                    }
                    if (offset >= end)
                    {
                        x = glyph.x + glyph.advance;
                        found = true;
                    }
                }
            }
            return found ? x : Lines[line].Left;
        }
        /// The offset a caret takes for a point: the line under its y (the first above, the last
        /// below), then the glyph edge nearest its x.
        [[nodiscard]] usize OffsetAt(core::Float2 point) const
        {
            if (Lines.IsEmpty())
                return 0;
            usize line = Lines.Size() - 1;
            for (usize i = 0; i < Lines.Size(); ++i)
                if (point.y < Lines[i].Top + Lines[i].Height)
                {
                    line = i;
                    break;
                }
            return OffsetOnLine(line, point.x);
        }
        /// The offset on line `line` nearest across to `x`.
        [[nodiscard]] usize OffsetOnLine(usize line, f32 x) const
        {
            if (line >= Lines.Size())
                return 0;
            usize best = Lines[line].Start;
            usize lastStart = Lines[line].Start; // the last glyph's start: a wrapped line's end
            f32 bestDistance = Abs(x - Lines[line].Left);
            for (const LaidOutGlyphs& run : GlyphRuns)
            {
                if (run.Line != line || run.Run < 0)
                    continue;
                for (usize g = 0; g < run.Glyphs.Size(); ++g)
                {
                    const fonts::GlyphPosition& glyph = run.Glyphs[g];
                    const f32 left = Abs(x - glyph.x);
                    const f32 right = Abs(x - (glyph.x + glyph.advance));
                    if (left < bestDistance)
                    {
                        bestDistance = left;
                        best = run.GlyphStart[g];
                    }
                    if (right < bestDistance)
                    {
                        bestDistance = right;
                        best = run.GlyphEnd[g];
                    }
                    lastStart = core::Max(lastStart, run.GlyphStart[g]);
                }
            }
            best = core::Min(best, Lines[line].End);
            // Where a line wraps, its end is the next line's start: a caret there would stand on
            // the next line, so the end of a wrapped line is before its last space.
            if (best == Lines[line].End && line + 1 < Lines.Size() && Lines[line + 1].Start == best)
                best = lastStart;
            return best;
        }
    };

    /// The font a run draws in, and what of its style the face lacks (to synthesize).
    struct ResolvedRunFont
    {
        fonts::CachedFont* Font = nullptr;
        f32 Size = 16.0f;
        bool SyntheticBold = false;
        bool SyntheticItalic = false;
    };

    namespace detail
    {
        // "<family><suffix>": a styled face's family name.
        [[nodiscard]] inline core::String WithSuffix(core::StringView family, const char8_t* suffix)
        {
            core::String name(family);
            name.Append(core::StringView(suffix));
            return name;
        }
    }

    [[nodiscard]] inline ResolvedRunFont ResolveRunFont(const TextRun& run, const TextLayoutStyle& style)
    {
        ResolvedRunFont out;
        out.Font = style.Font;
        out.Size = run.Size.HasValue() ? run.Size.Value() : style.Size;
        const bool ownFont = run.Family.HasValue() || run.Size.HasValue() || run.Bold || run.Italic;
        if (!ownFont)
            return out;
        if (style.Fonts == nullptr)
        {
            // No service to ask: the label's font, its style synthesized.
            out.Size = style.Size;
            out.SyntheticBold = run.Bold;
            out.SyntheticItalic = run.Italic;
            return out;
        }
        fonts::IFontService& service = *style.Fonts;
        const core::StringView family = run.Family.HasValue() ? run.Family.Value().AsView() : style.Family.AsView();
        core::String face(family);
        if (run.Bold || run.Italic)
        {
            core::String styled = detail::WithSuffix(family, run.Bold && run.Italic ? u8" Bold Italic"
                                                             : run.Bold             ? u8" Bold"
                                                                                    : u8" Italic");
            if (service.HasFamily(styled.AsView()))
            {
                face = Move(styled);
            }
            else if (run.Bold && run.Italic && service.HasFamily(detail::WithSuffix(family, u8" Bold").AsView()))
            {
                face = detail::WithSuffix(family, u8" Bold");
                out.SyntheticItalic = true;
            }
            else
            {
                out.SyntheticBold = run.Bold;
                out.SyntheticItalic = run.Italic;
            }
        }
        if (fonts::CachedFont* font = service.GetFont(face.AsView(), out.Size))
            out.Font = font;
        return out;
    }

    namespace detail
    {
        // A piece the lines are made of: a word with the spaces after it, an icon, or a break.
        struct TextPiece
        {
            i32 Run = 0;
            core::StringView Text;
            usize Start = 0; // its bytes of the plain text
            ResolvedRunFont Font;
            f32 Width = 0.0f;
            f32 TrailingSpace = 0.0f; // the width of the spaces it ends with
            f32 Ascent = 0.0f;
            f32 Descent = 0.0f; // below the baseline, positive
            f32 Gap = 0.0f;
            bool Icon = false;
            bool LineBreak = false;
            f32 X = 0.0f;   // placed, from its line's start
            usize Line = 0; // the line it landed on
        };

        [[nodiscard]] inline usize CodepointCount(core::StringView text)
        {
            usize count = 0;
            for (usize i = 0; i < text.Size(); ++i)
                if ((static_cast<u8>(text[i]) & 0xC0) != 0x80)
                    ++count;
            return count;
        }

        inline void MeasurePiece(TextPiece& piece, f32 letterSpacing)
        {
            if (piece.Font.Font == nullptr || piece.Font.Font->font == nullptr)
                return;
            fonts::IFont& font = *piece.Font.Font->font;
            const fonts::FontMetrics metrics = font.Metrics();
            piece.Ascent = metrics.ascent;
            piece.Descent = Abs(metrics.descent);
            piece.Gap = metrics.lineGap;
            if (piece.Text.Size() == 0)
                return;
            piece.Width = font.MeasureString(piece.Text) + letterSpacing * static_cast<f32>(CodepointCount(piece.Text));
            usize spaces = piece.Text.Size();
            while (spaces > 0 && piece.Text[spaces - 1] == u8' ')
                --spaces;
            if (spaces < piece.Text.Size())
            {
                const core::StringView kept = piece.Text.SubStr(0, spaces);
                piece.TrailingSpace =
                    piece.Width - font.MeasureString(kept) - letterSpacing * static_cast<f32>(CodepointCount(kept));
            }
        }

        // The byte offset of each codepoint of `text` from `base`, and the end (count + 1 entries).
        inline void CodepointOffsets(core::StringView text, usize base, Array<usize>& out)
        {
            out.Clear();
            for (usize i = 0; i < text.Size(); ++i)
                if ((static_cast<u8>(text[i]) & 0xC0) != 0x80)
                    out.PushBack(base + i);
            out.PushBack(base + text.Size());
        }

        // Shifts everything laid out on `line` across by `dx`.
        inline void ShiftLine(TextLayout& layout, usize line, f32 dx)
        {
            if (dx == 0.0f)
                return;
            for (LaidOutGlyphs& run : layout.GlyphRuns)
                if (run.Line == line)
                {
                    run.X += dx;
                    for (fonts::GlyphPosition& g : run.Glyphs)
                        g.x += dx;
                }
            Array<LaidOutBox>* lists[] = {&layout.Icons, &layout.Underlines, &layout.Links};
            for (Array<LaidOutBox>* boxes : lists)
                for (LaidOutBox& box : *boxes)
                    if (box.Line == line)
                        box.Rect.x += dx;
        }

        // Keeps only what of `line` lies in [from, to) across; what straddles an edge is cut
        // (a glyph goes whole, a box is narrowed). Returns the right edge of what stayed, and sets
        // `left` to its left edge.
        inline f32 KeepSpan(TextLayout& layout, usize line, f32 from, f32 to, f32* left = nullptr)
        {
            f32 right = from;
            f32 leftmost = to;
            for (LaidOutGlyphs& run : layout.GlyphRuns)
            {
                if (run.Line != line || run.Unshaped)
                    continue;
                Array<fonts::GlyphPosition> glyphs;
                Array<usize> starts, ends;
                for (usize g = 0; g < run.Glyphs.Size(); ++g)
                {
                    const fonts::GlyphPosition& glyph = run.Glyphs[g];
                    if (glyph.x < from - 0.01f || glyph.x + glyph.advance > to + 0.01f)
                        continue;
                    glyphs.PushBack(glyph);
                    starts.PushBack(run.GlyphStart[g]);
                    ends.PushBack(run.GlyphEnd[g]);
                    right = Max(right, glyph.x + glyph.advance);
                    leftmost = Min(leftmost, glyph.x);
                }
                if (!glyphs.IsEmpty())
                {
                    // Its start and advance are its glyphs' that stayed.
                    run.X = glyphs[0].x;
                    run.Width = glyphs.Back().x + glyphs.Back().advance - run.X;
                }
                run.Glyphs = Move(glyphs);
                run.GlyphStart = Move(starts);
                run.GlyphEnd = Move(ends);
            }
            for (usize r = layout.GlyphRuns.Size(); r-- > 0;)
                if (layout.GlyphRuns[r].Line == line && !layout.GlyphRuns[r].Unshaped && layout.GlyphRuns[r].Glyphs.IsEmpty())
                    layout.GlyphRuns.RemoveAt(r);
            Array<LaidOutBox>* lists[] = {&layout.Icons, &layout.Underlines, &layout.Links};
            for (Array<LaidOutBox>* boxes : lists)
            {
                const bool whole = boxes == &layout.Icons; // an icon is not cut in two
                for (usize b = boxes->Size(); b-- > 0;)
                {
                    LaidOutBox& box = (*boxes)[b];
                    if (box.Line != line)
                        continue;
                    const f32 left = Max(box.Rect.x, from);
                    const f32 boxRight = Min(box.Rect.x + box.Rect.width, to);
                    if (boxRight <= left || (whole && (box.Rect.x < from - 0.01f || box.Rect.x + box.Rect.width > to + 0.01f)))
                    {
                        boxes->RemoveAt(b);
                        continue;
                    }
                    box.Rect.x = left;
                    box.Rect.width = boxRight - left;
                    if (whole)
                    {
                        right = Max(right, boxRight);
                        leftmost = Min(leftmost, left);
                    }
                }
            }
            if (left != nullptr)
                *left = leftmost;
            return right;
        }

        // An ellipsis on `line` at x in the style of `like`.
        inline void AddEllipsis(TextLayout& layout, usize line, f32 x, const LaidOutGlyphs& like, usize offset)
        {
            LaidOutGlyphs ellipsis;
            ellipsis.Font = like.Font;
            ellipsis.TextColor = like.TextColor;
            ellipsis.Size = like.Size;
            ellipsis.SyntheticBold = like.SyntheticBold;
            ellipsis.SyntheticItalic = like.SyntheticItalic;
            ellipsis.Baseline = layout.Lines[line].Baseline;
            ellipsis.X = x;
            ellipsis.Line = line;
            ellipsis.Run = -1;
            ellipsis.TextStart = ellipsis.TextEnd = offset;
            if (like.Font != nullptr && like.Font->shaper != nullptr && like.Font->font != nullptr &&
                like.Font->shaper->ShapeText(*like.Font->font, EllipsisFor(like.Font), x, ellipsis.Baseline, ellipsis.Glyphs)
                    .HasValue())
            {
                for (const fonts::GlyphPosition& g : ellipsis.Glyphs)
                {
                    ellipsis.Width += g.advance;
                    ellipsis.GlyphStart.PushBack(offset);
                    ellipsis.GlyphEnd.PushBack(offset);
                }
                layout.GlyphRuns.PushBack(Move(ellipsis));
            }
        }
        [[nodiscard]] inline f32 EllipsisWidth(const LaidOutGlyphs& like)
        {
            return like.Font != nullptr && like.Font->font != nullptr ? like.Font->font->MeasureString(EllipsisFor(like.Font)) : 0.0f;
        }

        // Ends `line` in an ellipsis where it passes `maxWidth` (always, when `forced`: the clamp cut
        // the lines after it), at its end, start or middle. The line is still unaligned (from 0).
        inline void Ellipsize(TextLayout& layout, usize line, f32 maxWidth, TextOverflow mode, bool forced)
        {
            LaidOutLine& at = layout.Lines[line];
            const LaidOutGlyphs* first = nullptr;
            const LaidOutGlyphs* last = nullptr;
            for (const LaidOutGlyphs& run : layout.GlyphRuns)
                if (run.Line == line && !run.Unshaped && !run.Glyphs.IsEmpty())
                {
                    if (first == nullptr)
                        first = &run;
                    last = &run;
                }
            if (last == nullptr)
                return; // nothing shaped to cut
            const LaidOutGlyphs firstStyle = *first; // copies: the runs change below
            const LaidOutGlyphs lastStyle = *last;
            const f32 width = at.Width;
            if (forced && mode != TextOverflow::Ellipsis)
                mode = TextOverflow::Ellipsis; // a clamped paragraph ends at its end
            const f32 e = EllipsisWidth(mode == TextOverflow::EllipsisStart ? firstStyle : lastStyle);
            const f32 room = Max(0.0f, maxWidth - e);
            if (mode == TextOverflow::Ellipsis)
            {
                const f32 limit = forced && width + e <= maxWidth ? width : room;
                const f32 right = KeepSpan(layout, line, 0.0f, limit);
                AddEllipsis(layout, line, right, lastStyle, at.End);
                at.Width = right + e;
            }
            else if (mode == TextOverflow::EllipsisStart)
            {
                // The tail from where its first whole glyph starts, moved up to the ellipsis.
                const f32 cut = width - room;
                f32 kept = cut;
                (void)KeepSpan(layout, line, cut, width + 1.0f, &kept);
                ShiftLine(layout, line, e - kept);
                AddEllipsis(layout, line, 0.0f, firstStyle, at.Start);
                at.Width = width - kept + e;
            }
            else
            {
                // A head and a tail of half the room each, the ellipsis between.
                const f32 half = room * 0.5f;
                const f32 tailFrom = width - half;
                // The tail moves next to the head: kept apart, then shifted.
                TextLayout tail = layout;
                const f32 headRight = KeepSpan(layout, line, 0.0f, half);
                f32 tailLeft = tailFrom;
                (void)KeepSpan(tail, line, tailFrom, width + 1.0f, &tailLeft);
                const f32 shift = headRight + e - tailLeft;
                for (LaidOutGlyphs& run : tail.GlyphRuns)
                    if (run.Line == line && !run.Unshaped)
                    {
                        run.X += shift;
                        for (fonts::GlyphPosition& g : run.Glyphs)
                            g.x += shift;
                        layout.GlyphRuns.PushBack(Move(run));
                    }
                for (LaidOutBox& box : tail.Icons)
                    if (box.Line == line)
                    {
                        box.Rect.x += shift;
                        layout.Icons.PushBack(box);
                    }
                for (LaidOutBox& box : tail.Underlines)
                    if (box.Line == line)
                    {
                        box.Rect.x += shift;
                        layout.Underlines.PushBack(box);
                    }
                for (LaidOutBox& box : tail.Links)
                    if (box.Line == line)
                    {
                        box.Rect.x += shift;
                        layout.Links.PushBack(box);
                    }
                AddEllipsis(layout, line, headRight, lastStyle, at.Start);
                at.Width = headRight + e + (width - tailLeft);
            }
            at.Ellipsized = true;
        }
    }

    /// Lay a document out.
    [[nodiscard]] inline TextLayout LayOutText(const TextDocument& document, const TextLayoutStyle& style)
    {
        using detail::TextPiece;
        const f32 spacing = style.LetterSpacing;
        Array<TextPiece> pieces;
        usize runStart = 0; // the run's first byte in the plain text
        for (usize r = 0; r < document.Runs.Size(); ++r)
        {
            const TextRun& run = document.Runs[r];
            const ResolvedRunFont font = ResolveRunFont(run, style);
            if (run.IsIcon())
            {
                TextPiece piece;
                piece.Run = static_cast<i32>(r);
                piece.Start = runStart;
                piece.Font = font;
                piece.Icon = true;
                detail::MeasurePiece(piece, spacing);
                piece.Width = font.Size;
                piece.Ascent = Max(piece.Ascent, font.Size);
                pieces.PushBack(piece);
                runStart += 3; // U+FFFC in the plain text
                continue;
            }
            const core::StringView text = run.Text.AsView();
            usize start = 0;
            usize i = 0;
            const auto emit = [&](usize end)
            {
                if (end <= start)
                    return;
                TextPiece piece;
                piece.Run = static_cast<i32>(r);
                piece.Text = text.SubStr(start, end - start);
                piece.Start = runStart + start;
                piece.Font = font;
                detail::MeasurePiece(piece, spacing);
                pieces.PushBack(piece);
            };
            while (i < text.Size())
            {
                if (text[i] == u8'\n')
                {
                    emit(i);
                    TextPiece lineBreak;
                    lineBreak.Run = static_cast<i32>(r);
                    lineBreak.Start = runStart + i;
                    lineBreak.Font = font;
                    lineBreak.LineBreak = true;
                    detail::MeasurePiece(lineBreak, spacing);
                    pieces.PushBack(lineBreak);
                    start = ++i;
                    continue;
                }
                if (text[i] == u8' ')
                {
                    while (i < text.Size() && text[i] == u8' ')
                        ++i;
                    emit(i); // a word takes the spaces after it
                    start = i;
                    continue;
                }
                ++i;
            }
            emit(text.Size());
            runStart += text.Size();
        }

        // The base font's metrics size an empty line.
        TextPiece empty;
        empty.Font = ResolveRunFont(TextRun{}, style);
        detail::MeasurePiece(empty, spacing);

        TextLayout layout;
        const bool bounded = style.MaxWidth > 0.0f;
        usize lineStart = 0;
        f32 x = 0.0f;
        usize nextOffset = 0; // where the next line's text starts
        const auto finishLine = [&](usize end)
        {
            LaidOutLine line;
            line.Top = layout.Height;
            f32 ascent = 0.0f, descent = 0.0f, gap = 0.0f;
            bool any = false;
            for (usize p = lineStart; p < end; ++p)
            {
                ascent = Max(ascent, pieces[p].Ascent);
                descent = Max(descent, pieces[p].Descent);
                gap = Max(gap, pieces[p].Gap);
                any = any || !pieces[p].LineBreak;
            }
            if (!any)
            {
                // A line with nothing on it is as tall as its break's font, or the base font's.
                const TextPiece& sized = (end > lineStart) ? pieces[lineStart] : empty;
                ascent = sized.Ascent;
                descent = sized.Descent;
                gap = sized.Gap;
            }
            if (style.LineHeight > 0.0f)
            {
                // CSS's half-leading: the line's extra height (or what it lacks) split above and below.
                line.Height = style.LineHeight;
                line.Baseline = line.Top + (style.LineHeight - (ascent + descent)) * 0.5f + ascent;
            }
            else
            {
                line.Height = ascent + descent + gap;
                line.Baseline = line.Top + ascent;
            }
            line.Width = x;
            for (usize p = end; p > lineStart; --p)
            {
                if (pieces[p - 1].LineBreak)
                    continue;
                line.Width -= pieces[p - 1].TrailingSpace;
                break;
            }
            line.Start = nextOffset;
            line.End = nextOffset;
            for (usize p = lineStart; p < end; ++p)
            {
                pieces[p].Line = layout.Lines.Size();
                if (pieces[p].LineBreak)
                {
                    line.End = pieces[p].Start;
                    nextOffset = pieces[p].Start + 1;
                }
                else
                {
                    line.End = pieces[p].Start + (pieces[p].Icon ? 3 : pieces[p].Text.Size());
                    nextOffset = line.End;
                }
            }
            layout.Lines.PushBack(line);
            layout.Height += line.Height;
            lineStart = end;
            x = 0.0f;
        };
        for (usize p = 0; p < pieces.Size(); ++p)
        {
            TextPiece& piece = pieces[p];
            if (piece.LineBreak)
            {
                finishLine(p + 1);
                continue;
            }
            if (style.Wrap && bounded && p > lineStart && x + piece.Width - piece.TrailingSpace > style.MaxWidth)
                finishLine(p);
            piece.X = x;
            x += piece.Width;
        }
        if (lineStart < pieces.Size() || layout.Lines.IsEmpty() || (!pieces.IsEmpty() && pieces.Back().LineBreak))
            finishLine(pieces.Size());

        // The clamp: the lines past it go.
        usize keptLines = layout.Lines.Size();
        if (style.MaxLines > 0 && keptLines > style.MaxLines)
        {
            keptLines = style.MaxLines;
            layout.Lines.Resize(keptLines);
            layout.Clamped = true;
            layout.Height = layout.Lines.Back().Top + layout.Lines.Back().Height;
        }

        // Glyphs, icons, underlines and links, from each line's start.
        Array<usize> offsets;
        for (usize p = 0; p < pieces.Size(); ++p)
        {
            const TextPiece& piece = pieces[p];
            if (piece.LineBreak || piece.Line >= keptLines)
                continue;
            const LaidOutLine& at = layout.Lines[piece.Line];
            const TextRun& run = document.Runs[static_cast<usize>(piece.Run)];
            const Color color = run.TextColor.HasValue() ? run.TextColor.Value() : style.TextColor;
            const f32 visible = piece.Width - piece.TrailingSpace;
            if (piece.Icon)
            {
                LaidOutBox icon;
                icon.Rect = Rectangle{piece.X, at.Baseline - piece.Font.Size, piece.Font.Size, piece.Font.Size};
                icon.Run = piece.Run;
                icon.Line = piece.Line;
                layout.Icons.PushBack(icon);
            }
            else if (piece.Font.Font != nullptr)
            {
                LaidOutGlyphs glyphs;
                glyphs.Font = piece.Font.Font;
                glyphs.TextColor = color;
                glyphs.Size = piece.Font.Size;
                glyphs.SyntheticBold = piece.Font.SyntheticBold;
                glyphs.SyntheticItalic = piece.Font.SyntheticItalic;
                glyphs.Baseline = at.Baseline;
                glyphs.X = piece.X;
                glyphs.Width = piece.Width;
                glyphs.TextStart = piece.Start;
                glyphs.TextEnd = piece.Start + piece.Text.Size();
                glyphs.Line = piece.Line;
                glyphs.Run = piece.Run;
                if (piece.Font.Font->shaper != nullptr && piece.Font.Font->font != nullptr &&
                    piece.Font.Font->shaper->ShapeText(*piece.Font.Font->font, piece.Text, piece.X, at.Baseline, glyphs.Glyphs)
                        .HasValue())
                {
                    // Each glyph's bytes from its cluster (a character index); letter spacing after
                    // every character before it.
                    detail::CodepointOffsets(piece.Text, piece.Start, offsets);
                    const usize count = offsets.Size() - 1;
                    for (fonts::GlyphPosition& g : glyphs.Glyphs)
                    {
                        const usize first = core::Min(static_cast<usize>(core::Max(0, g.cluster)), count);
                        const usize last = core::Min(static_cast<usize>(core::Max(0, g.ClusterEnd())), count);
                        glyphs.GlyphStart.PushBack(offsets[first]);
                        glyphs.GlyphEnd.PushBack(offsets[last]);
                        g.x += spacing * static_cast<f32>(first);
                        g.advance += spacing * static_cast<f32>(last - first);
                    }
                }
                else
                {
                    // No shaper: a position for each character from the widths of what comes
                    // before it, so carets still find their places; drawn as text.
                    glyphs.Glyphs.Clear();
                    glyphs.Unshaped = true;
                    detail::CodepointOffsets(piece.Text, piece.Start, offsets);
                    f32 before = 0.0f;
                    for (usize c = 0; c + 1 < offsets.Size(); ++c)
                    {
                        const usize end = offsets[c + 1] - piece.Start;
                        const f32 after = piece.Font.Font->font != nullptr
                                              ? piece.Font.Font->font->MeasureString(piece.Text.SubStr(0, end)) + spacing * static_cast<f32>(c + 1)
                                              : before;
                        fonts::GlyphPosition g;
                        g.cluster = static_cast<i32>(c);
                        g.x = piece.X + before;
                        g.y = at.Baseline;
                        g.advance = after - before;
                        glyphs.Glyphs.PushBack(g);
                        glyphs.GlyphStart.PushBack(offsets[c]);
                        glyphs.GlyphEnd.PushBack(offsets[c + 1]);
                        before = after;
                    }
                }
                layout.GlyphRuns.PushBack(Move(glyphs));
            }
            if (run.Underline && visible > 0.0f && piece.Font.Font != nullptr && piece.Font.Font->font != nullptr)
            {
                const fonts::TextDecorationMetrics decorations = piece.Font.Font->font->Metrics().decorations;
                LaidOutBox underline;
                // A word's underline runs on under the spaces after it unless it ends the line.
                const bool lastOnLine = p + 1 >= pieces.Size() || pieces[p + 1].LineBreak || pieces[p + 1].Line != piece.Line;
                underline.Rect = Rectangle{piece.X, at.Baseline + decorations.underlinePosition,
                                           lastOnLine ? visible : piece.Width, decorations.underlineThickness};
                underline.Run = piece.Run;
                underline.Line = piece.Line;
                underline.TextColor = color;
                layout.Underlines.PushBack(underline);
            }
            if (run.Link.Size() != 0)
            {
                LaidOutBox link;
                link.Rect = Rectangle{piece.X, at.Top, piece.Width, at.Height};
                link.Run = piece.Run;
                link.Line = piece.Line;
                layout.Links.PushBack(link);
            }
        }

        // An ellipsis where a line passes the width (or ends a clamped paragraph), then alignment.
        for (usize l = 0; l < layout.Lines.Size(); ++l)
        {
            const bool forced = layout.Clamped && l + 1 == layout.Lines.Size() && style.Overflow != TextOverflow::Clip;
            if (bounded && style.Overflow != TextOverflow::Clip && (forced || layout.Lines[l].Width > style.MaxWidth + 0.01f))
                detail::Ellipsize(layout, l, style.MaxWidth, style.Overflow, forced);
            LaidOutLine& line = layout.Lines[l];
            if (bounded && style.Align == TextHAlign::Center)
                line.Left = (style.MaxWidth - line.Width) * 0.5f;
            else if (bounded && style.Align == TextHAlign::Right)
                line.Left = style.MaxWidth - line.Width;
            detail::ShiftLine(layout, l, line.Left);
            layout.Width = Max(layout.Width, line.Width);
        }
        return layout;
    }
}
