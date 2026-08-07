// Draconic UI - module implementation unit for UIDebugOverlay::DrawOverlays.
//
// Holds the body (it reaches into the full View/ViewGroup cluster: Cast<ViewGroup>, view.Width()/
// IsHovered()/IsFocused()/LayoutParams), so :ui_debug_overlay stays a thin declaration :view can call.
// Ported from Sedulous.UI/src/Debug/UIDebugOverlay.bf; Beef `Color(r,g,b,a)` byte literals -> the local
// C() helper; `view as ViewGroup` -> Cast<ViewGroup>.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h" // Cast

module draconic.ui;

using namespace draconic::foundation;

namespace draconic::ui
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
    }

    void UIDebugOverlay::DrawOverlays(UIDrawContext& ctx, View& view)
    {
        const UIDebugDrawSettings& settings = ctx.DebugSettings();
        const f32 w = view.Width();
        const f32 h = view.Height();

        // Padding (green interior bands).
        if (settings.ShowPadding)
        {
            if (ViewGroup* vg = Cast<ViewGroup>(&view))
            {
                const Thickness pad = vg->Padding;
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
            if (LayoutParams* lp = view.LayoutParams.Get(); lp != nullptr && !lp->Margin.IsZero())
            {
                const Thickness m = lp->Margin;
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
