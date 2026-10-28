// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :breadcrumb_bar partition
//
// Horizontal path display with clickable segments and separator arrows. Ported from
// Sedulous.UI.Toolkit/src/BreadcrumbBar.bf. Beef `List<String>` owned -> Array<String>;
// `path.Split(sep)` + Trim -> a manual char-scan split using core::Trim; `Event<delegate void(
// BreadcrumbBar, int32)>` -> Event<void(BreadcrumbBar*, i32)>; `Context.InputManager.MouseX` ->
// Context->GetInputManager()->MouseX(); `bg as RoundedRectDrawable` -> Cast<RoundedRectDrawable>(bg).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:breadcrumb_bar;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui::toolkit
{
    /// Horizontal path display with clickable segments and separator arrows.
    /// Used for file path navigation, hierarchy display, etc.
    class BreadcrumbBar : public ViewGroup
    {
        RTTI_OBJECT(BreadcrumbBar, ViewGroup)
    public:
        /// Fired when a segment is clicked. Parameter: segment index.
        Event<void(BreadcrumbBar*, i32)> OnSegmentClicked;

        BreadcrumbBar() { Cursor = CursorType::Hand; }

        [[nodiscard]] i32 SegmentCount() const noexcept
        {
            return static_cast<i32>(m_segments.Size());
        }

        /// Set the path from a list of segments.
        void SetSegments(Span<StringView> segments)
        {
            m_segments.Clear();
            for (const StringView& s : segments)
            {
                m_segments.PushBack(String(s));
            }
            Invalidate();
        }

        /// Set the path from a separator-delimited string.
        void SetPath(StringView path, char8_t separator = u8'/')
        {
            m_segments.Clear();
            const usize n = path.Length();
            usize start = 0;
            for (usize i = 0; i <= n; ++i)
            {
                if (i == n || path[i] == separator)
                {
                    const StringView trimmed = Trim(path.SubStr(start, i - start));
                    if (!trimmed.IsEmpty())
                    {
                        m_segments.PushBack(String(trimmed));
                    }
                    start = i + 1;
                }
            }
            Invalidate();
        }

        /// Get the segment text at an index.
        [[nodiscard]] StringView GetSegment(i32 index) const
        {
            if (index < 0 || index >= static_cast<i32>(m_segments.Size()))
            {
                return StringView{};
            }
            return m_segments[static_cast<usize>(index)].AsView();
        }

        /// Get the full path up to and including the given segment index.
        void GetPathUpTo(i32 index, String& output, char8_t separator = u8'/') const
        {
            for (i32 i = 0; i <= index && i < static_cast<i32>(m_segments.Size()); ++i)
            {
                if (i > 0)
                {
                    output.Append(separator);
                }
                output.Append(m_segments[static_cast<usize>(i)].AsView());
            }
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 h = Height();

            // Background (rounded to the theme's CornerRadius when the drawable supports it).
            Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background);
            if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(bgDrawable))
            {
                const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);
                const foundation::vg::CornerRadii saved = rrd->Radii;
                rrd->Radii = foundation::vg::CornerRadii{cr, cr, cr, cr};
                rrd->Draw(ctx, Rectangle{0, 0, Width(), h});
                rrd->Radii = saved;
            }
            else if (bgDrawable != nullptr)
            {
                bgDrawable->Draw(ctx, Rectangle{0, 0, Width(), h});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), h}, Rgb(40, 42, 52, 255));
            }

            fonts::CachedFont* font =
                (ctx.FontService() != nullptr) ? ctx.FontService()->GetFont(ResolveStyleFloat(StyleProperty::FontSize, m_fontSize)) : nullptr;
            if (font == nullptr)
            {
                return;
            }

            const Color textColor =
                ResolveStyleColor(StyleProperty::TextColor, Rgb(220, 225, 235, 255));
            const Color hoverColor =
                ResolveStyleColor(StyleProperty::AccentColor, Rgb(80, 150, 240, 255));
            const Color lastColor =
                ResolveStyleColor(StyleProperty::TextDimColor, Rgb(180, 185, 200, 255));
            const Color sepColor =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(100, 105, 120, 255));

            for (usize i = 0; i < m_segments.Size(); ++i)
            {
                if (i >= m_segmentRects.Size())
                {
                    break;
                }
                const Rectangle rect = m_segmentRects[i];
                const bool isLast = i == m_segments.Size() - 1;
                const bool isHovered = static_cast<i32>(i) == m_hoveredIndex;

                // Hover highlight - derived from background.
                if (isHovered && !isLast)
                {
                    Color bgColor = Rgb(40, 42, 52, 255);
                    if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(bgDrawable))
                    {
                        bgColor = rrd->FillColor;
                    }
                    else if (ColorDrawable* cd = Cast<ColorDrawable>(bgDrawable))
                    {
                        bgColor = cd->Color;
                    }
                    const Color hc = Palette::ComputeHover(bgColor);
                    const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);
                    if (cr > 0.0f)
                    {
                        ctx.VG().FillRoundedRect(rect, cr, hc);
                    }
                    else
                    {
                        ctx.VG().FillRect(rect, hc);
                    }
                }

                // Segment text.
                const Color color = isHovered ? hoverColor : (isLast ? lastColor : textColor);
                ctx.VG().DrawText(m_segments[i], font, rect, fonts::TextAlignment::Center,
                                  fonts::VerticalAlignment::Middle, color);

                // Separator arrow after each segment except last.
                if (!isLast)
                {
                    const f32 sepX = rect.x + rect.width + 2.0f;
                    const f32 sepCY = h * 0.5f;
                    const f32 arrowSz = 4.0f;
                    ctx.VG().BeginPath();
                    ctx.VG().MoveTo(sepX, sepCY - arrowSz);
                    ctx.VG().LineTo(sepX + arrowSz, sepCY);
                    ctx.VG().LineTo(sepX, sepCY + arrowSz);
                    ctx.VG().Stroke(sepColor, 1.5f);
                }
            }
        }

        // === Input ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
            {
                return;
            }

            const Float2 local = MouseLocal();
            const i32 idx = GetSegmentAt(local.x, local.y);
            if (idx >= 0)
            {
                OnSegmentClicked.Invoke(this, idx);
                e.Handled = true;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            (void)e;
            const Float2 local = MouseLocal();
            const i32 idx = GetSegmentAt(local.x, local.y);
            if (idx != m_hoveredIndex)
            {
                m_hoveredIndex = idx;
                Invalidate();
            }
        }

        void OnMouseLeave() override
        {
            if (m_hoveredIndex >= 0)
            {
                m_hoveredIndex = -1;
                Invalidate();
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            f32 totalW = 0.0f;
            f32 textH = ResolveStyleFloat(StyleProperty::FontSize, m_fontSize);

            if (Context != nullptr && Context->FontService() != nullptr)
            {
                fonts::CachedFont* font = Context->FontService()->GetFont(ResolveStyleFloat(StyleProperty::FontSize, m_fontSize));
                if (font != nullptr)
                {
                    textH = font->font->Metrics().lineHeight;
                    for (usize i = 0; i < m_segments.Size(); ++i)
                    {
                        totalW +=
                            font->font->MeasureString(m_segments[i]) + m_segmentPadding * 2.0f;
                        if (i < m_segments.Size() - 1)
                        {
                            totalW += m_separatorWidth;
                        }
                    }
                }
            }

            MeasuredSize = Float2{constraints.ConstrainWidth(totalW),
                                  constraints.ConstrainHeight(textH + 8.0f)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            (void)width;
            (void)height;
            RebuildSegmentRects();
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] Float2 MouseLocal() const
        {
            const f32 screenX = (Context != nullptr) ? Context->GetInputManager()->MouseX() : 0.0f;
            const f32 screenY = (Context != nullptr) ? Context->GetInputManager()->MouseY() : 0.0f;
            return ScreenToLocal(Float2{screenX, screenY});
        }

        void RebuildSegmentRects()
        {
            m_segmentRects.Clear();
            fonts::CachedFont* font = (Context != nullptr && Context->FontService() != nullptr)
                                          ? Context->FontService()->GetFont(ResolveStyleFloat(StyleProperty::FontSize, m_fontSize))
                                          : nullptr;
            if (font == nullptr)
            {
                return;
            }

            f32 x = 0.0f;
            for (usize i = 0; i < m_segments.Size(); ++i)
            {
                const f32 textW = font->font->MeasureString(m_segments[i]);
                const f32 segW = textW + m_segmentPadding * 2.0f;
                m_segmentRects.PushBack(Rectangle{x, 0, segW, Height()});
                x += segW;
                if (i < m_segments.Size() - 1)
                {
                    x += m_separatorWidth;
                }
            }
        }

        [[nodiscard]] i32 GetSegmentAt(f32 x, f32 y) const
        {
            for (usize i = 0; i < m_segmentRects.Size(); ++i)
            {
                const Rectangle r = m_segmentRects[i];
                if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        Array<String> m_segments;
        Array<Rectangle> m_segmentRects;
        i32 m_hoveredIndex = -1;
        f32 m_fontSize = 13.0f;
        f32 m_segmentPadding = 8.0f;
        f32 m_separatorWidth = 16.0f;
    };

    RTTI_DEFINE_OBJECT(BreadcrumbBar, "rtti::ui::toolkit")
}
