// Draconic UI Toolkit - :dock_split partition
//
// Binary split node for the dock tree: two children separated by a draggable divider. Direct ViewGroup,
// no SplitView wrapper. Ported from Sedulous.UI.Toolkit/src/Docking/DockSplit.bf. Uses only
// View/ViewGroup/Orientation/Rectangle, so it is a clean leaf with no docking-type deps. Beef
// `Vector2` -> Float2; `RectangleF` -> Rectangle (fields lowercase x/y/width/height); the `Orientation()`
// getter hides the `Orientation` enum inside the class, so type positions are fully qualified.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:dock_split;

import draconic.foundation;
import draconic.vg;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Binary split node for the dock tree.
    class DockSplit : public ViewGroup
    {
        DRACONIC_OBJECT(DockSplit, ViewGroup)
    public:
        explicit DockSplit(
            ::draconic::ui::Orientation orientation = ::draconic::ui::Orientation::Horizontal)
            : m_orientation(orientation)
        {
        }

        [[nodiscard]] ::draconic::ui::Orientation Orientation() const { return m_orientation; }
        void SetOrientation(::draconic::ui::Orientation value)
        {
            m_orientation = value;
            Invalidate();
        }

        [[nodiscard]] f32 SplitRatio() const { return m_splitRatio; }
        void SetSplitRatio(f32 value)
        {
            m_splitRatio = Clamp(value, 0.05f, 0.95f);
            Invalidate();
        }

        [[nodiscard]] f32 DividerSize() const { return m_dividerSize; }
        void SetDividerSize(f32 value)
        {
            m_dividerSize = Max(2.0f, value);
            Invalidate();
        }

        [[nodiscard]] f32 MinPaneSize() const { return m_minPaneSize; }
        void SetMinPaneSize(f32 value) { m_minPaneSize = Max(10.0f, value); }

        /// First child (left or top).
        [[nodiscard]] View* First() const { return (ChildCount() > 0) ? GetChildAt(0) : nullptr; }

        /// Second child (right or bottom).
        [[nodiscard]] View* Second() const { return (ChildCount() > 1) ? GetChildAt(1) : nullptr; }

        /// Set both children. Removes existing children first.
        void SetChildren(View* first, View* second)
        {
            RemoveAllViews();
            if (first != nullptr)
            {
                AddView(first);
            }
            if (second != nullptr)
            {
                AddView(second);
            }
            Invalidate();
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            DrawChildren(ctx);

            // Draw divider.
            const Rectangle dividerRect = GetDividerRect();
            const Color dividerColor =
                (m_isDragging || m_isDividerHovered)
                    ? ResolveStyleColor(StyleProperty::AccentColor, Rgb(80, 150, 240, 255))
                    : ResolveStyleColor(StyleProperty::BorderColor, Rgb(65, 70, 85, 255));
            ctx.VG().FillRect(dividerRect, dividerColor);
        }

        // === Hit testing: intercept divider clicks ===

        [[nodiscard]] View* HitTest(Float2 localPoint) override
        {
            if (!IsInteractionEnabled || Visibility != VisibilityValue::Visible)
            {
                return nullptr;
            }
            if (localPoint.x < 0 || localPoint.y < 0 || localPoint.x >= Width() ||
                localPoint.y >= Height())
            {
                return nullptr;
            }

            // Check divider first.
            const Rectangle dividerRect = GetDividerRect();
            if (localPoint.x >= dividerRect.x && localPoint.x < dividerRect.x + dividerRect.width &&
                localPoint.y >= dividerRect.y && localPoint.y < dividerRect.y + dividerRect.height)
            {
                return this;
            }

            // Test children in reverse order.
            if (View* second = Second())
            {
                const Float2 childLocal{localPoint.x - second->Bounds.x,
                                        localPoint.y - second->Bounds.y};
                if (View* hit = second->HitTest(childLocal))
                {
                    return hit;
                }
            }
            if (View* first = First())
            {
                const Float2 childLocal{localPoint.x - first->Bounds.x,
                                        localPoint.y - first->Bounds.y};
                if (View* hit = first->HitTest(childLocal))
                {
                    return hit;
                }
            }

            return this;
        }

        // === Input ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }

            const Rectangle dividerRect = GetDividerRect();
            if (e.X >= dividerRect.x && e.X < dividerRect.x + dividerRect.width &&
                e.Y >= dividerRect.y && e.Y < dividerRect.y + dividerRect.height)
            {
                m_isDragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_isDragging)
            {
                UpdateSplitFromMouse(e.X, e.Y);
            }
            else
            {
                const Rectangle dividerRect = GetDividerRect();
                const bool overDivider =
                    e.X >= dividerRect.x && e.X < dividerRect.x + dividerRect.width &&
                    e.Y >= dividerRect.y && e.Y < dividerRect.y + dividerRect.height;

                if (overDivider != m_isDividerHovered)
                {
                    m_isDividerHovered = overDivider;
                    if (overDivider)
                    {
                        Cursor = (m_orientation == ::draconic::ui::Orientation::Horizontal)
                                     ? CursorType::SizeWE
                                     : CursorType::SizeNS;
                    }
                    else
                    {
                        Cursor = CursorType::Default;
                    }
                }
            }
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (m_isDragging && e.Button == MouseButton::Left)
            {
                m_isDragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }

        void OnMouseLeave() override
        {
            if (m_isDividerHovered)
            {
                m_isDividerHovered = false;
                Cursor = CursorType::Default;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 w = constraints.ConstrainWidth(200);
            const f32 h = constraints.ConstrainHeight(200);

            if (m_orientation == ::draconic::ui::Orientation::Horizontal)
            {
                const f32 available = w - m_dividerSize;
                const f32 firstW = available * m_splitRatio;
                const f32 secondW = available - firstW;
                if (View* first = First())
                {
                    first->Measure(BoxConstraints::Tight(firstW, h));
                }
                if (View* second = Second())
                {
                    second->Measure(BoxConstraints::Tight(secondW, h));
                }
            }
            else
            {
                const f32 available = h - m_dividerSize;
                const f32 firstH = available * m_splitRatio;
                const f32 secondH = available - firstH;
                if (View* first = First())
                {
                    first->Measure(BoxConstraints::Tight(w, firstH));
                }
                if (View* second = Second())
                {
                    second->Measure(BoxConstraints::Tight(w, secondH));
                }
            }

            MeasuredSize = Float2{w, h};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (m_orientation == ::draconic::ui::Orientation::Horizontal)
            {
                const f32 available = width - m_dividerSize;
                const f32 firstW = available * m_splitRatio;
                const f32 secondW = available - firstW;
                if (View* first = First())
                {
                    first->Layout(0, 0, firstW, height);
                }
                if (View* second = Second())
                {
                    second->Layout(firstW + m_dividerSize, 0, secondW, height);
                }
            }
            else
            {
                const f32 available = height - m_dividerSize;
                const f32 firstH = available * m_splitRatio;
                const f32 secondH = available - firstH;
                if (View* first = First())
                {
                    first->Layout(0, 0, width, firstH);
                }
                if (View* second = Second())
                {
                    second->Layout(0, firstH + m_dividerSize, width, secondH);
                }
            }
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] Rectangle GetDividerRect() const
        {
            if (m_orientation == ::draconic::ui::Orientation::Horizontal)
            {
                const f32 available = Width() - m_dividerSize;
                const f32 firstW = available * m_splitRatio;
                return Rectangle{firstW, 0, m_dividerSize, Height()};
            }
            else
            {
                const f32 available = Height() - m_dividerSize;
                const f32 firstH = available * m_splitRatio;
                return Rectangle{0, firstH, Width(), m_dividerSize};
            }
        }

        void UpdateSplitFromMouse(f32 localX, f32 localY)
        {
            f32 ratio;
            if (m_orientation == ::draconic::ui::Orientation::Horizontal)
            {
                const f32 available = Width() - m_dividerSize;
                if (available <= 0)
                {
                    return;
                }
                ratio = (localX - m_dividerSize * 0.5f) / available;
            }
            else
            {
                const f32 available = Height() - m_dividerSize;
                if (available <= 0)
                {
                    return;
                }
                ratio = (localY - m_dividerSize * 0.5f) / available;
            }
            SetSplitRatio(ratio);
        }

        ::draconic::ui::Orientation m_orientation = ::draconic::ui::Orientation::Horizontal;
        f32 m_splitRatio = 0.5f;
        f32 m_dividerSize = 4;
        f32 m_minPaneSize = 50;
        bool m_isDragging = false;
        bool m_isDividerHovered = false;
    };

    DRACONIC_DEFINE_OBJECT(DockSplit, "draconic::ui::toolkit")
}
