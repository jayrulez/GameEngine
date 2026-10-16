// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :scroll_view partition
//
// ScrollView: a container that clips its content to the viewport and offsets it by a scroll
// amount, so a large content area can be panned within a smaller box. Modeled on eepp's
// UIScrollableWidget / UIScrollView (role, not a line-for-line port).
//
// It owns an internal content container (GetContent() - add your widgets there) plus two
// overlay ScrollBars it positions, sizes, shows/hides (per-axis policy Auto/On/Off), and keeps
// in TWO-WAY SYNC with the scroll offset. Scrolling is driven by the wheel (bubbled from the
// hovered content via Node::WantsWheel), by the keyboard when focused (arrows / PageUp-Down /
// Home-End), by dragging a bar, and programmatically. Content extent is set explicitly
// (SetContentSize) or measured from the children (SetAutoMeasureContent / MeasureContent).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:scroll_view;

import foundation.core; // RefPtr, MakeRef, Function, Move, Max, Min, Float2
import :rect;
import :node;
import :event;
import :ui_widget;
import :scroll_bar;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class ScrollBarPolicy
    {
        Auto,
        AlwaysOn,
        AlwaysOff
    };

    // Content container that notifies its ScrollView when its children change (so auto-measure
    // can re-run). Internal helper; users reach it as a Node* via ScrollView::GetContent().
    class ScrollContent : public UIWidget
    {
        RTTI_OBJECT(ScrollContent, UIWidget)
    public:
        core::Function<void()> OnContentChanged;

    protected:
        void OnChildrenChanged() override
        {
            if (OnContentChanged)
                OnContentChanged();
        }
    };

    class ScrollView : public UIWidget
    {
        RTTI_OBJECT(ScrollView, UIWidget)
    public:
        ScrollView()
        {
            SetTag(core::StringView(u8"scrollview"));
            SetClipChildren(true);
            SetTabFocusable(true); // so the view can receive focus for keyboard scrolling

            m_content = core::MakeRef<ScrollContent>(MemoryAllocator());
            m_content->OnContentChanged = [this]()
            {
                if (m_autoMeasure)
                    MeasureContent();
            };
            AddChild(m_content.Get());

            m_vBar = core::MakeRef<ScrollBar>(MemoryAllocator());
            m_vBar->SetOrientation(Orientation::Vertical);
            m_vBar->SetOnValueChanged(
                [this](f32 v)
                {
                    if (!m_syncing)
                        SetVerticalFraction(v);
                });
            AddChild(m_vBar.Get());

            m_hBar = core::MakeRef<ScrollBar>(MemoryAllocator());
            m_hBar->SetOrientation(Orientation::Horizontal);
            m_hBar->SetOnValueChanged(
                [this](f32 v)
                {
                    if (!m_syncing)
                        SetHorizontalFraction(v);
                });
            AddChild(m_hBar.Get());
        }

        // The container holding the scrollable content: set its size to the scrollable extent
        // (or use SetAutoMeasureContent) and add your widgets to it.
        [[nodiscard]] Node* GetContent() const noexcept { return m_content.Get(); }
        [[nodiscard]] Node* GetChildHost() override { return m_content.Get(); } // markup's children scroll
        [[nodiscard]] ScrollBar* GetVerticalScrollBar() const noexcept { return m_vBar.Get(); }
        [[nodiscard]] ScrollBar* GetHorizontalScrollBar() const noexcept { return m_hBar.Get(); }

        void SetContentSize(core::Float2 size)
        {
            m_content->SetSize(size);
            Relayout();
        }
        [[nodiscard]] core::Float2 GetContentSize() const { return m_content->GetSize(); }

        // Size the content container to the bounding box of its (visible) children.
        void MeasureContent()
        {
            f32 w = 0.0f, h = 0.0f;
            for (usize i = 0; i < m_content->ChildCount(); ++i)
            {
                Node* child = m_content->GetChildAt(i);
                if (child == nullptr || !child->IsVisible())
                    continue;
                const core::Float2 p = child->GetPosition();
                const core::Float2 s = child->GetSize();
                w = core::Max(w, p.x + s.x);
                h = core::Max(h, p.y + s.y);
            }
            m_content->SetSize(core::Float2{w, h});
            Relayout();
        }
        // When on, MeasureContent runs automatically whenever the content's children change.
        void SetAutoMeasureContent(bool enabled)
        {
            m_autoMeasure = enabled;
            if (enabled)
                MeasureContent();
        }
        [[nodiscard]] bool IsAutoMeasureContent() const noexcept { return m_autoMeasure; }

        // The viewport (padding-inset content box). Overlay scrollbars float over it, so the
        // viewport is the full box (content under a bar's right/bottom strip is covered).
        [[nodiscard]] Rect Viewport() const { return GetContentBounds(); }

        // The maximum scrollable distance on each axis: max(0, content - viewport).
        [[nodiscard]] core::Float2 ScrollRange() const
        {
            const Rect vp = Viewport();
            const core::Float2 cs = m_content->GetSize();
            return core::Float2{core::Max(0.0f, cs.x - vp.width),
                                core::Max(0.0f, cs.y - vp.height)};
        }

        [[nodiscard]] core::Float2 GetScrollOffset() const noexcept { return m_offset; }
        void SetScrollOffset(core::Float2 offset)
        {
            const core::Float2 clamped = Clamp(offset);
            if (clamped.x == m_offset.x && clamped.y == m_offset.y)
                return;
            m_offset = clamped;
            Relayout();
            if (m_onScroll)
                m_onScroll(m_offset);
        }
        void ScrollBy(core::Float2 delta)
        {
            SetScrollOffset(core::Float2{m_offset.x + delta.x, m_offset.y + delta.y});
        }

        // Scroll fraction on each axis in [0,1] (0 when the axis cannot scroll).
        [[nodiscard]] core::Float2 GetScrollFraction() const
        {
            const core::Float2 r = ScrollRange();
            return core::Float2{r.x > 0.0f ? m_offset.x / r.x : 0.0f,
                                r.y > 0.0f ? m_offset.y / r.y : 0.0f};
        }
        void SetVerticalFraction(f32 fraction)
        {
            SetScrollOffset(core::Float2{
                m_offset.x, ScrollRange().y * core::Max(0.0f, core::Min(1.0f, fraction))});
        }
        void SetHorizontalFraction(f32 fraction)
        {
            SetScrollOffset(core::Float2{
                ScrollRange().x * core::Max(0.0f, core::Min(1.0f, fraction)), m_offset.y});
        }

        void SetVerticalScrollBarPolicy(ScrollBarPolicy policy)
        {
            m_vPolicy = policy;
            Relayout();
        }
        void SetHorizontalScrollBarPolicy(ScrollBarPolicy policy)
        {
            m_hPolicy = policy;
            Relayout();
        }
        void SetScrollBarThickness(f32 thickness)
        {
            m_barThickness = thickness;
            Relayout();
        }

        void SetWheelSpeed(f32 pixelsPerNotch) noexcept { m_wheelSpeed = pixelsPerNotch; }
        void SetLineStep(f32 pixels) noexcept { m_lineStep = pixels; }
        void SetOnScroll(core::Function<void(core::Float2)> callback)
        {
            m_onScroll = core::Move(callback);
        }

        [[nodiscard]] bool WantsWheel() const override { return true; }

    protected:
        void OnMouseWheel(const WheelEvent& event) override
        {
            // Wheel up (positive y) reveals content above -> decreases the offset.
            ScrollBy(core::Float2{-event.Delta.x * m_wheelSpeed, -event.Delta.y * m_wheelSpeed});
        }

        void OnKeyDown(const KeyEvent& event) override
        {
            const Rect vp = Viewport();
            const core::Float2 before = m_offset;
            switch (static_cast<KeyCode>(event.KeyCode))
            {
            case KeyCode::Down:
                ScrollBy(core::Float2{0.0f, m_lineStep});
                break;
            case KeyCode::Up:
                ScrollBy(core::Float2{0.0f, -m_lineStep});
                break;
            case KeyCode::Right:
                ScrollBy(core::Float2{m_lineStep, 0.0f});
                break;
            case KeyCode::Left:
                ScrollBy(core::Float2{-m_lineStep, 0.0f});
                break;
            case KeyCode::PageDown:
                ScrollBy(core::Float2{0.0f, vp.height});
                break;
            case KeyCode::PageUp:
                ScrollBy(core::Float2{0.0f, -vp.height});
                break;
            case KeyCode::Home:
                SetScrollOffset(core::Float2{m_offset.x, 0.0f});
                break;
            case KeyCode::End:
                SetScrollOffset(core::Float2{m_offset.x, ScrollRange().y});
                break;
            default:
                return;
            }
            // An arrow that could not scroll further is given back (focus moves on).
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            const bool arrow = key == KeyCode::Up || key == KeyCode::Down || key == KeyCode::Left || key == KeyCode::Right;
            event.Handled = !arrow || m_offset.x != before.x || m_offset.y != before.y;
        }

        void OnSizeChange() override { Relayout(); }

        [[nodiscard]] bool ScrollsDescendantsIntoView() const override { return true; }
        // A focused widget inside comes into view (G7): scrolled by the least that shows it.
        void OnDescendantFocused(Node& focused) override
        {
            const Rect world = focused.GetScreenBounds();
            const core::Float2 lo = ConvertToNodeSpace(core::Float2{world.x, world.y});
            const core::Float2 hi = ConvertToNodeSpace(core::Float2{world.x + world.width, world.y + world.height});
            const Rect vp = Viewport();
            core::Float2 offset = m_offset;
            if (lo.y < vp.y)
                offset.y -= vp.y - lo.y;
            else if (hi.y > vp.y + vp.height)
                offset.y += hi.y - (vp.y + vp.height);
            if (lo.x < vp.x)
                offset.x -= vp.x - lo.x;
            else if (hi.x > vp.x + vp.width)
                offset.x += hi.x - (vp.x + vp.width);
            SetScrollOffset(offset);
        }

    private:
        [[nodiscard]] core::Float2 Clamp(core::Float2 offset) const
        {
            const core::Float2 r = ScrollRange();
            return core::Float2{core::Max(0.0f, core::Min(r.x, offset.x)),
                                core::Max(0.0f, core::Min(r.y, offset.y))};
        }

        [[nodiscard]] static bool BarVisible(ScrollBarPolicy policy, f32 range)
        {
            return policy == ScrollBarPolicy::AlwaysOn ||
                   (policy == ScrollBarPolicy::Auto && range > 0.0f);
        }

        // Reposition/size the content and both overlay bars, and sync bar value/proportion to
        // the current scroll. m_syncing guards the bars' change callbacks from feeding back.
        void Relayout()
        {
            const Rect vp = Viewport();
            const core::Float2 cs = m_content->GetSize();
            const core::Float2 range{core::Max(0.0f, cs.x - vp.width),
                                     core::Max(0.0f, cs.y - vp.height)};
            m_offset = core::Float2{core::Max(0.0f, core::Min(range.x, m_offset.x)),
                                    core::Max(0.0f, core::Min(range.y, m_offset.y))};
            m_content->SetPosition(core::Float2{vp.x - m_offset.x, vp.y - m_offset.y});

            const bool vVis = BarVisible(m_vPolicy, range.y);
            const bool hVis = BarVisible(m_hPolicy, range.x);

            m_syncing = true;
            m_vBar->SetVisible(vVis);
            if (vVis)
            {
                m_vBar->SetPosition(core::Float2{vp.x + vp.width - m_barThickness, vp.y});
                m_vBar->SetSize(
                    core::Float2{m_barThickness, vp.height - (hVis ? m_barThickness : 0.0f)});
                m_vBar->SetThumbProportion(cs.y > 0.0f ? vp.height / cs.y : 1.0f);
                m_vBar->SetValue(range.y > 0.0f ? m_offset.y / range.y : 0.0f);
            }
            m_hBar->SetVisible(hVis);
            if (hVis)
            {
                m_hBar->SetPosition(core::Float2{vp.x, vp.y + vp.height - m_barThickness});
                m_hBar->SetSize(
                    core::Float2{vp.width - (vVis ? m_barThickness : 0.0f), m_barThickness});
                m_hBar->SetThumbProportion(cs.x > 0.0f ? vp.width / cs.x : 1.0f);
                m_hBar->SetValue(range.x > 0.0f ? m_offset.x / range.x : 0.0f);
            }
            m_syncing = false;
        }

        RefPtr<ScrollContent> m_content;
        RefPtr<ScrollBar> m_vBar;
        RefPtr<ScrollBar> m_hBar;
        core::Float2 m_offset{0.0f, 0.0f};
        ScrollBarPolicy m_vPolicy = ScrollBarPolicy::Auto;
        ScrollBarPolicy m_hPolicy = ScrollBarPolicy::Auto;
        f32 m_barThickness = 12.0f;
        f32 m_wheelSpeed = 40.0f;
        f32 m_lineStep = 30.0f;
        bool m_autoMeasure = false;
        bool m_syncing = false;
        core::Function<void(core::Float2)> m_onScroll;
    };

    RTTI_DEFINE_OBJECT(ScrollContent, "rtti::gui")
}
