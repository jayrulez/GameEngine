// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :text_layout partition
//
// The inline formatter (F2 of Specs/vg-and-gui-lab.md): lays a TextDocument's runs into lines.
// Text breaks after spaces and at line breaks, runs of different sizes share a baseline (each
// line as tall as its tallest run), an icon sits on the baseline as tall as its run's text is
// big, and each line aligns on its own. What comes out is positioned glyphs per run, underlines,
// icon rectangles and link rectangles, in the layout's own coordinates; the control draws them.
//
// Bold and italic take the family's "<family> Bold", "<family> Italic" or "<family> Bold Italic"
// face when one is loaded, and are synthesized when not (bold drawn twice a little apart, italic
// slanted), as browsers do.

module;
#include "Core/Prelude.h"

export module foundation.ui:text_layout;

import foundation.core;
import foundation.fonts;
import :text_runs;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui
{
    /// What a run takes when its markup leaves a style unset, and how the lines go.
    struct TextLayoutStyle
    {
        String family;
        f32 size = 16.0f;
        Color color = Color::White;
        fonts::TextAlignment align = fonts::TextAlignment::Left;
        bool wrap = true;
        f32 maxWidth = 0.0f; ///< The width lines wrap at and align in (wrap or not); 0: unbounded.
    };

    /// A run's glyphs on one line: positions in layout coordinates, y on the baseline.
    struct LaidOutGlyphs
    {
        fonts::CachedFont* font = nullptr;
        Array<fonts::GlyphPosition> glyphs;
        Color color = Color::White;
        f32 size = 0.0f;
        bool syntheticBold = false;   ///< no bold face: draw twice, `size / 24` apart
        bool syntheticItalic = false; ///< no italic face: slant about the baseline
        f32 baseline = 0.0f;
        i32 run = 0;
    };

    /// One laid-out line.
    struct LaidOutLine
    {
        f32 top = 0.0f;
        f32 height = 0.0f;
        f32 baseline = 0.0f;
        f32 left = 0.0f;  ///< after alignment
        f32 width = 0.0f; ///< without trailing spaces
    };

    /// A rectangle a part of the text covers, with its run (an icon's, an underline's, a link's).
    struct LaidOutBox
    {
        Rectangle rect{};
        i32 run = 0;
        Color color = Color::White; ///< an underline's colour
    };

    /// A document laid out.
    struct TextLayout
    {
        Array<LaidOutLine> lines;
        Array<LaidOutGlyphs> glyphRuns;
        Array<LaidOutBox> icons;
        Array<LaidOutBox> underlines;
        Array<LaidOutBox> links; ///< one per run per line it spans
        f32 width = 0.0f;  ///< the widest line
        f32 height = 0.0f; ///< every line

        /// The run of the link under a point, or -1.
        [[nodiscard]] i32 LinkAt(Float2 point) const
        {
            for (const LaidOutBox& box : links)
            {
                if (point.x >= box.rect.x && point.x < box.rect.x + box.rect.width && point.y >= box.rect.y &&
                    point.y < box.rect.y + box.rect.height)
                {
                    return box.run;
                }
            }
            return -1;
        }
    };

    /// The font a run draws in, and what of its style the face lacks (to synthesize).
    struct ResolvedRunFont
    {
        fonts::CachedFont* font = nullptr;
        f32 size = 16.0f;
        bool syntheticBold = false;
        bool syntheticItalic = false;
    };

    // "<family><suffix>": a styled face's family name.
    [[nodiscard]] inline String WithSuffix(StringView family, const char8_t* suffix)
    {
        String name(family);
        name.Append(StringView(suffix));
        return name;
    }

    [[nodiscard]] inline ResolvedRunFont ResolveRunFont(const TextRun& run, const TextLayoutStyle& style,
                                                        fonts::IFontService& service)
    {
        ResolvedRunFont out;
        const StringView family = run.family.HasValue() ? run.family.Value().AsView() : style.family.AsView();
        out.size = run.size.HasValue() ? run.size.Value() : style.size;
        String face(family);
        if (run.bold || run.italic)
        {
            String styled = WithSuffix(family, run.bold && run.italic ? u8" Bold Italic"
                                               : run.bold             ? u8" Bold"
                                                                      : u8" Italic");
            if (service.HasFamily(styled.AsView()))
            {
                face = Move(styled);
            }
            else if (run.bold && run.italic && service.HasFamily(WithSuffix(family, u8" Bold").AsView()))
            {
                face = WithSuffix(family, u8" Bold");
                out.syntheticItalic = true;
            }
            else
            {
                out.syntheticBold = run.bold;
                out.syntheticItalic = run.italic;
            }
        }
        out.font = service.GetFont(face.AsView(), out.size);
        return out;
    }

    namespace detail
    {
        // A piece the lines are made of: a word with the spaces after it, an icon, or a break.
        struct TextPiece
        {
            i32 run = 0;
            StringView text;
            ResolvedRunFont font;
            f32 width = 0.0f;
            f32 trailingSpace = 0.0f; // the width of the spaces it ends with
            f32 ascent = 0.0f;
            f32 descent = 0.0f; // below the baseline, positive
            f32 gap = 0.0f;
            bool icon = false;
            bool lineBreak = false;
            f32 x = 0.0f;   // placed
            usize line = 0; // the line it landed on
        };

        inline void MeasurePiece(TextPiece& piece)
        {
            if (piece.font.font == nullptr || piece.font.font->font == nullptr)
                return;
            fonts::IFont& font = *piece.font.font->font;
            const fonts::FontMetrics metrics = font.Metrics();
            piece.ascent = metrics.ascent;
            piece.descent = Abs(metrics.descent);
            piece.gap = metrics.lineGap;
            if (piece.text.IsEmpty())
                return;
            piece.width = font.MeasureString(piece.text);
            usize spaces = piece.text.Size();
            while (spaces > 0 && piece.text[spaces - 1] == u8' ')
                --spaces;
            if (spaces < piece.text.Size())
                piece.trailingSpace = piece.width - font.MeasureString(piece.text.SubStr(0, spaces));
        }
    }

    /// Lay a document out.
    [[nodiscard]] inline TextLayout LayOutText(const TextDocument& document, const TextLayoutStyle& style,
                                               fonts::IFontService& service)
    {
        using detail::TextPiece;
        Array<TextPiece> pieces;
        for (usize r = 0; r < document.runs.Size(); ++r)
        {
            const TextRun& run = document.runs[r];
            const ResolvedRunFont font = ResolveRunFont(run, style, service);
            if (run.IsIcon())
            {
                TextPiece piece;
                piece.run = static_cast<i32>(r);
                piece.font = font;
                piece.icon = true;
                detail::MeasurePiece(piece);
                piece.width = font.size;
                piece.ascent = Max(piece.ascent, font.size);
                pieces.PushBack(piece);
                continue;
            }
            const StringView text = run.text.AsView();
            usize start = 0;
            usize i = 0;
            const auto emit = [&](usize end)
            {
                if (end <= start)
                    return;
                TextPiece piece;
                piece.run = static_cast<i32>(r);
                piece.text = text.SubStr(start, end - start);
                piece.font = font;
                detail::MeasurePiece(piece);
                pieces.PushBack(piece);
            };
            while (i < text.Size())
            {
                if (text[i] == u8'\n')
                {
                    emit(i);
                    TextPiece lineBreak;
                    lineBreak.run = static_cast<i32>(r);
                    lineBreak.font = font;
                    lineBreak.lineBreak = true;
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
        TextRun plain;
        const ResolvedRunFont base = ResolveRunFont(plain, style, service);
        TextPiece empty;
        empty.font = base;
        detail::MeasurePiece(empty);

        TextLayout layout;
        const bool bounded = style.maxWidth > 0.0f;
        usize lineStart = 0;
        f32 x = 0.0f;
        const auto finishLine = [&](usize end)
        {
            LaidOutLine line;
            line.top = layout.height;
            f32 ascent = 0.0f, descent = 0.0f, gap = 0.0f;
            bool any = false;
            for (usize p = lineStart; p < end; ++p)
            {
                ascent = Max(ascent, pieces[p].ascent);
                descent = Max(descent, pieces[p].descent);
                gap = Max(gap, pieces[p].gap);
                any = any || !pieces[p].lineBreak;
            }
            if (!any)
            {
                // A line with nothing on it is as tall as its break's font, or the base font's.
                const TextPiece& sized = (end > lineStart) ? pieces[lineStart] : empty;
                ascent = sized.ascent;
                descent = sized.descent;
                gap = sized.gap;
            }
            line.height = ascent + descent + gap;
            line.baseline = line.top + ascent;
            line.width = x;
            for (usize p = end; p > lineStart; --p)
            {
                if (pieces[p - 1].lineBreak)
                    continue;
                line.width -= pieces[p - 1].trailingSpace;
                break;
            }
            if (bounded && style.align == fonts::TextAlignment::Center)
                line.left = (style.maxWidth - line.width) * 0.5f;
            else if (bounded && style.align == fonts::TextAlignment::Right)
                line.left = style.maxWidth - line.width;
            for (usize p = lineStart; p < end; ++p)
            {
                pieces[p].x += line.left;
                pieces[p].line = layout.lines.Size();
            }
            layout.lines.PushBack(line);
            layout.height += line.height;
            layout.width = Max(layout.width, line.width);
            lineStart = end;
            x = 0.0f;
        };
        for (usize p = 0; p < pieces.Size(); ++p)
        {
            TextPiece& piece = pieces[p];
            if (piece.lineBreak)
            {
                finishLine(p + 1);
                continue;
            }
            if (style.wrap && bounded && p > lineStart && x + piece.width - piece.trailingSpace > style.maxWidth)
                finishLine(p);
            piece.x = x;
            x += piece.width;
        }
        if (lineStart < pieces.Size() || layout.lines.IsEmpty() ||
            (!pieces.IsEmpty() && pieces.Back().lineBreak))
            finishLine(pieces.Size());

        // Glyphs, icons, underlines and links.
        {
            for (usize p = 0; p < pieces.Size(); ++p)
            {
                const TextPiece& piece = pieces[p];
                if (piece.lineBreak)
                    continue;
                const LaidOutLine& at = layout.lines[piece.line];
                const TextRun& run = document.runs[static_cast<usize>(piece.run)];
                const Color color = run.color.HasValue() ? run.color.Value() : style.color;
                const f32 visible = piece.width - piece.trailingSpace;
                if (piece.icon)
                {
                    LaidOutBox icon;
                    icon.rect = Rectangle{piece.x, at.baseline - piece.font.size, piece.font.size, piece.font.size};
                    icon.run = piece.run;
                    layout.icons.PushBack(icon);
                }
                else if (piece.font.font != nullptr && piece.font.font->shaper != nullptr)
                {
                    LaidOutGlyphs glyphs;
                    glyphs.font = piece.font.font;
                    glyphs.color = color;
                    glyphs.size = piece.font.size;
                    glyphs.syntheticBold = piece.font.syntheticBold;
                    glyphs.syntheticItalic = piece.font.syntheticItalic;
                    glyphs.baseline = at.baseline;
                    glyphs.run = piece.run;
                    if (piece.font.font->shaper->ShapeText(*piece.font.font->font, piece.text, piece.x, at.baseline,
                                                           glyphs.glyphs)
                            .HasValue())
                    {
                        layout.glyphRuns.PushBack(Move(glyphs));
                    }
                }
                if (run.underline && visible > 0.0f && piece.font.font != nullptr)
                {
                    const fonts::TextDecorationMetrics decorations = piece.font.font->font->Metrics().decorations;
                    LaidOutBox underline;
                    // A word's underline runs on under the spaces after it unless it ends the line.
                    const bool lastOnLine =
                        p + 1 >= pieces.Size() || pieces[p + 1].lineBreak || pieces[p + 1].line != piece.line;
                    underline.rect = Rectangle{piece.x, at.baseline + decorations.underlinePosition,
                                               lastOnLine ? visible : piece.width, decorations.underlineThickness};
                    underline.run = piece.run;
                    underline.color = color;
                    layout.underlines.PushBack(underline);
                }
                if (!run.link.IsEmpty())
                {
                    LaidOutBox link;
                    link.rect = Rectangle{piece.x, at.top, piece.width, at.height};
                    link.run = piece.run;
                    layout.links.PushBack(link);
                }
            }
        }
        return layout;
    }
}
