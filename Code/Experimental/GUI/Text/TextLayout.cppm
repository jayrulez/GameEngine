// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_layout partition
//
// The inline formatter: lays a TextDocument's runs into lines. Text breaks after spaces and at line
// breaks, runs of different sizes share a baseline (each line as tall as its tallest run), an icon
// sits on the baseline as tall as its run's text is big, and each line aligns on its own. What comes
// out is positioned glyphs per run, underlines, icon rectangles and link rectangles, in the layout's
// own coordinates; the label draws them.
//
// A run that sets no font of its own draws in the style's font (the label's). One that sets a family,
// a size, bold or italic asks the font service; bold and italic take the family's "<family> Bold",
// "<family> Italic" or "<family> Bold Italic" face when one is loaded, and are synthesized when not
// (bold drawn twice a little apart, italic slanted), as browsers do. With no service, such a run
// draws in the style's font, its bold and italic synthesized.
//
// Copied from foundation.ui's :text_layout (E4 of Specs/gui-lab-evolution.md), and the lab's own
// from then on.

module;
#include "Core/Prelude.h"

export module experimental.gui:text_layout;

import foundation.core;  // Array, String, Rectangle, Float2, Max, Abs
import foundation.fonts; // CachedFont, IFontService, GlyphPosition
import :text;            // TextHAlign
import :text_runs;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    /// What a run takes when its markup leaves a style unset, and how the lines go.
    struct TextLayoutStyle
    {
        fonts::CachedFont* Font = nullptr;      ///< the label's font: plain runs draw in it
        fonts::IFontService* Fonts = nullptr;   ///< resolves a run's own family, size, bold and italic
        core::String Family;                    ///< the label's family as the service names it
        f32 Size = 16.0f;                       ///< the label's size in pixels
        Color TextColor = Color::White;
        TextHAlign Align = TextHAlign::Left;
        bool Wrap = true;
        f32 MaxWidth = 0.0f; ///< The width lines wrap at and align in (wrap or not); 0: unbounded.
    };

    /// A run's glyphs on one line: positions in layout coordinates, y on the baseline.
    struct LaidOutGlyphs
    {
        fonts::CachedFont* Font = nullptr;
        Array<fonts::GlyphPosition> Glyphs;
        Color TextColor = Color::White;
        f32 Size = 0.0f;
        bool SyntheticBold = false;   ///< no bold face: draw twice, `Size / 24` apart
        bool SyntheticItalic = false; ///< no italic face: slant about the baseline
        f32 Baseline = 0.0f;
        i32 Run = 0;
    };

    /// One laid-out line.
    struct LaidOutLine
    {
        f32 Top = 0.0f;
        f32 Height = 0.0f;
        f32 Baseline = 0.0f;
        f32 Left = 0.0f;  ///< after alignment
        f32 Width = 0.0f; ///< without trailing spaces
    };

    /// A rectangle a part of the text covers, with its run (an icon's, an underline's, a link's).
    struct LaidOutBox
    {
        Rectangle Rect{};
        i32 Run = 0;
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
            ResolvedRunFont Font;
            f32 Width = 0.0f;
            f32 TrailingSpace = 0.0f; // the width of the spaces it ends with
            f32 Ascent = 0.0f;
            f32 Descent = 0.0f; // below the baseline, positive
            f32 Gap = 0.0f;
            bool Icon = false;
            bool LineBreak = false;
            f32 X = 0.0f;   // placed
            usize Line = 0; // the line it landed on
        };

        inline void MeasurePiece(TextPiece& piece)
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
            piece.Width = font.MeasureString(piece.Text);
            usize spaces = piece.Text.Size();
            while (spaces > 0 && piece.Text[spaces - 1] == u8' ')
                --spaces;
            if (spaces < piece.Text.Size())
                piece.TrailingSpace = piece.Width - font.MeasureString(piece.Text.SubStr(0, spaces));
        }
    }

    /// Lay a document out.
    [[nodiscard]] inline TextLayout LayOutText(const TextDocument& document, const TextLayoutStyle& style)
    {
        using detail::TextPiece;
        Array<TextPiece> pieces;
        for (usize r = 0; r < document.Runs.Size(); ++r)
        {
            const TextRun& run = document.Runs[r];
            const ResolvedRunFont font = ResolveRunFont(run, style);
            if (run.IsIcon())
            {
                TextPiece piece;
                piece.Run = static_cast<i32>(r);
                piece.Font = font;
                piece.Icon = true;
                detail::MeasurePiece(piece);
                piece.Width = font.Size;
                piece.Ascent = Max(piece.Ascent, font.Size);
                pieces.PushBack(piece);
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
                piece.Font = font;
                detail::MeasurePiece(piece);
                pieces.PushBack(piece);
            };
            while (i < text.Size())
            {
                if (text[i] == u8'\n')
                {
                    emit(i);
                    TextPiece lineBreak;
                    lineBreak.Run = static_cast<i32>(r);
                    lineBreak.Font = font;
                    lineBreak.LineBreak = true;
                    detail::MeasurePiece(lineBreak);
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
        }

        // The base font's metrics size an empty line.
        TextPiece empty;
        empty.Font = ResolveRunFont(TextRun{}, style);
        detail::MeasurePiece(empty);

        TextLayout layout;
        const bool bounded = style.MaxWidth > 0.0f;
        usize lineStart = 0;
        f32 x = 0.0f;
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
            line.Height = ascent + descent + gap;
            line.Baseline = line.Top + ascent;
            line.Width = x;
            for (usize p = end; p > lineStart; --p)
            {
                if (pieces[p - 1].LineBreak)
                    continue;
                line.Width -= pieces[p - 1].TrailingSpace;
                break;
            }
            if (bounded && style.Align == TextHAlign::Center)
                line.Left = (style.MaxWidth - line.Width) * 0.5f;
            else if (bounded && style.Align == TextHAlign::Right)
                line.Left = style.MaxWidth - line.Width;
            for (usize p = lineStart; p < end; ++p)
            {
                pieces[p].X += line.Left;
                pieces[p].Line = layout.Lines.Size();
            }
            layout.Lines.PushBack(line);
            layout.Height += line.Height;
            layout.Width = Max(layout.Width, line.Width);
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

        // Glyphs, icons, underlines and links.
        for (usize p = 0; p < pieces.Size(); ++p)
        {
            const TextPiece& piece = pieces[p];
            if (piece.LineBreak)
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
                layout.Icons.PushBack(icon);
            }
            else if (piece.Font.Font != nullptr && piece.Font.Font->shaper != nullptr)
            {
                LaidOutGlyphs glyphs;
                glyphs.Font = piece.Font.Font;
                glyphs.TextColor = color;
                glyphs.Size = piece.Font.Size;
                glyphs.SyntheticBold = piece.Font.SyntheticBold;
                glyphs.SyntheticItalic = piece.Font.SyntheticItalic;
                glyphs.Baseline = at.Baseline;
                glyphs.Run = piece.Run;
                if (piece.Font.Font->shaper->ShapeText(*piece.Font.Font->font, piece.Text, piece.X, at.Baseline, glyphs.Glyphs)
                        .HasValue())
                {
                    layout.GlyphRuns.PushBack(Move(glyphs));
                }
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
                underline.TextColor = color;
                layout.Underlines.PushBack(underline);
            }
            if (run.Link.Size() != 0)
            {
                LaidOutBox link;
                link.Rect = Rectangle{piece.X, at.Top, piece.Width, at.Height};
                link.Run = piece.Run;
                layout.Links.PushBack(link);
            }
        }
        return layout;
    }
}
