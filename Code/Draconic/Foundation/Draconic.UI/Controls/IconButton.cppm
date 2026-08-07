// Draconic UI - :icon_button partition
//
// A small clickable button that draws an SVG icon centered over the themed button background, at a
// fixed square size. For toolbars, list-row actions (add / remove / reorder), and any icon-only
// affordance. Themable like the core controls: the background + pressed/hover/disabled states come
// from the style (DrawButtonBackground), the icon inset from the Padding style, and the icon tint
// from the TextColor style (dimmed when disabled), the same foreground the glyph controls use. The
// icon drawable is borrowed (typically owned by an icon set that outlives the button).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:icon_button;

import draconic.foundation;
import :button_base;
import :view;
import :control_state;
import :style_property;
import :thickness;
import :box_constraints;
import :draw_context;
import :svg_drawable;
import :palette;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class IconButton : public ButtonBase
    {
        DRACONIC_OBJECT(IconButton, ButtonBase)
    public:
        explicit IconButton(SVGDrawable* icon, f32 size = 20.0f) : m_icon(icon), m_size(size) {}

        void SetIcon(SVGDrawable* icon)
        {
            m_icon = icon;
            Invalidate();
        }
        void SetSize(f32 size)
        {
            m_size = size;
            Invalidate();
        }
        [[nodiscard]] f32 Size() const noexcept { return m_size; }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(m_size), constraints.ConstrainHeight(m_size)};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const ControlState state = GetControlState();
            DrawButtonBackground(ctx, bounds, state);
            if (m_icon == nullptr)
            {
                return;
            }
            const Thickness pad = ResolveStyleThickness(StyleProperty::Padding, Thickness{3, 3});
            Color tint = ResolveStyleColor(
                StyleProperty::TextColor,
                Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
            if (HasFlag(state, ControlState::Disabled))
            {
                tint = Palette::ComputeDisabled(tint);
            }
            const Optional<Color> previous = m_icon->TintColor;
            m_icon->TintColor = Optional<Color>(tint);
            m_icon->Draw(ctx, Rectangle{pad.Left, pad.Top, Width() - pad.TotalHorizontal(),
                                        Height() - pad.TotalVertical()});
            m_icon->TintColor = previous; // restore (the drawable is shared with the icon set)
        }

    private:
        SVGDrawable* m_icon = nullptr; // borrowed (owned by the icon set; outlives the button)
        f32 m_size = 20.0f;
    };

    DRACONIC_DEFINE_OBJECT(IconButton, "draconic::ui")
}
