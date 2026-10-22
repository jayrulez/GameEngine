// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :breadcrumb_bar partition
//
// The breadcrumb bar (T7 of Specs/gui-lab-evolution-3.md): where a view is, as segments from its
// root to the current one, each carrying its target (an id the application gets back), so the
// application keeps no table beside it (doriax pairs a name with its path). A click navigates on
// release; the last segment is the current one, shown so and inert. When the segments do not fit,
// the current one and as many before it as fit stay, filled from the end, and the rest fold into a
// "…" button whose menu lists them (doriax). One tab stop: Left and Right move along it (mirrored
// right to left, where its segments run right to left and its chevrons turn), Return navigates.
// History (back, forward, up) is the view's that holds it, as in every bar surveyed.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui.toolkit:breadcrumb_bar;

import foundation.core;
import foundation.vg;
import experimental.gui;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    /// One segment of a breadcrumb bar: what it shows and what navigating to it means.
    struct BreadcrumbItem
    {
        core::String Id;      ///< what the application gets back (a folder's path, a node's id)
        core::String Text;    ///< what it shows
        core::String Tooltip; ///< shown on hover (the full path); empty: none
        core::String Icon;    ///< an icon's path, loaded as a button's; empty: none
    };

    class BreadcrumbBar;

    /// A segment: a button with the item's text and icon (`crumb`; the current one `.current`).
    class BreadcrumbSegment : public Button
    {
        RTTI_OBJECT(BreadcrumbSegment, Button)
    public:
        BreadcrumbSegment()
        {
            SetTag(core::StringView(u8"crumb"));
            StyleWriteScope defaults(*this);
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Shrink = 0.0f;
                s.MinWidth = LayoutLength::Px(0.0f);
            });
        }
        /// Whether it is the current segment, to a screen reader (its description).
        [[nodiscard]] core::String GetAccessDescription() const
        {
            return HasClass(core::StringView(u8"current")) ? core::String(core::StringView(u8"current")) : core::String{};
        }
    };

    /// The mark between two segments (`crumbseparator`): a chevron toward the current one.
    class BreadcrumbSeparator : public UIWidget
    {
        RTTI_OBJECT(BreadcrumbSeparator, UIWidget)
    public:
        BreadcrumbSeparator()
        {
            SetTag(core::StringView(u8"crumbseparator"));
            SetHitTestVisible(false);
            SetMirrorOnRtl(true); // drawn pointing right; right to left, it points left
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{8.0f, 10.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            const Rect c = GetContentBounds();
            const f32 cx = c.x + c.width * 0.5f;
            const f32 cy = c.y + c.height * 0.5f;
            vg::PathBuilder chevron;
            chevron.MoveTo(cx - 1.5f, cy - 3.5f);
            chevron.LineTo(cx + 2.0f, cy);
            chevron.LineTo(cx - 1.5f, cy + 3.5f);
            ctx.VG().StrokePath(chevron.ToPath(), GetTextColor(), vg::StrokeStyle(1.25f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
        }
    };

    class BreadcrumbBar : public UIWidget
    {
        RTTI_OBJECT(BreadcrumbBar, UIWidget)
    public:
        BreadcrumbBar()
        {
            SetTag(core::StringView(u8"breadcrumbbar"));
            {
                StyleWriteScope defaults(*this);
                EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Row;
                    s.AlignItems = Align::Center;
                    s.ColumnGap = 2.0f;
                    s.MinWidth = LayoutLength::Px(0.0f); // never held at its segments' length: they fold
                });
                SetPadding(Thickness{4.0f, 2.0f, 4.0f, 2.0f});
            }
            m_overflow = core::MakeRef<Button>(MemoryAllocator());
            m_overflow->SetTag(core::StringView(u8"crumboverflow"));
            m_overflow->SetText(core::StringView(u8"…"));
            m_overflow->SetVisible(false);
            BreadcrumbBar* self = this;
            m_overflow->SetOnClick([self]() { self->OpenOverflow(); });
            AddChild(m_overflow.Get());
            m_overflowSeparator = core::MakeRef<BreadcrumbSeparator>(MemoryAllocator());
            m_overflowSeparator->SetVisible(false);
            AddChild(m_overflowSeparator.Get());
        }

        /// Its segments, from the root to the current one (the last).
        void SetItems(core::Span<const BreadcrumbItem> items)
        {
            for (const core::RefPtr<BreadcrumbSegment>& segment : m_segments)
                RemoveChild(segment.Get());
            for (const core::RefPtr<BreadcrumbSeparator>& separator : m_separators)
                RemoveChild(separator.Get());
            m_segments.Clear();
            m_separators.Clear();
            m_items.Clear();
            m_roving = nullptr;
            BreadcrumbBar* self = this;
            for (usize i = 0; i < items.Size(); ++i)
            {
                const BreadcrumbItem& item = items[i];
                m_items.PushBack(item);
                if (i > 0)
                {
                    m_separators.PushBack(core::MakeRef<BreadcrumbSeparator>(MemoryAllocator()));
                    AddChild(m_separators.Back().Get());
                }
                core::RefPtr<BreadcrumbSegment> segment = core::MakeRef<BreadcrumbSegment>(MemoryAllocator());
                segment->SetText(item.Text.AsView());
                if (Label* text = segment->GetTextPart())
                {
                    text->SetTextOverflow(TextOverflow::Ellipsis); // the current one, when even it does not fit
                    text->EditLayoutStyle([](LayoutStyle& s) { s.MinWidth = LayoutLength::Px(0.0f); });
                }
                if (!item.Icon.IsEmpty())
                    segment->SetIconSource(item.Icon);
                segment->SetTooltip(item.Tooltip.AsView());
                if (i + 1 == items.Size())
                    segment->AddClass(core::StringView(u8"current"));
                const usize index = i;
                segment->SetOnClick([self, index]() { (void)self->Navigate(index); });
                AddChild(segment.Get());
                m_segments.PushBack(core::Move(segment));
            }
            RequestLayout();
        }
        /// Its segments from a `/` path: each folder's id the path up to it. A leading `/` is a
        /// segment of its own (the root), as is a drive (`C:`); empty parts are skipped.
        void SetPath(core::StringView path)
        {
            core::Array<BreadcrumbItem> items(MemoryAllocator());
            usize at = 0;
            if (path.Size() != 0 && path[0] == u8'/')
            {
                items.PushBack(BreadcrumbItem{core::String(core::StringView(u8"/")), core::String(core::StringView(u8"/")),
                                              core::String(core::StringView(u8"/")), {}});
                at = 1;
            }
            while (at < path.Size())
            {
                usize end = at;
                while (end < path.Size() && path[end] != u8'/')
                    ++end;
                if (end > at)
                {
                    const core::String upTo(path.SubStr(0, end));
                    items.PushBack(BreadcrumbItem{upTo, core::String(path.SubStr(at, end - at)), upTo, {}});
                }
                at = end + 1;
            }
            SetItems(items.AsSpan());
        }
        [[nodiscard]] usize ItemCount() const noexcept { return m_items.Size(); }
        [[nodiscard]] const BreadcrumbItem& ItemAt(usize index) const noexcept { return m_items[index]; }

        /// Told the item navigated to (a segment clicked, chosen from the overflow menu, or Return).
        void SetOnNavigate(core::Function<void(const BreadcrumbItem&)> callback) { m_onNavigate = core::Move(callback); }
        /// Navigates to segment `index` (as a click does); false for the current one or none.
        bool Navigate(usize index)
        {
            if (index + 1 >= m_items.Size())
                return false; // the current segment is where it is
            if (m_onNavigate)
            {
                const BreadcrumbItem item = m_items[index]; // the callback may set new items
                m_onNavigate(item);
            }
            return true;
        }

        /// The segments, from the root.
        [[nodiscard]] BreadcrumbSegment& SegmentAt(usize index) const noexcept { return *m_segments[index]; }
        /// The separator before segment `index` (from 1).
        [[nodiscard]] BreadcrumbSeparator& SeparatorBefore(usize index) const noexcept { return *m_separators[index - 1]; }
        /// How many segments, from the root, folded into the overflow menu (0: all show).
        [[nodiscard]] usize FoldedCount() const noexcept { return m_folded; }
        [[nodiscard]] Button& GetOverflowButton() const noexcept { return *m_overflow; }
        /// The overflow menu, while it is open.
        [[nodiscard]] Menu* GetOverflowMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }
        /// Lists the folded segments below the "…" button; choosing one navigates to it.
        void OpenOverflow()
        {
            m_menu = core::MakeRef<Menu>(MemoryAllocator());
            ShareTextStyle(*m_menu, false);
            m_menu->SetAutoWidth(true);
            BreadcrumbBar* self = this;
            for (usize i = 0; i < m_folded; ++i)
                (void)m_menu->AddItem(m_items[i].Text.AsView(), [self, i]() { (void)self->Navigate(i); });
            m_menu->OpenBelow(*this, *m_overflow);
        }

        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            // Every segment and separator as it would be with nothing in its way (what it asks while
            // all fit), as tall as the tallest.
            const f32 gap = GetLayoutStyle().ColumnGap;
            f32 along = 0.0f, across = 0.0f;
            usize count = 0;
            const auto add = [&](Node& node)
            {
                const core::Float2 size = Natural(node);
                along += size.x;
                across = core::Max(across, size.y);
                ++count;
            };
            for (usize i = 0; i < m_segments.Size(); ++i)
            {
                if (i > 0)
                    add(*m_separators[i - 1]);
                add(*m_segments[i]);
            }
            if (count > 1)
                along += gap * static_cast<f32>(count - 1);
            return core::Float2{along, across};
        }

    protected:
        // Its segments, laid out by itself: from the end, as many as fit; the rest folded.
        void OnLayout() override
        {
            const Rect content = GetContentBounds();
            const f32 gap = GetLayoutStyle().ColumnGap;
            const usize count = m_segments.Size();
            core::Array<core::Float2> segments(MemoryAllocator());
            core::Array<core::Float2> separators(MemoryAllocator());
            for (usize i = 0; i < count; ++i)
            {
                segments.PushBack(Natural(*m_segments[i]));
                if (i > 0)
                    separators.PushBack(Natural(*m_separators[i - 1]));
            }
            const core::Float2 overflow = Natural(*m_overflow);
            const core::Float2 overflowSeparator = Natural(*m_overflowSeparator);
            // What segments `first` to the last take, with the "…" and its separator before them
            // when any fold.
            const auto used = [&](usize first)
            {
                f32 total = 0.0f;
                usize items = 0;
                for (usize i = first; i < count; ++i)
                {
                    if (i > first)
                    {
                        total += separators[i - 1].x;
                        ++items;
                    }
                    total += segments[i].x;
                    ++items;
                }
                if (first > 0)
                {
                    total += overflow.x + overflowSeparator.x;
                    items += 2;
                }
                return items > 1 ? total + gap * static_cast<f32>(items - 1) : total;
            };
            usize first = 0;
            while (count > 0 && first + 1 < count && used(first) > content.width + 0.5f)
                ++first;
            m_folded = first;

            const bool reverse = IsRightToLeft();
            f32 at = 0.0f;
            const auto place = [&](Node& node, core::Float2 size, f32 extent)
            {
                const f32 height = core::Min(size.y, content.height);
                const f32 y = content.y + (content.height - height) * 0.5f;
                const f32 x = reverse ? content.x + content.width - at - extent : content.x + at;
                node.Arrange(Rect{x, y, core::Max(0.0f, extent), height});
                at += extent + gap;
            };
            const auto hide = [&](Node& node)
            {
                node.SetVisible(false);
                node.Arrange(Rect{content.x, content.y, 0.0f, 0.0f});
            };
            const bool folding = first > 0;
            m_overflow->SetVisible(folding);
            m_overflowSeparator->SetVisible(folding);
            if (folding)
            {
                place(*m_overflow, overflow, overflow.x);
                place(*m_overflowSeparator, overflowSeparator, overflowSeparator.x);
            }
            else
            {
                hide(*m_overflow);
                hide(*m_overflowSeparator);
            }
            for (usize i = 0; i < count; ++i)
            {
                if (i > 0)
                {
                    BreadcrumbSeparator& separator = *m_separators[i - 1];
                    if (i > first)
                    {
                        separator.SetVisible(true);
                        place(separator, separators[i - 1], separators[i - 1].x);
                    }
                    else
                        hide(separator);
                }
                BreadcrumbSegment& segment = *m_segments[i];
                if (i < first)
                {
                    hide(segment);
                    continue;
                }
                segment.SetVisible(true);
                // The current one, alone and still too long, takes what is left (its text cut short).
                const f32 extent = i + 1 == count ? core::Min(segments[i].x, content.width - at) : segments[i].x;
                place(segment, segments[i], extent);
            }
            KeepTabStop();
        }
        // The one tab stop goes where focus went among its items.
        void OnDescendantFocused(Node& focused) override
        {
            UIWidget::OnDescendantFocused(focused);
            for (Node* item : Stops())
                if (item == &focused)
                {
                    m_roving = item;
                    KeepTabStop();
                    return;
                }
        }
        // Left and Right (the other way right to left), Home and End move along its items.
        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Modifiers != 0)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            const core::Array<Node*> stops = Stops();
            if (stops.IsEmpty())
                return;
            usize at = stops.Size();
            for (usize i = 0; i < stops.Size(); ++i)
                if (stops[i]->IsFocused())
                    at = i;
            if (at == stops.Size())
                return;
            usize next = at;
            if (const i32 step = InlineStep(key); step != 0)
            {
                if (step < 0 && at > 0)
                    next = at - 1;
                else if (step > 0 && at + 1 < stops.Size())
                    next = at + 1;
            }
            else if (key == KeyCode::Home)
                next = 0;
            else if (key == KeyCode::End)
                next = stops.Size() - 1;
            else
                return;
            event.Handled = true;
            if (next != at)
                stops[next]->RequestFocus();
        }

    private:
        // What the keys move along, in order: the "…" while segments fold, then the shown segments.
        [[nodiscard]] core::Array<Node*> Stops() const
        {
            core::Array<Node*> out(MemoryAllocator());
            if (m_folded > 0)
                out.PushBack(m_overflow.Get());
            for (usize i = m_folded; i < m_segments.Size(); ++i)
                out.PushBack(m_segments[i].Get());
            return out;
        }
        // One of its items takes Tab: the one focus was last on, else the current segment.
        void KeepTabStop()
        {
            const core::Array<Node*> stops = Stops();
            bool held = false;
            for (Node* item : stops)
                held = held || item == m_roving;
            if (!held)
                m_roving = stops.IsEmpty() ? nullptr : stops.Back();
            m_overflow->SetTabFocusable(m_overflow.Get() == m_roving);
            for (const core::RefPtr<BreadcrumbSegment>& segment : m_segments)
                segment->SetTabFocusable(segment.Get() == m_roving);
        }
        // Its border box with its margins, as it would be with nothing in its way.
        [[nodiscard]] static core::Float2 Natural(Node& node)
        {
            LayoutConstraints constraints;
            constraints.Max = core::Float2{kUnbounded, kUnbounded};
            const core::Float2 size = node.Measure(constraints);
            const Thickness m = node.GetLayoutStyle().Margin;
            return core::Float2{size.x + m.TotalHorizontal(), size.y + m.TotalVertical()};
        }

        core::Array<BreadcrumbItem> m_items;
        core::Array<core::RefPtr<BreadcrumbSegment>> m_segments;
        core::Array<core::RefPtr<BreadcrumbSeparator>> m_separators; // [i - 1]: before segment i
        core::RefPtr<Button> m_overflow;
        core::RefPtr<BreadcrumbSeparator> m_overflowSeparator;
        core::RefPtr<Menu> m_menu;
        Node* m_roving = nullptr; // the item that takes Tab
        usize m_folded = 0;
        core::Function<void(const BreadcrumbItem&)> m_onNavigate;
    };
}
