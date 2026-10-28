// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - module implementation unit for UIDebugOverlay::DrawOverlays.
//
// Holds the body (it reaches into the full View/ViewGroup cluster: Cast<ViewGroup>, view.Width()/
// IsHovered()/IsFocused()/Layout()), so :ui_debug_overlay stays a thin declaration :view can call.
// Ported from Sedulous.UI/src/Debug/UIDebugOverlay.bf; Beef `Color(r,g,b,a)` byte literals -> the local
// C() helper; `view as ViewGroup` -> Cast<ViewGroup>.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h" // Cast

module foundation.ui;

using namespace foundation::core;

namespace foundation::ui
{
    namespace
    {
        [[nodiscard]] constexpr Color C(f32 r, f32 g, f32 b, f32 a)
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        const Color sBoundsColor = C(255, 60, 60, 180);
        const Color sPaddingColor = C(60, 200, 60, 60);
        const Color sMarginColor = C(255, 160, 40, 60);
        const Color sHitTargetColor = C(255, 255, 0, 100);
        const Color sFocusColor = C(80, 160, 255, 200);
        const Color sZOrderColor = C(230, 120, 255, 230);
        const Color sBadgeColor = C(10, 12, 16, 210);

        // A number drawn as seven-segment strokes: an overlay needs no font, so it works in any
        // GUI, whatever its fonts. Each digit is 5 x 9 at (x, y), with 2 between digits.
        void DrawNumber(UIDrawContext& ctx, usize number, f32 x, f32 y, Color color)
        {
            // Segments a (top), b (top right), c (bottom right), d (bottom), e (bottom left),
            // f (top left), g (middle), as bits 0..6.
            static constexpr u8 kDigits[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
            char text[24];
            usize length = 0;
            do
            {
                text[length++] = static_cast<char>(u8'0' + number % 10);
                number /= 10;
            } while (number != 0 && length < sizeof(text));
            constexpr f32 w = 5.0f, h = 9.0f, half = 4.5f, advance = 7.0f, stroke = 1.4f;
            for (usize i = 0; i < length; ++i)
            {
                const u8 segments = kDigits[text[length - 1 - i] - '0'];
                const f32 dx = x + static_cast<f32>(i) * advance;
                const auto line = [&](f32 x0, f32 y0, f32 x1, f32 y1)
                { ctx.VG().DrawLine(Float2{dx + x0, y + y0}, Float2{dx + x1, y + y1}, color, stroke); };
                if (segments & 0x01) line(0, 0, w, 0);
                if (segments & 0x02) line(w, 0, w, half);
                if (segments & 0x04) line(w, half, w, h);
                if (segments & 0x08) line(0, h, w, h);
                if (segments & 0x10) line(0, half, 0, h);
                if (segments & 0x20) line(0, 0, 0, half);
                if (segments & 0x40) line(0, half, w, half);
            }
        }

        // A number on a dark badge at (x, y), wide enough for its digits.
        void DrawBadge(UIDrawContext& ctx, usize number, f32 x, f32 y, Color color)
        {
            usize digits = 1;
            for (usize n = number; n >= 10; n /= 10)
            {
                ++digits;
            }
            ctx.VG().FillRect(Rectangle{x, y, 4.0f + 7.0f * static_cast<f32>(digits), 13.0f}, sBadgeColor);
            DrawNumber(ctx, number, x + 2.0f, y + 2.0f, color);
        }
    }

    void UIDebugOverlay::DrawOverlays(UIDrawContext& ctx, View& view, usize drawIndex)
    {
        const UIDebugDrawSettings& settings = ctx.DebugSettings();
        const f32 w = view.Width();
        const f32 h = view.Height();

        // Padding (green interior bands).
        if (settings.ShowPadding)
        {
            {
                // The padding as layout uses it: the sheet's, the field's and the background's.
                const Thickness pad = view.ResolveBoxMetrics().Padding;
                if (!pad.IsZero())
                {
                    ctx.VG().FillRect(Rectangle{0, 0, w, pad.Top}, sPaddingColor);
                    ctx.VG().FillRect(Rectangle{0, h - pad.Bottom, w, pad.Bottom}, sPaddingColor);
                    ctx.VG().FillRect(Rectangle{0, pad.Top, pad.Left, h - pad.Top - pad.Bottom},
                                      sPaddingColor);
                    ctx.VG().FillRect(
                        Rectangle{w - pad.Right, pad.Top, pad.Right, h - pad.Top - pad.Bottom},
                        sPaddingColor);
                }
            }
        }

        // Margin (orange exterior bands).
        if (settings.ShowMargin)
        {
            if (!view.Layout().Margin->IsZero())
            {
                const Thickness m = view.Layout().Margin;
                ctx.VG().FillRect(Rectangle{-m.Left, -m.Top, w + m.TotalHorizontal(), m.Top},
                                  sMarginColor);
                ctx.VG().FillRect(Rectangle{-m.Left, h, w + m.TotalHorizontal(), m.Bottom},
                                  sMarginColor);
                ctx.VG().FillRect(Rectangle{-m.Left, 0, m.Left, h}, sMarginColor);
                ctx.VG().FillRect(Rectangle{w, 0, m.Right, h}, sMarginColor);
            }
        }

        // Bounds (red outline).
        if (settings.ShowBounds)
        {
            ctx.VG().StrokeRect(Rectangle{0, 0, w, h}, sBoundsColor, 1.0f);
        }

        // Hit target highlight (yellow fill on the hovered view).
        if (settings.ShowHitTarget)
        {
            if (view.IsHovered())
            {
                ctx.VG().FillRect(Rectangle{0, 0, w, h}, sHitTargetColor);
            }
        }

        // Draw order within the parent (violet number at the top right).
        if (settings.ShowZOrder)
        {
            DrawBadge(ctx, drawIndex, Max(0.0f, w - 18.0f), 0.0f, sZOrderColor);
        }

        // Tab order (blue number at the top left of each stop, and a line on to the next stop).
        if (settings.ShowTabOrder && view.IsFocusable && view.Context != nullptr)
        {
            Array<View*> stops;
            view.Context->GetFocusManager()->CollectTabStops(stops);
            for (usize i = 0; i < stops.Size(); ++i)
            {
                if (stops[i] != &view)
                {
                    continue;
                }
                DrawBadge(ctx, i + 1, 0.0f, 0.0f, sFocusColor);
                if (i + 1 < stops.Size())
                {
                    View* next = stops[i + 1];
                    const Float2 to = view.ScreenToLocal(next->LocalToScreen(Float2{next->Width() * 0.5f, next->Height() * 0.5f}));
                    ctx.VG().DrawLine(Float2{w * 0.5f, h * 0.5f}, to,
                                      Color{sFocusColor.r, sFocusColor.g, sFocusColor.b, 0.5f}, 1.0f);
                }
                break;
            }
        }

        // Focus path (blue outline on the focused view + ancestors with focus-within).
        if (settings.ShowFocusPath)
        {
            if (view.IsFocused())
            {
                ctx.VG().StrokeRect(Rectangle{-2, -2, w + 4, h + 4}, sFocusColor, 2.0f);
            }
            else if (view.IsFocusWithin())
            {
                ctx.VG().StrokeRect(
                    Rectangle{-1, -1, w + 2, h + 2},
                    Color{sFocusColor.r, sFocusColor.g, sFocusColor.b, 80.0f / 255.0f}, 1.0f);
            }
        }
    }
}
