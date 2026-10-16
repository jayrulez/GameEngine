// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :linear_layout partition
//
// LinearLayout: stacks its children in a row or column with spacing, starting from the
// padding-inset content bounds. Modeled on eepp's UILinearLayout (role, not a line-for-line
// port). Re-runs after a size change or a child added or removed (it requests its layout, which
// its SceneRoot's layout pass runs once a frame; at once when detached).
// Children keep their own sizes here; measurement (wrap-content), weights/stretch, and
// gravity are not implemented.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:linear_layout;

import foundation.core; // Float2, Max
import :rect;
import :thickness;
import :node;
import :ui_widget;
import :css_values; // ParseLength (markup spacing)

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class Orientation
    {
        Horizontal,
        Vertical
    };

    class LinearLayout : public UIWidget
    {
        RTTI_OBJECT(LinearLayout, UIWidget)
    public:
        LinearLayout() = default;

        void SetOrientation(Orientation orientation)
        {
            m_orientation = orientation;
            RequestLayout();
        }
        [[nodiscard]] Orientation GetOrientation() const noexcept { return m_orientation; }

        void SetSpacing(f32 spacing)
        {
            m_spacing = spacing;
            RequestLayout();
        }
        [[nodiscard]] f32 GetSpacing() const noexcept { return m_spacing; }

        // Wrap-content: when on, the layout resizes itself along its orientation to exactly fit
        // its stacked children (plus padding), leaving the cross axis untouched. Useful inside a
        // ScrollView with SetAutoMeasureContent so a tall stack scrolls. Off by default.
        void SetWrapContent(bool wrap)
        {
            m_wrapContent = wrap;
            RequestLayout();
        }
        [[nodiscard]] bool IsWrapContent() const noexcept { return m_wrapContent; }

        // Markup: <LinearLayout orientation="horizontal" spacing="8" wrap-content="true">.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"orientation"))
            {
                SetOrientation(value == core::StringView(u8"horizontal") ? Orientation::Horizontal
                                                                         : Orientation::Vertical);
                return true;
            }
            if (name == core::StringView(u8"spacing"))
            {
                if (Optional<f32> s = ParseLength(value); s.HasValue())
                    SetSpacing(s.Value());
                return true;
            }
            if (name == core::StringView(u8"wrap-content"))
            {
                SetWrapContent(value == core::StringView(u8"true"));
                return true;
            }
            return UIWidget::SetMarkupAttribute(name, value);
        }

        // Position children in sequence along the orientation, skipping hidden ones.
        void PerformLayout()
        {
            if (m_layingOut)
                return; // re-entrancy guard (a wrap-content SetSize re-enters)
            m_layingOut = true;

            const Rect content = GetContentBounds();
            f32 x = content.x;
            f32 y = content.y;
            f32 extent = 0.0f; // main-axis size of the stacked children (no trailing spacing)
            for (usize i = 0; i < ChildCount(); ++i)
            {
                Node* child = GetChildAt(i);
                if (child == nullptr || !child->IsVisible())
                    continue;

                child->SetPosition(core::Float2{x, y});
                if (m_orientation == Orientation::Vertical)
                {
                    const f32 h = child->GetSize().y;
                    y += h + m_spacing;
                    extent = (y - content.y) - m_spacing;
                }
                else
                {
                    const f32 w = child->GetSize().x;
                    x += w + m_spacing;
                    extent = (x - content.x) - m_spacing;
                }
            }
            m_layingOut = false;

            if (m_wrapContent)
            {
                const Thickness p = GetPadding();
                if (m_orientation == Orientation::Vertical)
                    SetSize(core::Float2{GetSize().x, core::Max(0.0f, extent) + p.TotalVertical()});
                else
                    SetSize(
                        core::Float2{core::Max(0.0f, extent) + p.TotalHorizontal(), GetSize().y});
            }
        }

    protected:
        void OnLayout() override { PerformLayout(); } // the SceneRoot's pass, or at once when detached
        void OnChildLayoutChange(Node&, bool) override { RequestLayout(); }
        void OnSizeChange() override { RequestLayout(); }
        void OnChildrenChanged() override { RequestLayout(); }

        Orientation m_orientation = Orientation::Vertical;
        f32 m_spacing = 0.0f;
        bool m_wrapContent = false;
        bool m_layingOut = false;
    };
}
