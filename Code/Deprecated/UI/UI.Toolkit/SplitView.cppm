// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :split_view partition
//
// Resizable two-pane container with a draggable divider. Ported from Sedulous.UI.Toolkit/src/SplitView.bf.
// Panes are borrowed raw View* (the child tree owns the RefPtr). Beef `Math.Clamp` -> core::Clamp;
// `Context.FocusManager` -> Context->GetFocusManager(); mouse capture via Set/ReleaseCapture. NOTE: the
// public field `Orientation` shadows the enum type of the same name inside the class, so the enum is
// spelled fully-qualified (foundation::ui::Orientation) wherever the type/enumerators are named.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:split_view;

import foundation.core;
import foundation.vg;
import foundation.ui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Which pane of a SplitView an operation targets.
    enum class SplitPane : u8
    {
        First,
        Second
    };

    /// Resizable two-pane container with a draggable divider.
    class SplitView : public ViewGroup
    {
        RTTI_OBJECT(SplitView, ViewGroup)
    public:
        ::foundation::ui::Orientation Orientation = ::foundation::ui::Orientation::Horizontal;
        f32 MinPaneSize = 50.0f;

        /// Width/height of the draggable divider area.
        f32 DividerSize = 6.0f;

        Event<void(SplitView*, f32)> OnSplitChanged;

        explicit SplitView(
            ::foundation::ui::Orientation orientation = ::foundation::ui::Orientation::Horizontal)
        {
            Orientation = orientation;
        }

        /// Split ratio (0..1). 0 = first pane collapsed, 1 = second pane collapsed.
        [[nodiscard]] f32 SplitRatio() const noexcept { return m_splitRatio; }

        void SetSplitRatio(f32 value)
        {
            const f32 clamped = core::Clamp(value, 0.0f, 1.0f);
            if (m_splitRatio != clamped)
            {
                m_splitRatio = clamped;
                Invalidate();
                OnSplitChanged.Invoke(this, clamped);
            }
        }

        /// Set the two panes. SplitView takes a ref via AddView.
        void SetPanes(View* first, View* second)
        {
            if (m_first != nullptr)
            {
                RemoveView(m_first, true);
            }
            if (m_second != nullptr)
            {
                RemoveView(m_second, true);
            }

            m_first = first;
            m_second = second;

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

        [[nodiscard]] View* FirstPane() const noexcept { return m_first; }
        [[nodiscard]] View* SecondPane() const noexcept { return m_second; }

        /// Collapse a pane to its content's minimum along the split axis: the other pane fills the rest,
        /// the divider hides and stops hit-testing, and the split RATIO is preserved for restore (the
        /// Godot bottom-dock: expand/collapse the dopesheet without losing the user's size). Layout-only
        /// - the pane view is never reparented or destroyed, so borrowed pointers stay valid.
        void SetPaneCollapsed(SplitPane pane, bool collapsed)
        {
            bool& flag = (pane == SplitPane::First) ? m_firstCollapsed : m_secondCollapsed;
            if (flag == collapsed)
            {
                return;
            }
            flag = collapsed;
            Invalidate();
        }
        [[nodiscard]] bool IsPaneCollapsed(SplitPane pane) const noexcept
        {
            return (pane == SplitPane::First) ? m_firstCollapsed : m_secondCollapsed;
        }
        [[nodiscard]] bool AnyPaneCollapsed() const noexcept
        {
            return m_firstCollapsed || m_secondCollapsed;
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            DrawChildren(ctx);

            if (AnyPaneCollapsed())
            {
                return; // a collapsed split shows no divider (nothing to drag)
            }

            // Draw divider.
            const Rectangle divRect = GetDividerRect();
            const Color divColor =
                (m_dividerHovered || m_dragging)
                    ? ResolveStyleColor(StyleProperty::AccentColor, Rgb(80, 85, 105, 255))
                    : ResolveStyleColor(StyleProperty::BorderColor, Rgb(55, 58, 70, 255));
            ctx.VG().FillRect(divRect, divColor);

            // Grip indicator in divider center.
            const Color gripColor =
                ResolveStyleColor(StyleProperty::TextDimColor, Rgb(100, 105, 120, 180));
            const f32 cx = divRect.x + divRect.width * 0.5f;
            const f32 cy = divRect.y + divRect.height * 0.5f;
            const f32 dotR = 1.5f;

            if (Orientation == ::foundation::ui::Orientation::Horizontal)
            {
                for (i32 i = -2; i <= 2; ++i)
                {
                    ctx.VG().FillCircle(Float2{cx, cy + i * 5.0f}, dotR, gripColor);
                }
            }
            else
            {
                for (i32 i = -2; i <= 2; ++i)
                {
                    ctx.VG().FillCircle(Float2{cx + i * 5.0f, cy}, dotR, gripColor);
                }
            }
        }

        // === Input ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }

            if (IsInDivider(e.X, e.Y))
            {
                m_dragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_dragging)
            {
                const f32 divSize = DividerSize;
                if (Orientation == ::foundation::ui::Orientation::Horizontal)
                {
                    const f32 available = Width() - divSize;
                    if (available > 0.0f)
                    {
                        const f32 minRatio =
                            (available > MinPaneSize * 2.0f) ? MinPaneSize / available : 0.0f;
                        const f32 maxRatio = (available > MinPaneSize * 2.0f)
                                                 ? 1.0f - MinPaneSize / available
                                                 : 1.0f;
                        SetSplitRatio(
                            core::Clamp((e.X - divSize * 0.5f) / available, minRatio, maxRatio));
                    }
                }
                else
                {
                    const f32 available = Height() - divSize;
                    if (available > 0.0f)
                    {
                        const f32 minRatio =
                            (available > MinPaneSize * 2.0f) ? MinPaneSize / available : 0.0f;
                        const f32 maxRatio = (available > MinPaneSize * 2.0f)
                                                 ? 1.0f - MinPaneSize / available
                                                 : 1.0f;
                        SetSplitRatio(
                            core::Clamp((e.Y - divSize * 0.5f) / available, minRatio, maxRatio));
                    }
                }
            }
            else
            {
                const bool wasHovered = m_dividerHovered;
                m_dividerHovered = IsInDivider(e.X, e.Y);
                if (m_dividerHovered != wasHovered)
                {
                    if (m_dividerHovered)
                    {
                        Cursor = (Orientation == ::foundation::ui::Orientation::Horizontal)
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
            if (e.Button != MouseButton::Left || !m_dragging)
            {
                return;
            }
            m_dragging = false;
            if (Context != nullptr)
            {
                Context->GetFocusManager()->ReleaseCapture();
            }
            e.Handled = true;
        }

        void OnMouseLeave() override
        {
            m_dividerHovered = false;
            if (!m_dragging)
            {
                Cursor = CursorType::Default;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // SplitView fills its parent - request the full available space.
            MeasuredSize = Float2{constraints.MaxWidth, constraints.MaxHeight};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const f32 w = width;
            const f32 h = height;
            const f32 divSize = DividerSize;

            if (AnyPaneCollapsed())
            {
                LayoutCollapsed(w, h);
                return;
            }

            if (Orientation == ::foundation::ui::Orientation::Horizontal)
            {
                const f32 available = w - divSize;
                f32 firstW = available * m_splitRatio;
                f32 secondW = available - firstW;

                // Enforce minimums.
                if (firstW < MinPaneSize && available > MinPaneSize * 2.0f)
                {
                    firstW = MinPaneSize;
                    secondW = available - firstW;
                }
                if (secondW < MinPaneSize && available > MinPaneSize * 2.0f)
                {
                    secondW = MinPaneSize;
                    firstW = available - secondW;
                }

                if (m_first != nullptr)
                {
                    m_first->Measure(BoxConstraints::Tight(firstW, h));
                    m_first->Layout(0, 0, firstW, h);
                }
                if (m_second != nullptr)
                {
                    const f32 secondX = firstW + divSize;
                    m_second->Measure(BoxConstraints::Tight(secondW, h));
                    m_second->Layout(secondX, 0, secondW, h);
                }
            }
            else
            {
                const f32 available = h - divSize;
                f32 firstH = available * m_splitRatio;
                f32 secondH = available - firstH;

                if (firstH < MinPaneSize && available > MinPaneSize * 2.0f)
                {
                    firstH = MinPaneSize;
                    secondH = available - firstH;
                }
                if (secondH < MinPaneSize && available > MinPaneSize * 2.0f)
                {
                    secondH = MinPaneSize;
                    firstH = available - secondH;
                }

                if (m_first != nullptr)
                {
                    m_first->Measure(BoxConstraints::Tight(w, firstH));
                    m_first->Layout(0, 0, w, firstH);
                }
                if (m_second != nullptr)
                {
                    const f32 secondY = firstH + divSize;
                    m_second->Measure(BoxConstraints::Tight(w, secondH));
                    m_second->Layout(0, secondY, w, secondH);
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
            const f32 divSize = DividerSize;
            if (Orientation == ::foundation::ui::Orientation::Horizontal)
            {
                const f32 available = Width() - divSize;
                const f32 divX = available * m_splitRatio;
                return Rectangle{divX, 0, divSize, Height()};
            }
            const f32 available = Height() - divSize;
            const f32 divY = available * m_splitRatio;
            return Rectangle{0, divY, Width(), divSize};
        }

        [[nodiscard]] bool IsInDivider(f32 x, f32 y) const
        {
            if (AnyPaneCollapsed())
            {
                return false; // no draggable divider while a pane is collapsed
            }
            const Rectangle r = GetDividerRect();
            return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
        }

        // A pane collapsed to its content minimum along the split axis; the other pane fills the rest,
        // no divider space reserved. The stored ratio is untouched (restored on un-collapse).
        void LayoutCollapsed(f32 w, f32 h)
        {
            if (Orientation == ::foundation::ui::Orientation::Horizontal)
            {
                if (m_secondCollapsed)
                {
                    f32 secondW = 0.0f;
                    if (m_second != nullptr)
                    {
                        m_second->Measure(BoxConstraints{0.0f, w, h, h});
                        secondW = core::Clamp(m_second->MeasuredSize.x, 0.0f, w);
                    }
                    const f32 firstW = w - secondW;
                    if (m_first != nullptr)
                    {
                        m_first->Measure(BoxConstraints::Tight(firstW, h));
                        m_first->Layout(0, 0, firstW, h);
                    }
                    if (m_second != nullptr)
                    {
                        m_second->Layout(firstW, 0, secondW, h);
                    }
                }
                else
                {
                    f32 firstW = 0.0f;
                    if (m_first != nullptr)
                    {
                        m_first->Measure(BoxConstraints{0.0f, w, h, h});
                        firstW = core::Clamp(m_first->MeasuredSize.x, 0.0f, w);
                    }
                    const f32 secondW = w - firstW;
                    if (m_first != nullptr)
                    {
                        m_first->Layout(0, 0, firstW, h);
                    }
                    if (m_second != nullptr)
                    {
                        m_second->Measure(BoxConstraints::Tight(secondW, h));
                        m_second->Layout(firstW, 0, secondW, h);
                    }
                }
            }
            else
            {
                if (m_secondCollapsed)
                {
                    f32 secondH = 0.0f;
                    if (m_second != nullptr)
                    {
                        m_second->Measure(BoxConstraints{w, w, 0.0f, h});
                        secondH = core::Clamp(m_second->MeasuredSize.y, 0.0f, h);
                    }
                    const f32 firstH = h - secondH;
                    if (m_first != nullptr)
                    {
                        m_first->Measure(BoxConstraints::Tight(w, firstH));
                        m_first->Layout(0, 0, w, firstH);
                    }
                    if (m_second != nullptr)
                    {
                        m_second->Layout(0, firstH, w, secondH);
                    }
                }
                else
                {
                    f32 firstH = 0.0f;
                    if (m_first != nullptr)
                    {
                        m_first->Measure(BoxConstraints{w, w, 0.0f, h});
                        firstH = core::Clamp(m_first->MeasuredSize.y, 0.0f, h);
                    }
                    const f32 secondH = h - firstH;
                    if (m_first != nullptr)
                    {
                        m_first->Layout(0, 0, w, firstH);
                    }
                    if (m_second != nullptr)
                    {
                        m_second->Measure(BoxConstraints::Tight(w, secondH));
                        m_second->Layout(0, firstH, w, secondH);
                    }
                }
            }
        }

        View* m_first = nullptr;  // borrowed; the child tree owns the RefPtr
        View* m_second = nullptr; // borrowed
        f32 m_splitRatio = 0.5f;
        bool m_dragging = false;
        bool m_dividerHovered = false;
        bool m_firstCollapsed = false;
        bool m_secondCollapsed = false;
    };

    RTTI_DEFINE_OBJECT(SplitView, "rtti::ui::toolkit")
}
