// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text partition
//
// Text: a measurable, drawable run of text on a font, rendered through the DrawContext/VG
// seam. Derived from eepp's Graphics::Text (the object UITextView/labels cache), adapted to
// our needs: it holds a non-owning foundation.fonts CachedFont (owned by the font service),
// measures via IFont metrics, and draws via VG's DrawText. Rich styling (per-range colors,
// outline/shadow, wrap, bidi) is not implemented.

module;
#include "Core/Prelude.h"

export module experimental.gui:text;

import foundation.core;  // String, StringView, Float2, Color, Array, IsWhiteSpace
import foundation.fonts; // CachedFont, IFont, FontMetrics
import :rect;
import :draw_context;

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

    class Text
    {
    public:
        Text() = default;
        Text(core::StringView string, fonts::CachedFont* font, Color color = Color::White)
            : m_string(string), m_font(font), m_color(color)
        {
        }

        void SetString(core::StringView string) { m_string = string; }
        [[nodiscard]] core::StringView GetString() const { return m_string.AsView(); }
        [[nodiscard]] bool IsEmpty() const { return m_string.AsView().Size() == 0; }

        void SetFont(fonts::CachedFont* font) { m_font = font; }
        [[nodiscard]] fonts::CachedFont* GetFont() const noexcept { return m_font; }

        void SetColor(Color color) noexcept { m_color = color; }
        [[nodiscard]] Color GetColor() const noexcept { return m_color; }

        void SetAlignment(TextHAlign horizontal, TextVAlign vertical) noexcept
        {
            m_hAlign = horizontal;
            m_vAlign = vertical;
        }
        [[nodiscard]] TextHAlign GetHAlign() const noexcept { return m_hAlign; }
        [[nodiscard]] TextVAlign GetVAlign() const noexcept { return m_vAlign; }

        // Word wrap: when on, Draw(bounds) breaks the text into lines that fit the bounds
        // width (greedy at whitespace; explicit '\n' always breaks). Off = single line.
        void SetWordWrap(bool enabled) noexcept { m_wordWrap = enabled; }
        [[nodiscard]] bool IsWordWrap() const noexcept { return m_wordWrap; }

        // === Measurement (0 if no font) ===
        [[nodiscard]] f32 GetWidth() const
        {
            return HasFont() ? m_font->font->MeasureString(m_string.AsView()) : 0.0f;
        }
        [[nodiscard]] f32 GetLineHeight() const
        {
            return HasFont() ? m_font->font->Metrics().lineHeight : 0.0f;
        }
        [[nodiscard]] core::Float2 Measure() const
        {
            return core::Float2{GetWidth(), GetLineHeight()};
        }

        // Break the string into lines that fit `maxWidth` (greedy at whitespace; '\n' always
        // breaks). A word longer than maxWidth takes its own line and overflows (no in-word
        // breaking yet). Returned views point into the current string - valid until it changes.
        void ComputeLines(f32 maxWidth, core::Array<core::StringView>& out) const
        {
            out.Clear();
            const core::StringView v = m_string.AsView();
            const usize n = v.Size();
            if (!HasFont() || n == 0)
            {
                if (n != 0)
                    out.PushBack(v);
                return;
            }

            usize lineStart = 0;
            usize lastFitEnd = 0; // end of content that fits on the current line
            usize i = 0;
            while (i < n)
            {
                if (v[i] == u8'\n')
                {
                    out.PushBack(v.SubStr(lineStart, i - lineStart));
                    lineStart = i + 1;
                    lastFitEnd = lineStart;
                    i = lineStart;
                    continue;
                }
                usize j = i;
                while (j < n && core::IsWhiteSpace(v[j]) && v[j] != u8'\n')
                    ++j; // leading spaces
                while (j < n && !core::IsWhiteSpace(v[j]))
                    ++j; // the word
                const f32 w = m_font->font->MeasureString(v.SubStr(lineStart, j - lineStart));
                if (w <= maxWidth || lastFitEnd == lineStart)
                {
                    lastFitEnd = j; // fits (or the line is empty -> force one word)
                    i = j;
                }
                else
                {
                    usize end = lastFitEnd; // break before this word; trim trailing spaces
                    while (end > lineStart && core::IsWhiteSpace(v[end - 1]))
                        --end;
                    out.PushBack(v.SubStr(lineStart, end - lineStart));
                    usize ns = lastFitEnd; // skip leading spaces on the next line
                    while (ns < n && core::IsWhiteSpace(v[ns]) && v[ns] != u8'\n')
                        ++ns;
                    lineStart = ns;
                    lastFitEnd = ns;
                    i = ns;
                }
            }
            usize end = n; // flush the remainder
            while (end > lineStart && core::IsWhiteSpace(v[end - 1]))
                --end;
            out.PushBack(v.SubStr(lineStart, end - lineStart));
        }

        // Size the wrapped text occupies at `maxWidth`: widest line x (line count * lineHeight).
        [[nodiscard]] core::Float2 MeasureWrapped(f32 maxWidth) const
        {
            if (!HasFont() || IsEmpty())
                return core::Float2{0.0f, 0.0f};
            core::Array<core::StringView> lines;
            ComputeLines(maxWidth, lines);
            f32 widest = 0.0f;
            for (const core::StringView& line : lines)
            {
                const f32 w = m_font->font->MeasureString(line);
                if (w > widest)
                    widest = w;
            }
            return core::Float2{widest, static_cast<f32>(lines.Size()) * GetLineHeight()};
        }

        // The top-left draw position for the current alignment within `bounds`.
        [[nodiscard]] core::Float2 AlignedPosition(const Rect& bounds) const
        {
            const f32 w = GetWidth();
            const f32 h = GetLineHeight();
            f32 x = bounds.Left();
            f32 y = bounds.Top();
            if (m_hAlign == TextHAlign::Center)
                x += (bounds.width - w) * 0.5f;
            else if (m_hAlign == TextHAlign::Right)
                x += (bounds.width - w);
            if (m_vAlign == TextVAlign::Middle)
                y += (bounds.height - h) * 0.5f;
            else if (m_vAlign == TextVAlign::Bottom)
                y += (bounds.height - h);
            return core::Float2{x, y};
        }

        // Where Draw(ctx, bounds) puts the first line's baseline: the line's top plus the ascent,
        // or for bottom alignment of one line, the descent (negative) above the bottom.
        [[nodiscard]] f32 FirstBaseline(const Rect& bounds) const
        {
            if (!HasFont())
                return bounds.Top() + bounds.height;
            const fonts::FontMetrics fm = m_font->font->Metrics();
            if (!m_wordWrap || IsEmpty())
            {
                if (m_vAlign == TextVAlign::Middle)
                    return bounds.Top() + (bounds.height - fm.lineHeight) * 0.5f + fm.ascent;
                if (m_vAlign == TextVAlign::Bottom)
                    return bounds.Top() + bounds.height + fm.descent;
                return bounds.Top() + fm.ascent;
            }
            core::Array<core::StringView> lines;
            ComputeLines(bounds.width, lines);
            const f32 blockH = static_cast<f32>(lines.Size()) * GetLineHeight();
            f32 top = bounds.Top();
            if (m_vAlign == TextVAlign::Middle)
                top += (bounds.height - blockH) * 0.5f;
            else if (m_vAlign == TextVAlign::Bottom)
                top += bounds.height - blockH;
            return top + fm.ascent;
        }

        // === Drawing ===
        // VG's point DrawText anchors at the text baseline; `position` here is the top-left,
        // so we offset down by the font ascent.
        void Draw(DrawContext& ctx, core::Float2 position) const
        {
            if (m_font == nullptr || IsEmpty())
                return;
            const f32 ascent = HasFont() ? m_font->font->Metrics().ascent : 0.0f;
            ctx.VG().DrawText(m_string.AsView(), m_font,
                              core::Float2{position.x, position.y + ascent}, m_color);
        }
        // Draw aligned within bounds. Single line delegates to VG's alignment-aware overload;
        // word-wrap lays out multiple lines (each H-aligned, the block V-aligned).
        void Draw(DrawContext& ctx, const Rect& bounds) const
        {
            if (m_font == nullptr || IsEmpty())
                return;
            if (!m_wordWrap)
            {
                ctx.VG().DrawText(m_string.AsView(), m_font, bounds.ToRectangle(),
                                  ToFontsHAlign(m_hAlign), ToFontsVAlign(m_vAlign), m_color);
                return;
            }

            core::Array<core::StringView> lines;
            ComputeLines(bounds.width, lines);
            const f32 lineHeight = GetLineHeight();
            const f32 ascent = m_font->font->Metrics().ascent;
            const f32 blockH = static_cast<f32>(lines.Size()) * lineHeight;

            f32 top = bounds.Top();
            if (m_vAlign == TextVAlign::Middle)
                top += (bounds.height - blockH) * 0.5f;
            else if (m_vAlign == TextVAlign::Bottom)
                top += (bounds.height - blockH);

            for (usize k = 0; k < lines.Size(); ++k)
            {
                const core::StringView line = lines[k];
                if (line.Size() == 0)
                    continue;
                const f32 lineW = m_font->font->MeasureString(line);
                f32 x = bounds.Left();
                if (m_hAlign == TextHAlign::Center)
                    x += (bounds.width - lineW) * 0.5f;
                else if (m_hAlign == TextHAlign::Right)
                    x += (bounds.width - lineW);
                const f32 y = top + static_cast<f32>(k) * lineHeight + ascent;
                ctx.VG().DrawText(line, m_font, core::Float2{x, y}, m_color);
            }
        }

    private:
        [[nodiscard]] bool HasFont() const noexcept
        {
            return m_font != nullptr && m_font->font != nullptr;
        }

        [[nodiscard]] static fonts::TextAlignment ToFontsHAlign(TextHAlign h) noexcept
        {
            switch (h)
            {
            case TextHAlign::Center:
                return fonts::TextAlignment::Center;
            case TextHAlign::Right:
                return fonts::TextAlignment::Right;
            default:
                return fonts::TextAlignment::Left;
            }
        }
        [[nodiscard]] static fonts::VerticalAlignment ToFontsVAlign(TextVAlign v) noexcept
        {
            switch (v)
            {
            case TextVAlign::Middle:
                return fonts::VerticalAlignment::Middle;
            case TextVAlign::Bottom:
                return fonts::VerticalAlignment::Bottom;
            default:
                return fonts::VerticalAlignment::Top;
            }
        }

        core::String m_string;
        fonts::CachedFont* m_font = nullptr; // non-owning (owned by the font service)
        Color m_color = Color::White;
        TextHAlign m_hAlign = TextHAlign::Left;
        TextVAlign m_vAlign = TextVAlign::Top;
        bool m_wordWrap = false;
    };
}
