// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :drag_adorner partition
//
// Visual overlay shown during a drag operation. Wraps a user-provided visual or shows a default
// indicator. Shown via PopupLayer; the entire subtree is invisible to hit testing (IsInteractionEnabled
// = false) so the underlying drop target can be found. Ported from Sedulous.UI/src/DragDrop/DragAdorner.bf.
// The visual is owned by the ViewGroup tree (AddView RefPtr).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:drag_adorner;

import foundation.core;
import foundation.vg;
import :view;
import :style_property;
import :box_constraints;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class DragAdorner : public ViewGroup
    {
        RTTI_OBJECT(DragAdorner, ViewGroup)
    public:
        DragAdorner(View* visual, f32 offsetX, f32 offsetY) : m_offsetX(offsetX), m_offsetY(offsetY)
        {
            IsInteractionEnabled = false;
            Opacity = 0.7f;
            if (visual != nullptr)
            {
                AddView(visual);
            }
        }

        /// Offset from the cursor position.
        [[nodiscard]] f32 OffsetX() const noexcept { return m_offsetX; }
        [[nodiscard]] f32 OffsetY() const noexcept { return m_offsetY; }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (ChildCount() > 0)
            {
                ViewGroup::OnDraw(ctx);
            }
            else
            {
                // Default ghost: themeable via a background-color rule.
                ctx.VG().FillRoundedRect(
                    Rectangle{0, 0, Width(), Height()}, 4.0f,
                    ResolveStyleColor(StyleProperty::Background,
                                      Color{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f,
                                            128.0f / 255.0f}));
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            if (ChildCount() > 0)
            {
                // Equivalent of Sedulous ViewGroup.OnMeasure (max-of-children): our ViewGroup
                // does not port that default (all concrete containers override), so replicate it here.
                const BoxConstraints inner = constraints.Deflate(Padding);
                f32 maxW = 0, maxH = 0;
                for (usize i = 0; i < ChildCount(); ++i)
                {
                    View* child = GetChildAt(i);
                    if (child->Visibility == VisibilityValue::Gone)
                    {
                        continue;
                    }
                    child->Measure(inner);
                    maxW = Max(maxW, child->MeasuredSize.x);
                    maxH = Max(maxH, child->MeasuredSize.y);
                }
                MeasuredSize =
                    Float2{constraints.ConstrainWidth(maxW + Padding.Left + Padding.Right),
                           constraints.ConstrainHeight(maxH + Padding.Top + Padding.Bottom)};
            }
            else
            {
                // Default size when no visual provided.
                MeasuredSize =
                    Float2{constraints.ConstrainWidth(32), constraints.ConstrainHeight(32)};
            }
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            // Layout children to fill the adorner bounds.
            for (usize i = 0; i < ChildCount(); ++i)
            {
                if (View* child = GetChildAt(i))
                {
                    child->Layout(0, 0, width, height);
                }
            }
        }

    private:
        f32 m_offsetX;
        f32 m_offsetY;
    };

    RTTI_DEFINE_OBJECT(DragAdorner, "rtti::ui")
}
