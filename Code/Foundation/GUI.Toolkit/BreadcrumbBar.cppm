// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :breadcrumb_bar partition
//
// The breadcrumb bar shows where a view is, as segments from its root to the current one. Each
// segment carries its target (an id the application gets back), so the application keeps no table
// beside it. A click navigates on release. The last segment is the current one; it is styled as
// current and does nothing when clicked. When the segments do not fit, the bar keeps the current
// one and as many before it as fit, filling from the end, and folds the rest into a "…" button
// whose menu lists them. The bar is one tab stop: Left and Right move along it, and Return
// navigates. In right-to-left layout the segments run right to left, the chevrons turn, and Left
// and Right are mirrored. Given a children callback, each chevron (or Down on a segment) opens a
// menu of what lies in the segment before it, and choosing a child navigates to it. Every segment
// except the current one accepts drops of the application's types. When path editing is enabled, a
// click on the bar's empty end turns it into a field holding the current item's id (the path).
// Return gives the text to the application, and Escape or losing focus brings the segments back.
// History (back, forward, up) belongs to the view that holds the bar.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.toolkit:breadcrumb_bar;

import foundation.core;
import foundation.vg;
import foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    /// One segment of a breadcrumb bar: what it shows and what navigating to it means.
    struct BreadcrumbItem
    {
        core::String Id;      ///< what the application gets back (a folder's path, a node's id)
        core::String Text;    ///< what it shows
        core::String Tooltip; ///< shown on hover (the full path); empty for none
        core::String Icon;    ///< an icon's path, loaded as a button loads its icon; empty for none
    };

    class BreadcrumbBar;

    /// A segment: a button with the item's text and icon. Its style class is `crumb`, and the
    /// current segment also has `.current`.
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
        /// Returns "current" for the current segment and empty otherwise, so a screen reader
        /// describes it.
        [[nodiscard]] core::String GetAccessDescription() const
        {
            return HasClass(core::StringView(u8"current")) ? core::String(core::StringView(u8"current")) : core::String{};
        }

    protected:
        // A segment other than the current one accepts drops of the bar's types.
        void OnDragQuery(DropQuery& query) override;
        void OnDrop(const DragEvent& event) override;

    private:
        friend class BreadcrumbBar;
        BreadcrumbBar* m_bar = nullptr;
        usize m_index = 0;
    };

    /// The mark between two segments (`crumbseparator`): a chevron pointing toward the current one.
    /// When the bar has a children callback, it is a button (`.menu`) that opens the menu of what
    /// lies in the segment before it.
    class BreadcrumbSeparator : public Button
    {
        RTTI_OBJECT(BreadcrumbSeparator, Button)
    public:
        BreadcrumbSeparator()
        {
            SetTag(core::StringView(u8"crumbseparator"));
            SetTabFocusable(false); // from the keyboard, Down on the segment opens its menu
            SetHitTestVisible(false);
            SetMirrorOnRtl(true); // drawn pointing right; in right to left, it points left
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{8.0f, 10.0f}; }
        /// Returns the index of the segment whose children its menu lists.
        [[nodiscard]] usize GetOwner() const noexcept { return m_owner; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            Button::OnDraw(ctx, localBounds);
            const Rect c = GetContentBounds();
            const f32 cx = c.x + c.width * 0.5f;
            const f32 cy = c.y + c.height * 0.5f;
            vg::PathBuilder chevron;
            chevron.MoveTo(cx - 1.5f, cy - 3.5f);
            chevron.LineTo(cx + 2.0f, cy);
            chevron.LineTo(cx - 1.5f, cy + 3.5f);
            ctx.VG().StrokePath(chevron.ToPath(), GetTextColor(), vg::StrokeStyle(1.25f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
        }

    private:
        friend class BreadcrumbBar;
        usize m_owner = 0;
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
                    s.MinWidth = LayoutLength::Px(0.0f); // the segments fold, so they never hold the bar at their full length
                });
                SetPadding(Thickness{4.0f, 2.0f, 4.0f, 2.0f});
            }
            m_overflow = core::MakeRef<Button>(MemoryAllocator());
            m_overflow->SetTag(core::StringView(u8"crumboverflow"));
            m_overflow->SetVisible(false);
            BreadcrumbBar* self = this;
            m_overflow->SetOnClick([self]() { self->OpenOverflow(); });
            AddChild(m_overflow.Get());
            m_overflowSeparator = MakeSeparator();
            m_overflowSeparator->SetVisible(false);
        }

        /// Sets the segments, from the root to the current one (the last).
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
            if (m_editingPath)
                EndPathEdit(false);
            BreadcrumbBar* self = this;
            for (usize i = 0; i < items.Size(); ++i)
            {
                const BreadcrumbItem& item = items[i];
                m_items.PushBack(item);
                if (i > 0)
                {
                    m_separators.PushBack(MakeSeparator());
                    m_separators.Back()->m_owner = i - 1;
                }
                core::RefPtr<BreadcrumbSegment> segment = core::MakeRef<BreadcrumbSegment>(MemoryAllocator());
                segment->m_bar = this;
                segment->m_index = i;
                segment->SetText(item.Text.AsView());
                if (Label* text = segment->GetTextPart())
                {
                    text->SetTextOverflow(TextOverflow::Ellipsis); // for the current segment when even it alone does not fit
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
        /// Sets the segments from a `/` path; each folder's id is the path up to it. A leading `/`
        /// is a segment of its own (the root), as is a drive (`C:`); empty parts are skipped.
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

        /// Called with the item navigated to, whether a segment was clicked, chosen from the
        /// overflow menu, or activated with Return.
        void SetOnNavigate(core::Function<void(const BreadcrumbItem&)> callback) { m_onNavigate = core::Move(callback); }
        /// Navigates to segment `index` as a click does. Returns false for the current segment or an
        /// index with no segment.
        bool Navigate(usize index)
        {
            if (index + 1 >= m_items.Size())
                return false; // the view is already at the current segment
            if (m_onNavigate)
            {
                const BreadcrumbItem item = m_items[index]; // the callback may set new items
                m_onNavigate(item);
            }
            return true;
        }

        /// Sets the callback that lists an item's children, which its chevron's menu shows. With a
        /// callback the chevrons are buttons; without one (the default) they are plain marks.
        void SetOnChildren(core::Function<void(const BreadcrumbItem& of, core::Array<BreadcrumbItem>& children)> callback)
        {
            m_onChildren = core::Move(callback);
            const bool menus = static_cast<bool>(m_onChildren);
            const auto show = [menus](BreadcrumbSeparator& separator)
            {
                separator.SetHitTestVisible(menus);
                if (menus)
                    separator.AddClass(core::StringView(u8"menu"));
                else
                    separator.RemoveClass(core::StringView(u8"menu"));
            };
            show(*m_overflowSeparator);
            for (const core::RefPtr<BreadcrumbSeparator>& separator : m_separators)
                show(*separator);
        }
        /// Opens the menu of segment `index`'s children below `anchor` (its chevron, else the
        /// segment); choosing one navigates to it. Returns false with no callback or no children.
        bool OpenChildren(usize index, Node& anchor)
        {
            if (!m_onChildren || index >= m_items.Size())
                return false;
            m_children.Clear();
            m_onChildren(m_items[index], m_children);
            if (m_children.IsEmpty())
                return false;
            m_menu = core::MakeRef<Menu>(MemoryAllocator());
            ShareTextStyle(*m_menu, false);
            m_menu->SetAutoWidth(true);
            BreadcrumbBar* self = this;
            for (usize i = 0; i < m_children.Size(); ++i)
                (void)m_menu->AddItem(m_children[i].Text.AsView(), [self, i]()
                {
                    if (self->m_onNavigate && i < self->m_children.Size())
                    {
                        const BreadcrumbItem child = self->m_children[i];
                        self->m_onNavigate(child);
                    }
                });
            m_menu->OpenBelow(*this, anchor);
            return true;
        }
        /// Returns the menu open now, the overflow menu or a chevron's, or null.
        [[nodiscard]] Menu* GetOpenMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }

        /// Segments other than the current one accept drops whose type is among `types`
        /// (space-separated).
        void SetSegmentDropTypes(core::StringView types) { m_dropTypes = core::String(types); }
        /// Called on a drop on a segment, with its item, what was dropped and the drop effect.
        void SetOnSegmentDrop(core::Function<void(const BreadcrumbItem&, const DragPayload&, DropEffect)> callback)
        {
            m_onDrop = core::Move(callback);
        }

        /// Sets whether a click on the bar's empty end edits the path as text (off by default).
        void SetPathEditable(bool editable) noexcept { m_pathEditable = editable; }
        [[nodiscard]] bool IsPathEditable() const noexcept { return m_pathEditable; }
        /// Called with the typed text when Return ends the edit; the application decides whether to
        /// navigate.
        void SetOnPathSubmitted(core::Function<void(core::StringView)> callback) { m_onPathSubmitted = core::Move(callback); }
        /// Turns the bar into a field holding the current item's id, all selected.
        void BeginPathEdit()
        {
            if (m_editingPath)
                return;
            if (!m_field)
            {
                m_field = core::MakeRef<TextField>(MemoryAllocator());
                m_field->AddClass(core::StringView(u8"path"));
                BreadcrumbBar* self = this;
                m_field->SetOnSubmit([self](core::StringView) { self->EndPathEdit(true); });
                (void)m_field->AddEventListener(EventType::FocusLost, [self](const Event&)
                {
                    // When focus moves elsewhere, the segments come back, but not when it moves to
                    // the field's own context menu.
                    if (const Menu* menu = self->m_field->GetContextMenu(); menu != nullptr && menu->IsOpen())
                        return;
                    self->EndPathEdit(false);
                });
                AddChild(m_field.Get());
            }
            m_editingPath = true;
            m_field->SetText(m_items.IsEmpty() ? core::StringView{} : m_items.Back().Id.AsView());
            m_field->SetDisplayed(true);
            m_field->GetModel().SelectAll();
            m_field->RequestFocus();
            RequestLayout();
        }
        /// Ends the path edit. With `submit`, the text goes to the application.
        void EndPathEdit(bool submit)
        {
            if (!m_editingPath)
                return;
            m_editingPath = false;
            const core::String typed(m_field->GetText());
            const bool hadFocus = m_field->IsFocused();
            m_field->SetDisplayed(false);
            RequestLayout();
            if (hadFocus && m_roving != nullptr)
            {
                m_roving->SetVisible(true); // it was hidden while the field showed; the layout places it
                m_roving->RequestFocus();
            }
            if (submit && m_onPathSubmitted)
                m_onPathSubmitted(typed.AsView());
        }
        [[nodiscard]] bool IsEditingPath() const noexcept { return m_editingPath; }
        [[nodiscard]] TextField* GetPathField() const noexcept { return m_field.Get(); }

        /// Returns segment `index`, counting from the root.
        [[nodiscard]] BreadcrumbSegment& SegmentAt(usize index) const noexcept { return *m_segments[index]; }
        /// Returns the separator before segment `index` (from 1).
        [[nodiscard]] BreadcrumbSeparator& SeparatorBefore(usize index) const noexcept { return *m_separators[index - 1]; }
        /// Returns how many segments, counting from the root, folded into the overflow menu; 0 when
        /// all show.
        [[nodiscard]] usize FoldedCount() const noexcept { return m_folded; }
        [[nodiscard]] Button& GetOverflowButton() const noexcept { return *m_overflow; }
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
            // Asks for the natural size of every segment and separator together, which is what the
            // bar needs for all of them to fit, and the height of the tallest.
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
        // The bar lays out its segments itself: as many as fit, from the end, with the rest folded.
        void OnLayout() override
        {
            const Rect content = GetContentBounds();
            const f32 gap = GetLayoutStyle().ColumnGap;
            const usize count = m_segments.Size();
            const auto hideAll = [&]()
            {
                const auto hide = [&](Node& node)
                {
                    node.SetVisible(false);
                    node.Arrange(Rect{content.x, content.y, 0.0f, 0.0f});
                };
                hide(*m_overflow);
                hide(*m_overflowSeparator);
                for (const core::RefPtr<BreadcrumbSegment>& segment : m_segments)
                    hide(*segment);
                for (const core::RefPtr<BreadcrumbSeparator>& separator : m_separators)
                    hide(*separator);
            };
            if (m_editingPath)
            {
                // The field covers the whole bar, and the segments are hidden.
                hideAll();
                m_field->Arrange(content);
                return;
            }
            core::Array<core::Float2> segments(MemoryAllocator());
            core::Array<core::Float2> separators(MemoryAllocator());
            for (usize i = 0; i < count; ++i)
            {
                segments.PushBack(Natural(*m_segments[i]));
                if (i > 0)
                    separators.PushBack(Natural(*m_separators[i - 1]));
            }
            // Use "…" where the font draws it, else three full stops (a baked atlas may lack it).
            m_overflow->SetText(EllipsisFor(m_overflow->GetFont()));
            const core::Float2 overflow = Natural(*m_overflow);
            const core::Float2 overflowSeparator = Natural(*m_overflowSeparator);
            // The width that segments `first` to the last take, plus the "…" and its separator
            // before them when any fold.
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
            m_overflowSeparator->m_owner = first > 0 ? first - 1 : 0;
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
                // When the current segment is alone and still too long, it takes what is left and its
                // text is cut short.
                const f32 extent = i + 1 == count ? core::Min(segments[i].x, content.width - at) : segments[i].x;
                place(segment, segments[i], extent);
            }
            KeepTabStop();
        }
        // The bar's one tab stop moves to whichever of its items took focus.
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
        // A click on the bar's empty end edits the path, when path editing is enabled.
        void OnMouseClick(const MouseEvent& event) override
        {
            if (event.Target != this || event.Button != MouseButton::Left || !m_pathEditable || m_editingPath)
                return;
            event.Handled = true;
            BeginPathEdit();
        }
        // Left and Right (swapped in right to left), Home and End move along the items; Down opens
        // a segment's children; Escape leaves the path edit.
        void OnKeyDown(const KeyEvent& event) override
        {
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (m_editingPath)
            {
                if (key == KeyCode::Escape && event.Modifiers == 0)
                {
                    EndPathEdit(false);
                    event.Handled = true;
                }
                return;
            }
            if (event.Modifiers != 0)
                return;
            if (key == KeyCode::Down)
            {
                for (usize i = 0; i < m_segments.Size(); ++i)
                    if (m_segments[i]->IsFocused())
                    {
                        Node* anchor = i + 1 < m_segments.Size() && m_separators[i]->IsVisible() ? static_cast<Node*>(m_separators[i].Get())
                                                                                                    : static_cast<Node*>(m_segments[i].Get());
                        if (OpenChildren(i, *anchor))
                            event.Handled = true;
                        return;
                    }
                return;
            }
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
        friend class BreadcrumbSegment;
        core::RefPtr<BreadcrumbSeparator> MakeSeparator()
        {
            core::RefPtr<BreadcrumbSeparator> separator = core::MakeRef<BreadcrumbSeparator>(MemoryAllocator());
            BreadcrumbSeparator* made = separator.Get();
            BreadcrumbBar* self = this;
            separator->SetOnClick([self, made]() { (void)self->OpenChildren(made->m_owner, *made); });
            if (m_onChildren)
            {
                separator->SetHitTestVisible(true);
                separator->AddClass(core::StringView(u8"menu"));
            }
            AddChild(separator.Get());
            return separator;
        }
        // Returns whether segment `index` accepts a drop of `type`: it must be one of the bar's types,
        // and the segment must not be the current one.
        [[nodiscard]] bool TakesDrop(usize index, core::StringView type) const
        {
            if (index + 1 >= m_items.Size() || m_dropTypes.IsEmpty())
                return false;
            core::StringView rest = core::Trim(m_dropTypes.AsView());
            while (rest.Size() != 0)
            {
                usize end = 0;
                while (end < rest.Size() && rest[end] != u8' ')
                    ++end;
                if (rest.SubStr(0, end) == type)
                    return true;
                rest = core::Trim(rest.SubStr(end, rest.Size() - end));
            }
            return false;
        }
        void Dropped(usize index, const DragEvent& event)
        {
            if (m_onDrop && index < m_items.Size())
            {
                const BreadcrumbItem item = m_items[index];
                m_onDrop(item, event.Payload, event.Effect);
            }
        }
        // Returns the items the keys move along, in order: the "…" while segments fold, then the
        // shown segments.
        [[nodiscard]] core::Array<Node*> Stops() const
        {
            core::Array<Node*> out(MemoryAllocator());
            if (m_folded > 0)
                out.PushBack(m_overflow.Get());
            for (usize i = m_folded; i < m_segments.Size(); ++i)
                out.PushBack(m_segments[i].Get());
            return out;
        }
        // Gives the tab stop to one item: the one focus was last on, else the current segment.
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
        // Returns the node's natural border box with its margins, rounded up to whole pixels across.
        // Once placed on the pixel grid, a box a fraction too narrow would cut its text with an
        // ellipsis.
        [[nodiscard]] static core::Float2 Natural(Node& node)
        {
            LayoutConstraints constraints;
            constraints.Max = core::Float2{kUnbounded, kUnbounded};
            const core::Float2 size = node.Measure(constraints);
            const Thickness m = node.GetLayoutStyle().Margin;
            return core::Float2{core::Ceil(size.x + m.TotalHorizontal()), size.y + m.TotalVertical()};
        }

        core::Array<BreadcrumbItem> m_items;
        core::Array<core::RefPtr<BreadcrumbSegment>> m_segments;
        core::Array<core::RefPtr<BreadcrumbSeparator>> m_separators; // [i - 1] is the separator before segment i
        core::RefPtr<Button> m_overflow;
        core::RefPtr<BreadcrumbSeparator> m_overflowSeparator;
        core::RefPtr<Menu> m_menu;
        core::RefPtr<TextField> m_field; // holds the path while it is edited
        Node* m_roving = nullptr; // the item that takes Tab
        usize m_folded = 0;
        bool m_pathEditable = false;
        bool m_editingPath = false;
        core::String m_dropTypes;
        core::Array<BreadcrumbItem> m_children; // the children the open chevron's menu lists
        core::Function<void(const BreadcrumbItem&)> m_onNavigate;
        core::Function<void(const BreadcrumbItem&, core::Array<BreadcrumbItem>&)> m_onChildren;
        core::Function<void(const BreadcrumbItem&, const DragPayload&, DropEffect)> m_onDrop;
        core::Function<void(core::StringView)> m_onPathSubmitted;
    };

    inline void BreadcrumbSegment::OnDragQuery(DropQuery& query)
    {
        if (m_bar != nullptr && m_bar->TakesDrop(m_index, query.Payload.Type.AsView()))
            query.Accept();
    }
    inline void BreadcrumbSegment::OnDrop(const DragEvent& event)
    {
        if (m_bar != nullptr)
            m_bar->Dropped(m_index, event);
    }
}
