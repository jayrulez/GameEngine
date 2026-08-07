// Draconic UI - :scroll_view partition
//
// Scrollable container: content can exceed the viewport, with optional scroll bars and momentum-based
// kinetic scrolling. Ported from Sedulous.UI/src/Controls/ScrollView.bf. The two ScrollBars are VISUAL
// children (not in m_children): VisualChildCount = ChildCount + 2, appended after logical children so
// they draw/hit-test on top but don't affect content measure/layout. Owned as RefPtr members (RAII) -
// UIContext::AttachView/DetachView already recurses VisualChildCount, so they attach/detach with the
// view (no manual dtor detach like Beef needs). ScrollX/ScrollY/MaxScroll*/Viewport*/Needs*Bar get/set
// properties -> methods; the `ScrollBarMode` Property field shadows its enum type -> aliased.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:scroll_view;

import draconic.foundation;
import :view;
import :property;
import :event;
import :box_constraints;
import :thickness;
import :draw_context;
import :event_args;
import :input_enums;
import :enums;
import :scroll_bar;
import :momentum_helper;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    /// Scroll bar visibility policy.
    enum class ScrollBarPolicy
    {
        Never,
        Always,
        Auto
    };

    /// Scroll bar rendering mode.
    enum class ScrollBarMode
    {
        Overlay, ///< Scrollbar overlays content (content uses full width/height).
        Reserved ///< Space is reserved for the scrollbar (content shrinks to make room).
    };
    // Alias so the faithful `ScrollBarMode` Property field can still name the enum type.
    using ScrollBarModeValue = ScrollBarMode;

    class ScrollView : public ViewGroup
    {
        DRACONIC_OBJECT(ScrollView, ViewGroup)
    public:
        Property<ScrollBarPolicy> VScrollBarPolicy{ScrollBarPolicy::Auto};
        Property<ScrollBarPolicy> HScrollBarPolicy{ScrollBarPolicy::Auto};
        Property<ScrollBarModeValue> ScrollBarMode{ScrollBarModeValue::Overlay};
        Property<bool> MomentumEnabled{true};
        Property<f32> ScrollBarThickness{10.0f};

        ScrollView()
        {
            ClipsContent = true;
            VScrollBarPolicy.SetOwner(this);
            HScrollBarPolicy.SetOwner(this);
            ScrollBarMode.SetOwner(this);
            MomentumEnabled.SetOwner(this);
            ScrollBarThickness.SetOwner(this);

            ScrollView* self = this;
            m_vBar = MakeRef<ScrollBar>(DefaultAllocator(), false);
            m_vBar->Visibility = VisibilityValue::Gone;
            m_vBar->BarThickness = ScrollBarThickness.Value();
            m_vBar->OnValueChanged.Add(Event<void(ScrollBar*, f32)>::Handler{
                [self](ScrollBar*, f32 val) { self->SetScrollY(val); }});

            m_hBar = MakeRef<ScrollBar>(DefaultAllocator(), true);
            m_hBar->Visibility = VisibilityValue::Gone;
            m_hBar->BarThickness = ScrollBarThickness.Value();
            m_hBar->OnValueChanged.Add(Event<void(ScrollBar*, f32)>::Handler{
                [self](ScrollBar*, f32 val) { self->SetScrollX(val); }});
        }

        // === Scroll offsets (Beef properties -> methods) ===
        [[nodiscard]] f32 ScrollX() const noexcept { return m_scrollX; }
        void SetScrollX(f32 value)
        {
            const f32 c = foundation::Clamp(value, 0.0f, MaxScrollX());
            if (m_scrollX == c)
            {
                return;
            }
            m_scrollX = c;
            Invalidate();
        }
        [[nodiscard]] f32 ScrollY() const noexcept { return m_scrollY; }
        void SetScrollY(f32 value)
        {
            const f32 c = foundation::Clamp(value, 0.0f, MaxScrollY());
            if (m_scrollY == c)
            {
                return;
            }
            m_scrollY = c;
            Invalidate();
        }

        [[nodiscard]] f32 MaxScrollX() const
        {
            return foundation::Max(0.0f, m_contentWidth - ViewportWidth());
        }
        [[nodiscard]] f32 MaxScrollY() const
        {
            return foundation::Max(0.0f, m_contentHeight - ViewportHeight());
        }
        [[nodiscard]] f32 ContentWidth() const noexcept { return m_contentWidth; }
        [[nodiscard]] f32 ContentHeight() const noexcept { return m_contentHeight; }

        [[nodiscard]] f32 ViewportWidth() const
        {
            const f32 barSpace =
                (ScrollBarMode.Value() == ScrollBarModeValue::Reserved && NeedsVBar())
                    ? ScrollBarThickness.Value()
                    : 0.0f;
            return foundation::Max(0.0f, Width() - Padding.TotalHorizontal() - barSpace);
        }
        [[nodiscard]] f32 ViewportHeight() const
        {
            const f32 barSpace =
                (ScrollBarMode.Value() == ScrollBarModeValue::Reserved && NeedsHBar())
                    ? ScrollBarThickness.Value()
                    : 0.0f;
            return foundation::Max(0.0f, Height() - Padding.TotalVertical() - barSpace);
        }

        // === Scroll commands ===
        void ScrollTo(f32 x, f32 y)
        {
            SetScrollX(x);
            SetScrollY(y);
            m_momentum.Stop();
        }
        void ScrollToTop()
        {
            SetScrollY(0);
            m_momentum.Stop();
        }
        void ScrollToBottom()
        {
            SetScrollY(MaxScrollY());
            m_momentum.Stop();
        }
        void ScrollToLeft()
        {
            SetScrollX(0);
            m_momentum.Stop();
        }
        void ScrollToRight()
        {
            SetScrollX(MaxScrollX());
            m_momentum.Stop();
        }
        void ScrollBy(f32 dx, f32 dy)
        {
            SetScrollX(m_scrollX + dx);
            SetScrollY(m_scrollY + dy);
        }

        /// Scroll to make `child` (a descendant) visible within the viewport.
        void ScrollToView(View* child)
        {
            if (child == nullptr)
            {
                return;
            }
            f32 offsetX = 0, offsetY = 0;
            View* current = child;
            while (current != nullptr && current != this)
            {
                offsetX += current->Bounds.x;
                offsetY += current->Bounds.y;
                current = current->Parent;
            }
            if (current == nullptr)
            {
                return;
            } // not a descendant

            offsetX += m_scrollX;
            offsetY += m_scrollY; // bounds are laid out scroll-adjusted

            const f32 childRight = offsetX + child->Width();
            if (offsetX < m_scrollX)
            {
                SetScrollX(offsetX);
            }
            else if (childRight > m_scrollX + ViewportWidth())
            {
                SetScrollX(childRight - ViewportWidth());
            }

            const f32 childBottom = offsetY + child->Height();
            if (offsetY < m_scrollY)
            {
                SetScrollY(offsetY);
            }
            else if (childBottom > m_scrollY + ViewportHeight())
            {
                SetScrollY(childBottom - ViewportHeight());
            }

            m_momentum.Stop();
        }

        // Scrollbars appended as visual children after logical children.
        [[nodiscard]] usize VisualChildCount() const override { return ChildCount() + 2; }
        [[nodiscard]] View* GetVisualChild(usize index) const override
        {
            if (index < ChildCount())
            {
                return GetChildAt(index);
            }
            if (index == ChildCount())
            {
                return m_vBar.Get();
            }
            if (index == ChildCount() + 1)
            {
                return m_hBar.Get();
            }
            return nullptr;
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (MomentumEnabled.Value() && m_momentum.IsActive())
            {
                const Float2 d = m_momentum.Update(Context ? Context->DeltaTime() : 0.016f);
                SetScrollX(m_scrollX + d.x);
                SetScrollY(m_scrollY + d.y);
            }
            DrawChildren(ctx); // includes the scrollbars (visual children), drawn on top
        }

        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            const f32 scrollAmount = 40.0f;
            f32 hDelta = 0;
            if (e.DeltaX != 0)
            {
                hDelta = e.DeltaX;
            }
            else if (NeedsHBar() && e.DeltaY != 0 && HasFlag(e.Modifiers, KeyModifiers::Shift))
            {
                hDelta = e.DeltaY;
            }
            else if (NeedsHBar() && !NeedsVBar() && e.DeltaY != 0)
            {
                hDelta = e.DeltaY;
            }

            if (NeedsHBar() && hDelta != 0)
            {
                SetScrollX(m_scrollX - hDelta * scrollAmount);
                if (MomentumEnabled.Value())
                {
                    m_momentum.VelocityX = -hDelta * scrollAmount * 3;
                }
                e.Handled = true;
            }
            else if (NeedsVBar() && e.DeltaY != 0)
            {
                SetScrollY(m_scrollY - e.DeltaY * scrollAmount);
                if (MomentumEnabled.Value())
                {
                    m_momentum.VelocityY = -e.DeltaY * scrollAmount * 3;
                }
                e.Handled = true;
            }
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button == MouseButton::Left && (MaxScrollX() > 0 || MaxScrollY() > 0))
            {
                const Float2 local = ScreenToLocal(MouseScreenPos());
                m_dragging = true;
                m_dragLastX = local.x;
                m_dragLastY = local.y;
                m_momentum.Stop();
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
            }
        }
        void OnMouseMove(MouseEventArgs& e) override
        {
            (void)e;
            if (m_dragging)
            {
                const Float2 local = ScreenToLocal(MouseScreenPos());
                const f32 dx = m_dragLastX - local.x;
                const f32 dy = m_dragLastY - local.y;
                if (Abs(dx) > 1 || Abs(dy) > 1)
                {
                    ScrollBy(dx, dy);
                    m_momentum.VelocityX = dx * 60;
                    m_momentum.VelocityY = dy * 60;
                    m_dragLastX = local.x;
                    m_dragLastY = local.y;
                }
            }
        }
        void OnMouseUp(MouseEventArgs& e) override
        {
            (void)e;
            if (m_dragging)
            {
                m_dragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            const f32 pageSize = ViewportHeight() * 0.9f;
            switch (e.Key)
            {
            case KeyCode::Up:
                SetScrollY(m_scrollY - 40);
                e.Handled = true;
                break;
            case KeyCode::Down:
                SetScrollY(m_scrollY + 40);
                e.Handled = true;
                break;
            case KeyCode::PageUp:
                SetScrollY(m_scrollY - pageSize);
                e.Handled = true;
                break;
            case KeyCode::PageDown:
                SetScrollY(m_scrollY + pageSize);
                e.Handled = true;
                break;
            case KeyCode::Home:
                ScrollToTop();
                e.Handled = true;
                break;
            case KeyCode::End:
                ScrollToBottom();
                e.Handled = true;
                break;
            default:
                break;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 fullW = foundation::Max(0.0f, constraints.MaxWidth - Padding.TotalHorizontal());
            const f32 fullH = foundation::Max(0.0f, constraints.MaxHeight - Padding.TotalVertical());

            const f32 childMaxW =
                (HScrollBarPolicy.Value() == ScrollBarPolicy::Always) ? kFloatMax : fullW;
            const f32 childMaxH =
                (VScrollBarPolicy.Value() == ScrollBarPolicy::Never) ? fullH : kFloatMax;

            f32 maxW = 0, maxH = 0;
            MeasureChildren(BoxConstraints{0, childMaxW, 0, childMaxH}, maxW, maxH);
            m_contentWidth = maxW;
            m_contentHeight = maxH;

            // Reserved mode second pass: re-measure with scrollbar space subtracted.
            if (ScrollBarMode.Value() == ScrollBarModeValue::Reserved)
            {
                const bool needsVBar =
                    (VScrollBarPolicy.Value() == ScrollBarPolicy::Always) ||
                    (VScrollBarPolicy.Value() == ScrollBarPolicy::Auto && maxH > fullH);
                const bool needsHBar =
                    (HScrollBarPolicy.Value() == ScrollBarPolicy::Always) ||
                    (HScrollBarPolicy.Value() == ScrollBarPolicy::Auto && maxW > fullW);
                if (needsVBar || needsHBar)
                {
                    const f32 adjustedW =
                        foundation::Max(0.0f, fullW - (needsVBar ? ScrollBarThickness.Value() : 0.0f));
                    const f32 adjustedH =
                        foundation::Max(0.0f, fullH - (needsHBar ? ScrollBarThickness.Value() : 0.0f));
                    const f32 adjChildMaxW = (HScrollBarPolicy.Value() == ScrollBarPolicy::Always)
                                                 ? kFloatMax
                                                 : adjustedW;
                    const f32 adjChildMaxH = (VScrollBarPolicy.Value() == ScrollBarPolicy::Never)
                                                 ? adjustedH
                                                 : kFloatMax;
                    maxW = 0;
                    maxH = 0;
                    MeasureChildren(BoxConstraints{0, adjChildMaxW, 0, adjChildMaxH}, maxW, maxH);
                    m_contentWidth = maxW;
                    m_contentHeight = maxH;
                }
            }

            const f32 measuredW = (HScrollBarPolicy.Value() == ScrollBarPolicy::Never)
                                      ? maxW + Padding.TotalHorizontal()
                                      : constraints.MaxWidth;
            f32 measuredH;
            if (VScrollBarPolicy.Value() == ScrollBarPolicy::Never)
            {
                const f32 hBarH = (HScrollBarPolicy.Value() != ScrollBarPolicy::Never &&
                                   ScrollBarMode.Value() == ScrollBarModeValue::Reserved)
                                      ? ScrollBarThickness.Value()
                                      : 0.0f;
                measuredH = maxH + Padding.TotalVertical() + hBarH;
            }
            else
            {
                measuredH = constraints.MaxHeight;
            }

            MeasuredSize = Float2{constraints.ConstrainWidth(measuredW),
                                  constraints.ConstrainHeight(measuredH)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            m_vBar->Visibility = NeedsVBar() ? VisibilityValue::Visible : VisibilityValue::Gone;
            m_hBar->Visibility = NeedsHBar() ? VisibilityValue::Visible : VisibilityValue::Gone;

            m_scrollX = foundation::Clamp(m_scrollX, 0.0f, MaxScrollX());
            m_scrollY = foundation::Clamp(m_scrollY, 0.0f, MaxScrollY());

            m_vBar->Parent = this;
            m_hBar->Parent = this;
            if (Context != nullptr && m_vBar->Context == nullptr)
            {
                Context->AttachView(m_vBar.Get());
            }
            if (Context != nullptr && m_hBar->Context == nullptr)
            {
                Context->AttachView(m_hBar.Get());
            }

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == VisibilityValue::Gone)
                {
                    continue;
                }
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
                const f32 childW =
                    foundation::Max(child->MeasuredSize.x, ViewportWidth() - margin.TotalHorizontal());
                const f32 childH = child->MeasuredSize.y;
                child->Layout(Padding.Left + margin.Left - m_scrollX,
                              Padding.Top + margin.Top - m_scrollY, childW, childH);
            }

            if (NeedsVBar())
            {
                m_vBar->SetMaxValue(MaxScrollY());
                m_vBar->SetViewportSize(ViewportHeight());
                m_vBar->SetValue(m_scrollY);
                m_vBar->Measure(
                    BoxConstraints::Tight(ScrollBarThickness.Value(), ViewportHeight()));
                m_vBar->Layout(width - ScrollBarThickness.Value(), Padding.Top,
                               ScrollBarThickness.Value(), ViewportHeight());
            }
            if (NeedsHBar())
            {
                m_hBar->SetMaxValue(MaxScrollX());
                m_hBar->SetViewportSize(ViewportWidth());
                m_hBar->SetValue(m_scrollX);
                m_hBar->Measure(BoxConstraints::Tight(ViewportWidth(), ScrollBarThickness.Value()));
                m_hBar->Layout(Padding.Left, height - ScrollBarThickness.Value(), ViewportWidth(),
                               ScrollBarThickness.Value());
            }
        }

    private:
        [[nodiscard]] bool NeedsVBar() const
        {
            if (VScrollBarPolicy.Value() == ScrollBarPolicy::Never)
            {
                return false;
            }
            if (VScrollBarPolicy.Value() == ScrollBarPolicy::Always)
            {
                return true;
            }
            return m_contentHeight > Height() - Padding.TotalVertical();
        }
        [[nodiscard]] bool NeedsHBar() const
        {
            if (HScrollBarPolicy.Value() == ScrollBarPolicy::Never)
            {
                return false;
            }
            if (HScrollBarPolicy.Value() == ScrollBarPolicy::Always)
            {
                return true;
            }
            return m_contentWidth > Width() - Padding.TotalHorizontal();
        }

        void MeasureChildren(BoxConstraints childConstraints, f32& maxW, f32& maxH)
        {
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == VisibilityValue::Gone)
                {
                    continue;
                }
                const Thickness margin =
                    child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
                child->Measure(childConstraints.Deflate(margin));
                maxW = foundation::Max(maxW, child->MeasuredSize.x + margin.TotalHorizontal());
                maxH = foundation::Max(maxH, child->MeasuredSize.y + margin.TotalVertical());
            }
        }

        [[nodiscard]] Float2 MouseScreenPos() const
        {
            if (Context == nullptr || Context->GetInputManager() == nullptr)
            {
                return Float2{0, 0};
            }
            return Float2{Context->GetInputManager()->MouseX(),
                          Context->GetInputManager()->MouseY()};
        }

        f32 m_scrollX = 0.0f;
        f32 m_scrollY = 0.0f;
        f32 m_contentWidth = 0.0f;
        f32 m_contentHeight = 0.0f;
        MomentumHelper m_momentum{};

        bool m_dragging = false;
        f32 m_dragLastX = 0.0f;
        f32 m_dragLastY = 0.0f;

        RefPtr<ScrollBar> m_vBar;
        RefPtr<ScrollBar> m_hBar;
    };

    DRACONIC_DEFINE_OBJECT(ScrollView, "draconic::ui")
}
