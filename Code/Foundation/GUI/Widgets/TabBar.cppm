// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tab_bar partition
//
// TabBar: a strip of tabs, separate from the container that shows pages (TabView). Each tab is a
// real element: a ButtonBase with text and icon parts plus a close part. A press selects a tab,
// which `:checked` then styles. The arrow keys, Home and End move along the strip; Ctrl+Tab and
// Ctrl+Shift+Tab (and Ctrl+Page Down and Ctrl+Page Up) cycle round it.
//
// Closing is a request: the close part or a middle click sends `tabclose` (a TabEvent), and the
// tab is removed unless a listener handles the event. The bar's policy decides which tabs show
// their close part: none, the selected tab, the selected and hovered tabs, or all of them.
//
// With `reorderable`, a tab dragged across a neighbour's midpoint swaps places with it. In a strip
// that has a `group`, a tab moved past the drag threshold in any direction becomes a drag (type
// `gui/tab`) straight away. Every strip of the group accepts it, its own strip included (as a
// reorder, when `reorderable`), and a mark shows where it would land. Between tab views, the
// tab's page moves with it.
//
// When the tabs pass the strip's end the strip scrolls: the wheel scrolls it, and it scrolls to
// the selected tab. A button at the end lists all the tabs, shown while they overflow or always
// (`list-display`). With `close-all`, a part after the list button asks to close every closable
// tab.
//
// A change the user makes sends `change`; a change from code only notifies bindings, as with a
// button.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:tab_bar;

import foundation.core; // Function, Move, RefPtr, Array, String, Abs, Max
import foundation.vg;   // PathBuilder, StrokeStyle
import :reactive;       // Observable, which bindings follow
import :rect;
import :event;
import :draw_context;
import :layout_style;
import :ui_widget;
import :panel;
import :text; // TextHAlign
import :button_base;
import :disclosure;
import :menu;
import :draw_context;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    class TabBar;

    /// Which tabs show their close part.
    enum class TabCloseDisplay : u8
    {
        Never,    ///< no tab; tabs close only from code
        Selected, ///< the selected tab
        Hover,    ///< the selected tab and the hovered tab
        Always,   ///< every closable tab
    };

    /// When the strip's list button shows.
    enum class TabListDisplay : u8
    {
        Overflow, ///< while the tabs overflow the strip
        Always,   ///< always, for a strip whose button does more than list (as in docking)
    };

    // Draws a cross in `color` filling `box`, as the close parts show.
    inline void DrawTabCross(DrawContext& ctx, const Rect& box, Color color)
    {
        const f32 s = core::Min(box.width, box.height) * 0.5f;
        const f32 cx = box.x + box.width * 0.5f;
        const f32 cy = box.y + box.height * 0.5f;
        vg::PathBuilder cross;
        cross.MoveTo(cx - s, cy - s);
        cross.LineTo(cx + s, cy + s);
        cross.MoveTo(cx + s, cy - s);
        cross.LineTo(cx - s, cy + s);
        ctx.VG().StrokePath(cross.ToPath(), color, vg::StrokeStyle(1.5f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
    }

    /// A tab's close part (`tab > close`): a cross drawn in the part's text colour.
    class TabCloseButton : public ButtonBase
    {
        RTTI_OBJECT(TabCloseButton, ButtonBase)
    public:
        TabCloseButton()
        {
            SetTag(core::StringView(u8"close"));
            SetTabFocusable(false); // the keyboard closes from the tab; the close part is not a focus stop
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{10.0f, 10.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            ButtonBase::OnDraw(ctx, localBounds);
            DrawTabCross(ctx, GetContentBounds(), GetTextColor());
        }
        // The close part handles its own press, so the tab under it neither selects nor starts a drag.
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button == MouseButton::Left)
                event.Handled = true;
        }
    };

    /// One tab: a ButtonBase with text and icon, and a close part.
    class Tab : public ButtonBase
    {
        RTTI_OBJECT(Tab, ButtonBase)
    public:
        Tab();

        /// Sets whether the tab can be closed, with its close part (shown as the bar's policy says)
        /// or a middle click.
        void SetClosable(bool closable)
        {
            m_closable = closable;
            UpdateClose();
        }
        [[nodiscard]] bool IsClosable() const noexcept { return m_closable; }
        [[nodiscard]] bool IsSelected() const noexcept { return m_selected; }
        [[nodiscard]] bool IsCurrentInGroup() const override { return m_selected; }
        [[nodiscard]] TabCloseButton* GetClosePart() const noexcept { return m_close.Get(); }
        /// Returns the bar the tab is in, or null when it is in none.
        [[nodiscard]] TabBar* GetBar() const;

        /// Sets what the tab shows (a TabView's page); not owned.
        void SetPage(Node* page) noexcept { m_page = page; }
        [[nodiscard]] Node* GetPage() const noexcept { return m_page; }

        [[nodiscard]] u32 GetWidgetStates() const override { return m_selected ? WidgetStateChecked : WidgetStateNone; }
        // In a reorderable strip with no group, the tab's press reorders it, so a drag is not
        // offered to the nodes that hold it. In a group's strip the press starts a drag, which the
        // tab provides itself.
        [[nodiscard]] bool OwnsPressDrag(core::Float2) const override;
        // A drag resting on the tab selects it.
        bool OnDragHoverOpen(core::Float2) override;
        /// Starts a drag when the tab is in a group's strip: from the pointer moving past the
        /// threshold, held where it was pressed, or picked up from the keyboard (`drag.pick`).
        bool OnDragRequested(DragRequest& request) override;
        /// Fills the drag payload for this tab when it is torn out of its strip.
        void FillTabPayload(DragPayload& payload);
        /// Creates a torn tab's ghost: its title in a `dragpreview.tab` at the tab's own size, held
        /// where it was grabbed so it does not jump beside the pointer.
        [[nodiscard]] core::RefPtr<Node> CreateDragPreview(const DragPayload& payload) override;

    protected:
        bool OnActivate() override;
        void OnMouseDown(const MouseEvent& event) override;
        void OnMouseMove(const MouseEvent& event) override;
        void OnMouseUp(const MouseEvent& event) override
        {
            ButtonBase::OnMouseUp(event);
            EndDrag();
        }
        void OnPointerCancel(const MouseEvent& event) override
        {
            ButtonBase::OnPointerCancel(event);
            EndDrag();
        }
        void OnPointerCaptureLost() override { EndDrag(); }
        void OnKeyDown(const KeyEvent& event) override;
        void OnDragEnd(const DragResult&) override;
        void OnPointerStateChange() override
        {
            ButtonBase::OnPointerStateChange();
            UpdateClose(); // under the hover policy the close part shows while hovered
        }

    private:
        friend class TabBar;
        void SetSelectedState(bool selected)
        {
            if (selected == m_selected)
                return;
            m_selected = selected;
            MarkStyleStateDirty(StateChecked);
            UpdateClose();
            Invalidate();
        }
        void UpdateClose();
        void EndDrag()
        {
            m_pressing = false;
            if (m_dragging)
            {
                m_dragging = false;
                RemoveClass(core::StringView(u8"dragging"));
            }
        }

        core::RefPtr<TabCloseButton> m_close;
        Node* m_page = nullptr;
        bool m_closable = true;
        bool m_selected = false;
        bool m_pressing = false; // a left press on the tab, not yet released
        bool m_dragging = false; // the press has moved far enough to reorder
        f32 m_pressX = 0.0f;
        core::RefPtr<TabBar> m_tornFrom; // the bar that was told of the tear, and is told again when the drag ends
    };

    // The row that holds the tabs. It scrolls horizontally with its scroll bar hidden.
    class TabStrip : public Panel
    {
        RTTI_OBJECT(TabStrip, Panel)
    public:
        explicit TabStrip(TabBar& bar);
        void OnChildrenChanged() override;

    private:
        TabBar& m_bar; // the parent bar, which owns this strip
    };

    // The button at the strip's end that lists every tab; shown while the tabs overflow, or always.
    class TabListButton : public ButtonBase
    {
        RTTI_OBJECT(TabListButton, ButtonBase)
    public:
        TabListButton()
        {
            SetTag(core::StringView(u8"tablist"));
            SetTabFocusable(false); // the arrow keys and Ctrl+Tab already reach every tab
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{12.0f, 12.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            ButtonBase::OnDraw(ctx, localBounds);
            DrawDisclosure(ctx, GetContentBounds(), true, GetTextColor(), IsRightToLeft());
        }
    };

    // The button after the list button that closes every closable tab (`closeall`). It exists with
    // `close-all` and shows while some tab is closable.
    class TabCloseAllButton : public ButtonBase
    {
        RTTI_OBJECT(TabCloseAllButton, ButtonBase)
    public:
        TabCloseAllButton()
        {
            SetTag(core::StringView(u8"closeall"));
            SetTabFocusable(false); // the keyboard closes one tab at a time
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{10.0f, 10.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            ButtonBase::OnDraw(ctx, localBounds);
            DrawTabCross(ctx, GetContentBounds(), GetTextColor());
        }
    };

    class TabBar : public UIWidget
    {
        RTTI_OBJECT(TabBar, UIWidget)
    public:
        TabBar()
        {
            SetTag(core::StringView(u8"tabbar"));
            {
                StyleWriteScope defaults(*this);
                EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Row;
                    s.AlignItems = Align::Stretch;
                });
            }
            m_strip = core::MakeRef<TabStrip>(MemoryAllocator(), *this);
            AddChild(m_strip.Get());
            m_list = core::MakeRef<TabListButton>(MemoryAllocator());
            m_list->SetDisplayed(false);
            TabBar* self = this;
            m_list->SetOnClick([self]() { self->OpenTabList(); });
            AddChild(m_list.Get());
            m_closeAllPart = core::MakeRef<TabCloseAllButton>(MemoryAllocator());
            m_closeAllPart->SetDisplayed(false);
            m_closeAllPart->SetOnClick([self]() { self->CloseAll(); });
            AddChild(m_closeAllPart.Get());
        }

        // === Tabs ===
        /// Adds a tab with `text` at `index`, or last when `index` is past the end. The first tab
        /// added is selected.
        Tab* InsertTab(i32 index, core::StringView text)
        {
            auto tab = core::MakeRef<Tab>(MemoryAllocator());
            tab->SetText(text);
            m_strip->AddChildAt(tab.Get(), index < 0 ? m_strip->ChildCount() : core::Min(static_cast<usize>(index), m_strip->ChildCount()));
            m_selectedAt = core::Max(0, IndexOf(m_selected)); // the new tab may have gone before the selected one
            return tab.Get();
        }
        Tab* AddTab(core::StringView text) { return InsertTab(-1, text); }
        /// Removes a tab. If it was selected, the tab after it becomes selected, or else the one
        /// before.
        void RemoveTab(i32 index)
        {
            if (Tab* tab = GetTab(index))
                m_strip->RemoveChild(tab);
        }
        [[nodiscard]] i32 GetTabCount() const noexcept { return static_cast<i32>(m_strip->ChildCount()); }
        [[nodiscard]] Tab* GetTab(i32 index) const
        {
            return index >= 0 && index < GetTabCount() ? core::Cast<Tab>(m_strip->GetChildAt(static_cast<usize>(index))) : nullptr;
        }
        [[nodiscard]] i32 IndexOf(const Tab* tab) const
        {
            const usize at = tab != nullptr ? m_strip->GetChildIndex(tab) : kInvalidIndex;
            return at == kInvalidIndex ? -1 : static_cast<i32>(at);
        }
        /// Moves a tab to `to`. As a change from code it is reported only to the moved callback.
        void MoveTab(i32 from, i32 to)
        {
            Tab* tab = GetTab(from);
            if (tab == nullptr || to < 0 || to >= GetTabCount() || to == from)
                return;
            m_strip->SetChildIndex(tab, static_cast<usize>(to));
            m_selectedAt = core::Max(0, IndexOf(m_selected));
            if (m_onMoved)
                m_onMoved(from, to);
        }

        // === Selection ===
        /// Returns the selected tab's index, or -1 for none. A binding that reads it follows later
        /// changes.
        [[nodiscard]] i32 GetSelectedIndex() const
        {
            m_selectedChanged.Track();
            return IndexOf(m_selected);
        }
        [[nodiscard]] Tab* GetSelectedTab() const noexcept { return m_selected; }
        /// Selects a tab from code, without events. An index past the last tab is kept until that
        /// tab exists, because markup's `selected` is read before its tabs are made.
        void SetSelectedIndex(i32 index)
        {
            m_pendingSelect = index >= GetTabCount() ? index : -1;
            if (m_pendingSelect < 0)
                Select(GetTab(index), false);
        }
        /// Selects a tab as the user does, sending `change` when the selection changes.
        void SelectByUser(i32 index) { Select(GetTab(index), true); }
        /// Moves the selection by `step`, wrapping round the strip as Ctrl+Tab does, on the user's
        /// behalf.
        void CycleSelection(i32 step)
        {
            const i32 count = GetTabCount();
            if (count == 0)
                return;
            const i32 from = core::Max(0, IndexOf(m_selected));
            SelectByUser(((from + step) % count + count) % count);
            if (m_selected != nullptr && IsFocusWithin())
                m_selected->RequestFocus();
        }

        // === Closing ===
        /// Sets which tabs show their close part.
        void SetCloseDisplay(TabCloseDisplay display)
        {
            m_closeDisplay = display;
            for (i32 i = 0; i < GetTabCount(); ++i)
                GetTab(i)->UpdateClose();
        }
        [[nodiscard]] TabCloseDisplay GetCloseDisplay() const noexcept { return m_closeDisplay; }
        /// Sets the close policy from "never", "selected", "hover" or "always" (the `close-display`
        /// property of markup and style sheets).
        void SetCloseDisplayName(const core::String& name)
        {
            static constexpr core::StringView kNames[] = {u8"never", u8"selected", u8"hover", u8"always"};
            TabCloseDisplay display = TabCloseDisplay::Never;
            for (usize i = 0; i < 4; ++i)
                if (core::Trim(name.AsView()) == kNames[i])
                    display = static_cast<TabCloseDisplay>(i);
            SetCloseDisplay(display);
        }
        [[nodiscard]] core::String GetCloseDisplayName() const
        {
            static constexpr core::StringView kNames[] = {u8"never", u8"selected", u8"hover", u8"always"};
            return core::String(kNames[static_cast<usize>(m_closeDisplay)]);
        }
        /// Asks to close every closable tab, as the close-all part does. It goes from the last tab to
        /// the first and makes each request as that tab's close part would. It walks by index
        /// rather than by tab object, because an owner may rebuild its tabs after each close
        /// (docking does), and the tabs before the one closed keep their indices.
        void CloseAll()
        {
            const core::RefPtr<TabBar> keep(this); // an owner may release the bar during a close
            for (i32 i = GetTabCount(); i-- > 0;)
                if (i < GetTabCount() && GetTab(i)->IsClosable())
                    RequestClose(i);
        }
        /// Sets whether a part after the list button closes every closable tab (the `close-all`
        /// property of markup and style sheets). The part shows while some tab is closable.
        void SetCloseAll(bool shown)
        {
            m_closeAll = shown;
            UpdateCloseAll();
        }
        [[nodiscard]] bool HasCloseAll() const noexcept { return m_closeAll; }
        [[nodiscard]] Node* GetCloseAllPart() const noexcept { return m_closeAllPart.Get(); }
        /// Asks to close a tab, as its close part does. The bar sends `tabclose`, and unless a
        /// listener handles it, the tab is removed. A close handler (a TabView sets one) takes over
        /// the request instead.
        void RequestClose(i32 index)
        {
            Tab* tab = GetTab(index);
            if (tab == nullptr || !tab->IsClosable())
                return;
            if (m_closeHandler)
            {
                m_closeHandler(index);
                return;
            }
            const TabEvent request(EventType::TabClose, this, index);
            SendEvent(request);
            if (!request.Handled)
                RemoveTab(index);
        }

        // === Reordering ===
        /// Sets whether tabs reorder: a tab dragged across a neighbour's midpoint swaps places with
        /// it. In a group's strip the tab is instead dropped back into the strip at a new place.
        void SetReorderable(bool reorderable) noexcept { m_reorderable = reorderable; }
        [[nodiscard]] bool IsReorderable() const noexcept { return m_reorderable; }

        // === Between strips ===
        /// Sets the group. Strips of the same group accept each other's tabs, torn out and dropped;
        /// an empty group shares with none.
        void SetGroup(const core::String& group) { m_group = group; }
        [[nodiscard]] core::String GetGroup() const { return m_group; }
        /// While a tab is dragged over the strip, returns where it would go if dropped, or -1.
        [[nodiscard]] i32 GetDropIndex() const noexcept { return m_dropIndex; }
        /// Returns whether the bar accepts a dragged tab (a torn-out tab of its group), and if so
        /// puts the mark at `index`. A TabView calls this for a drop anywhere over it.
        bool OfferTab(DropQuery& query, i32 index)
        {
            const Tab* tab = DraggedTab(query.Payload);
            const TabBar* from = tab != nullptr ? tab->GetBar() : nullptr;
            if (from == nullptr || m_group.IsEmpty() || from->m_group != m_group || (from == this && !m_reorderable))
                return false;
            query.Accept(DropEffect::Move);
            query.Place = index;
            if (m_onTabOffered)
            {
                m_onTabOffered(query, *const_cast<Tab*>(tab), index);
                if (query.Refused)
                {
                    SetDropIndex(-1);
                    return true; // refused: the query stops here with the reason
                }
                if (query.Effect == DropEffect::None)
                {
                    SetDropIndex(-1);
                    return false; // passed on: the strip's ancestors are asked next
                }
            }
            SetDropIndex(index);
            return true;
        }
        /// Clears the mark when the drag leaves.
        void ClearTabMark() { SetDropIndex(-1); }
        /// Drops the dragged tab at the marked place.
        void DropTab(const DragPayload& payload);
        /// Sets the callback that takes the page of a tab dropped here from another view (used by
        /// a TabView). The callback receives the page and the index, and the tab follows the page.
        void SetPageAdopter(core::Function<void(Node& page, i32 index)> adopter) { m_pageAdopter = core::Move(adopter); }
        /// Sets the callback asked when a tab of the group is offered at `index`, after the query
        /// has been accepted. It may refuse the tab and say why, pass it on by setting the effect
        /// back to none (the strip's ancestors are then asked), or describe what the drop would do
        /// in the query's Reason. An owner that tracks tab placement elsewhere (docking) answers
        /// with its own rules.
        void SetOnTabOffered(core::Function<void(DropQuery& query, Tab& tab, i32 index)> offered) { m_onTabOffered = core::Move(offered); }
        /// Sets a callback that handles dropped tabs instead of the bar, which then moves nothing.
        /// `index` counts every tab in its current place, including the dropped tab when it belongs
        /// to this bar.
        void SetTabTaker(core::Function<void(Tab& tab, i32 index)> taker) { m_tabTaker = core::Move(taker); }
        /// Sets the callback told when one of this bar's tabs is torn out as a drag (`torn` true)
        /// and when that drag ends, dropped or not (by then the tab may be in another bar). An
        /// owner may hide or dim the tab meanwhile. A tab that is not displayed is not a drop place.
        void SetOnTabTorn(core::Function<void(Tab& tab, bool torn)> torn) { m_onTabTorn = core::Move(torn); }
        /// Sets the callback told how the drag of a tab torn out of this bar ended, after the torn
        /// callback has reported the end. It lets an owner act on a tab released outside every
        /// window; docking floats it.
        void SetOnTabDragEnded(core::Function<void(Tab& tab, const DragResult& result)> ended) { m_onTabDragEnded = core::Move(ended); }

        // === Callbacks ===
        /// Sets the callback for the user selecting a tab (as the `change` event reports).
        void SetOnSelected(core::Function<void(i32)> callback) { m_onSelected = core::Move(callback); }
        /// Sets the callback for every selection change, by the user or by code; whatever shows the
        /// tab's page follows it.
        void SetOnSelectionChanged(core::Function<void(i32)> callback) { m_onSelectionChanged = core::Move(callback); }
        /// Sets the callback for a tab moved by a drag or by MoveTab.
        void SetOnTabMoved(core::Function<void(i32 from, i32 to)> callback) { m_onMoved = core::Move(callback); }
        /// Sets the callback for a tab leaving the bar, closed or removed by code.
        void SetOnTabRemoved(core::Function<void(Tab&)> callback) { m_onRemoved = core::Move(callback); }
        /// Sets a handler that takes over close requests (a TabView sets one). The bar then neither
        /// sends `tabclose` nor removes the tab.
        void SetCloseHandler(core::Function<void(i32)> handler) { m_closeHandler = core::Move(handler); }

        [[nodiscard]] Node* GetStrip() const noexcept { return m_strip.Get(); }
        [[nodiscard]] Node* GetListButton() const noexcept { return m_list.Get(); }
        /// Sets when the list button shows: while the tabs overflow (the default) or always.
        void SetListDisplay(TabListDisplay display)
        {
            m_listDisplay = display;
            RequestLayout();
        }
        [[nodiscard]] TabListDisplay GetListDisplay() const noexcept { return m_listDisplay; }
        /// Sets the list button policy from "overflow" or "always" (the `list-display` property of
        /// markup and style sheets).
        void SetListDisplayName(const core::String& name)
        {
            SetListDisplay(core::Trim(name.AsView()) == core::StringView(u8"always") ? TabListDisplay::Always : TabListDisplay::Overflow);
        }
        [[nodiscard]] core::String GetListDisplayName() const
        {
            return core::String(m_listDisplay == TabListDisplay::Always ? core::StringView(u8"always") : core::StringView(u8"overflow"));
        }
        /// Returns the menu listing every tab while it is open, or null otherwise.
        [[nodiscard]] Menu* GetTabListMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }
        /// Returns the host for markup's children, which become the tabs.
        [[nodiscard]] Node* GetChildHost() override { return m_strip.Get(); }

        /// Lists every tab in a menu below the list button; picking one selects it.
        void OpenTabList()
        {
            m_menu = core::MakeRef<Menu>(MemoryAllocator());
            ShareTextStyle(*m_menu, false);
            m_menu->SetAutoWidth(true);
            TabBar* self = this;
            for (i32 i = 0; i < GetTabCount(); ++i)
                (void)m_menu->AddCheckItem(GetTab(i)->GetText(), GetTab(i) == m_selected, [self, i](bool) { self->SelectByUser(i); });
            m_menu->OpenBelow(*this, *m_list);
        }

    protected:
        // The tabs and the menu use the bar's font.
        void OnTextStyleChange() override
        {
            for (i32 i = 0; i < GetTabCount(); ++i)
                ShareTextStyle(*GetTab(i), false);
        }
        // Shows the list button while the tabs overflow the strip, or always when the policy says so.
        void OnLayout() override
        {
            LayoutChildren();
            const bool shown = m_listDisplay == TabListDisplay::Always || m_strip->GetScrollRange().x > 0.5f;
            if (shown == m_list->IsDisplayed())
                return;
            m_list->SetDisplayed(shown);
            LayoutChildren();
            if (m_selected != nullptr)
                m_strip->ScrollIntoView(*m_selected);
        }
        void OnKeyDown(const KeyEvent& event) override
        {
            const i32 step = CycleStep(event);
            if (step == 0)
                return;
            CycleSelection(step);
            event.Handled = true;
        }
        // Accepts a torn-out tab of the group and marks where it would land.
        void OnDragQuery(DropQuery& query) override;
        void OnDragLeave(const DragEvent&) override { SetDropIndex(-1); }
        void OnDrop(const DragEvent& event) override;
        void OnDrawAbove(DrawContext& ctx) override;
        void CollectStyleParts(core::Array<core::StringView>& out) const override { out.PushBack(core::StringView(u8"drop-indicator")); }

    private:
        friend class Tab;
        // Shows the close-all part while it is enabled and some tab is closable.
        void UpdateCloseAll()
        {
            bool any = false;
            for (i32 i = 0; i < GetTabCount() && !any; ++i)
                any = GetTab(i)->IsClosable();
            m_closeAllPart->SetDisplayed(m_closeAll && any);
        }
        void SetDropIndex(i32 index)
        {
            if (index == m_dropIndex)
                return;
            m_dropIndex = index;
            Invalidate();
        }
        // Returns where a tab dropped at `local` (in the bar's space) would go: before the first tab
        // whose middle lies past `local` toward the inline end.
        [[nodiscard]] i32 InsertionIndex(core::Float2 local) const
        {
            const core::Float2 world = ConvertToWorldSpace(local);
            const f32 way = IsRightToLeft() ? -1.0f : 1.0f;
            for (i32 i = 0; i < GetTabCount(); ++i)
            {
                if (!GetTab(i)->IsDisplayed())
                    continue; // a hidden (torn-out) tab still counts, but offers no place of its own
                const Rect r = GetTab(i)->GetScreenBounds();
                if (way * world.x < way * (r.x + r.width * 0.5f))
                    return i;
            }
            return GetTabCount();
        }
        [[nodiscard]] static Tab* DraggedTab(const DragPayload& payload);
        friend class TabStrip;
        friend class TabView;

        // Returns 1 for Ctrl+Tab and Ctrl+Page Down, -1 for Ctrl+Shift+Tab and Ctrl+Page Up, and 0
        // for anything else.
        [[nodiscard]] static i32 CycleStep(const KeyEvent& event)
        {
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (key == KeyCode::Tab && event.Modifiers == KeyModCtrl)
                return 1;
            if (key == KeyCode::Tab && event.Modifiers == (KeyModCtrl | KeyModShift))
                return -1;
            if (event.Modifiers == KeyModCtrl && (key == KeyCode::PageDown || key == KeyCode::PageUp))
                return key == KeyCode::PageDown ? 1 : -1;
            return 0;
        }

        void Select(Tab* tab, bool byUser)
        {
            if (tab == m_selected)
                return;
            if (m_selected != nullptr)
                m_selected->SetSelectedState(false);
            m_selected = tab;
            if (tab != nullptr)
            {
                m_selectedAt = IndexOf(tab);
                tab->SetSelectedState(true);
                m_strip->ScrollIntoView(*tab);
            }
            m_selectedChanged.Notify();
            const i32 index = IndexOf(tab);
            if (m_onSelectionChanged)
                m_onSelectionChanged(index);
            if (!byUser)
                return;
            if (m_onSelected)
                m_onSelected(index);
            SendEvent(Event(EventType::Changed, this));
        }

        // Called when the strip's tabs changed. New tabs take the bar's font, a removed selected tab
        // passes the selection on, and each tab that left is reported. A removed tab is still alive
        // here, because the strip holds it across this call.
        void TabsChanged()
        {
            core::Array<Tab*> gone;
            for (Tab* known : m_known)
                if (IndexOf(known) < 0)
                    gone.PushBack(known);
            m_known.Clear();
            for (i32 i = 0; i < GetTabCount(); ++i)
            {
                Tab* tab = GetTab(i);
                m_known.PushBack(tab);
                ShareTextStyle(*tab, false);
                tab->UpdateClose();
            }
            UpdateCloseAll();
            if (m_selected != nullptr && IndexOf(m_selected) < 0)
            {
                m_selected->SetSelectedState(false);
                m_selected = nullptr;
                const i32 next = core::Min(m_selectedAt, GetTabCount() - 1);
                if (next >= 0)
                    Select(GetTab(next), false);
                else
                {
                    m_selectedChanged.Notify();
                    if (m_onSelectionChanged)
                        m_onSelectionChanged(-1);
                }
            }
            else if (m_pendingSelect >= 0 && m_pendingSelect < GetTabCount())
            {
                const i32 pending = m_pendingSelect;
                m_pendingSelect = -1;
                Select(GetTab(pending), false);
            }
            else if (m_selected == nullptr && GetTabCount() > 0 && m_pendingSelect < 0)
                Select(GetTab(0), false); // nothing asked for yet; a pending selection waits, so tab 0's page is neither shown nor built on the way
            else if (m_selected != nullptr)
                m_selectedAt = IndexOf(m_selected); // a tab added before the selected one moved it
            if (m_onRemoved)
                for (Tab* tab : gone)
                    m_onRemoved(*tab);
        }

        // Handles a drag of `tab` with the pointer at `world`: past a neighbour's midpoint, the two
        // swap places.
        void DragTab(Tab& tab, core::Float2 world)
        {
            const f32 x = tab.ConvertToNodeSpace(world).x; // in the tab's own space, which moves with the tab
            const i32 at = IndexOf(&tab);
            const Tab* next = GetTab(at + 1);
            const Tab* previous = GetTab(at - 1);
            const auto midpoint = [&](const Tab& other) { return other.GetPosition().x - tab.GetPosition().x + other.GetSize().x * 0.5f; };
            // Until the strip lays out again, the two tabs keep their old positions, and the next
            // test is against the new neighbour's midpoint, so a tab never swaps back on the same
            // move. The next tab is toward the inline end, which is the left in right to left.
            const f32 way = IsRightToLeft() ? -1.0f : 1.0f;
            if (next != nullptr && way * x > way * midpoint(*next))
                MoveTab(at, at + 1);
            else if (previous != nullptr && way * x < way * midpoint(*previous))
                MoveTab(at, at - 1);
        }

        core::RefPtr<TabStrip> m_strip;
        core::RefPtr<TabListButton> m_list;
        core::RefPtr<TabCloseAllButton> m_closeAllPart;
        core::RefPtr<Menu> m_menu;
        Tab* m_selected = nullptr; // a child of the strip, which owns it
        core::Array<Tab*> m_known; // the tabs as they were, to tell which ones left
        mutable Observable m_selectedChanged;
        TabCloseDisplay m_closeDisplay = TabCloseDisplay::Never;
        TabListDisplay m_listDisplay = TabListDisplay::Overflow;
        bool m_closeAll = false;
        bool m_reorderable = false;
        i32 m_pendingSelect = -1; // a selection asked for before its tab existed
        i32 m_selectedAt = 0; // the selected tab's index; if it is removed, the tab now at this index is selected
        core::Function<void(i32)> m_onSelected;
        core::Function<void(i32)> m_onSelectionChanged;
        core::Function<void(i32, i32)> m_onMoved;
        core::Function<void(Tab&)> m_onRemoved;
        core::Function<void(i32)> m_closeHandler;
        core::String m_group;
        i32 m_dropIndex = -1;
        core::Function<void(Node&, i32)> m_pageAdopter;
        core::Function<void(DropQuery&, Tab&, i32)> m_onTabOffered;
        core::Function<void(Tab&, i32)> m_tabTaker;
        core::Function<void(Tab&, bool)> m_onTabTorn;
        core::Function<void(Tab&, const DragResult&)> m_onTabDragEnded;
    };

    // === Tab ===

    inline Tab::Tab()
    {
        SetTag(core::StringView(u8"tab"));
        m_close = core::MakeRef<TabCloseButton>(MemoryAllocator());
        m_close->SetDisplayed(false);
        Tab* self = this;
        m_close->SetOnClick([self]()
        {
            if (TabBar* bar = self->GetBar())
                bar->RequestClose(bar->IndexOf(self));
        });
        AddChild(m_close.Get()); // after the icon and the text
        // When the tab is torn out (by the pointer or `drag.pick`), its bar is told, and told again
        // when the drag ends (OnDragEnd).
        (void)AddEventListener(EventType::DragStart, [self](const Event& event)
        {
            TabBar* bar = self->GetBar();
            if (bar == nullptr || TabBar::DraggedTab(static_cast<const DragEvent&>(event).Payload) != self)
                return;
            self->m_tornFrom = core::RefPtr<TabBar>(bar);
            if (bar->m_onTabTorn)
                bar->m_onTabTorn(*self, true);
        });
    }

    inline void Tab::OnDragEnd(const DragResult& result)
    {
        const core::RefPtr<TabBar> from = core::Move(m_tornFrom);
        if (from && from->m_onTabTorn)
            from->m_onTabTorn(*this, false);
        if (from && from->m_onTabDragEnded)
            from->m_onTabDragEnded(*this, result);
    }

    inline core::RefPtr<Node> Tab::CreateDragPreview(const DragPayload& payload)
    {
        core::RefPtr<Node> ghost = Node::CreateDragPreview(payload);
        if (UIWidget* widget = core::Cast<UIWidget>(ghost.Get()))
        {
            widget->AddClass(core::StringView(u8"tab"));
            const core::Float2 size = GetSize();
            StyleWriteScope defaults(*widget);
            widget->EditLayoutStyle([size](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(size.x);
                s.Height = LayoutLength::Px(size.y);
            });
        }
        return ghost;
    }

    inline TabBar* Tab::GetBar() const
    {
        Node* strip = GetParent();
        return strip != nullptr ? core::Cast<TabBar>(strip->GetParent()) : nullptr;
    }

    inline void Tab::UpdateClose()
    {
        const TabBar* bar = GetBar();
        const TabCloseDisplay display = bar != nullptr ? bar->GetCloseDisplay() : TabCloseDisplay::Never;
        // Whether or not the policy shows the close part, it keeps its room while the tab is
        // closable, so a hovered tab does not grow.
        m_close->SetDisplayed(m_closable && display != TabCloseDisplay::Never);
        const bool shown = display == TabCloseDisplay::Always || (display == TabCloseDisplay::Selected && m_selected) ||
                           (display == TabCloseDisplay::Hover && (m_selected || IsHovered()));
        m_close->SetVisible(shown);
        if (TabBar* owner = GetBar())
            owner->UpdateCloseAll(); // this tab's closability may be what changed
    }

    inline bool Tab::OwnsPressDrag(core::Float2) const
    {
        const TabBar* bar = GetBar();
        return bar != nullptr && bar->IsReorderable() && bar->GetGroup().IsEmpty();
    }

    inline void Tab::FillTabPayload(DragPayload& payload)
    {
        payload.Type = core::String(u8"gui/tab");
        payload.Value = HandleAs(Node::StaticType());
        payload.Label = core::String(GetText());
        payload.Allowed = DropEffectsMove;
    }

    inline bool Tab::OnDragRequested(DragRequest& request)
    {
        const TabBar* bar = GetBar();
        if (bar == nullptr || bar->GetGroup().IsEmpty())
            return false;
        FillTabPayload(request.Payload);
        if (!request.FromKeyboard)
        {
            request.GrabOffset = request.Press; // hold the ghost where the press grabbed it
            request.HasGrabOffset = true;
        }
        return true;
    }

    inline bool Tab::OnDragHoverOpen(core::Float2)
    {
        TabBar* bar = GetBar();
        if (bar == nullptr || m_selected)
            return false;
        bar->SelectByUser(bar->IndexOf(this));
        return true;
    }

    inline bool Tab::OnActivate()
    {
        if (TabBar* bar = GetBar())
            bar->SelectByUser(bar->IndexOf(this));
        return true;
    }

    inline void Tab::OnMouseDown(const MouseEvent& event)
    {
        TabBar* bar = GetBar();
        if (bar == nullptr)
            return;
        if (event.Button == MouseButton::Middle)
        {
            // A middle click closes the tab, as in a browser, unless the policy never shows a close
            // part.
            if (m_closable && bar->GetCloseDisplay() != TabCloseDisplay::Never)
                bar->RequestClose(bar->IndexOf(this));
            event.Handled = true;
            return;
        }
        if (event.Button != MouseButton::Left)
            return;
        event.Handled = true;
        RequestFocus();
        bar->SelectByUser(bar->IndexOf(this));
        m_pressing = true;
        m_dragging = false;
        m_pressX = event.Position.x;
    }

    inline void Tab::OnMouseMove(const MouseEvent& event)
    {
        TabBar* bar = GetBar();
        // In a group's strip the dispatcher's drag (OnDragRequested) handles the move instead of this.
        if (!m_pressing || bar == nullptr || !bar->IsReorderable() || !bar->GetGroup().IsEmpty())
            return;
        constexpr f32 kDragThreshold = 4.0f;
        if (!m_dragging && core::Abs(event.Position.x - m_pressX) < kDragThreshold)
            return;
        if (!m_dragging)
        {
            m_dragging = true;
            AddClass(core::StringView(u8"dragging"));
        }
        bar->DragTab(*this, event.Position);
    }

    inline void Tab::OnKeyDown(const KeyEvent& event)
    {
        TabBar* bar = GetBar();
        if (bar == nullptr || event.Modifiers != 0)
            return;
        const i32 at = bar->IndexOf(this);
        i32 to = -1;
        switch (static_cast<KeyCode>(event.KeyCode))
        {
        // Move along the strip in its direction; in right to left, Left goes to the next tab.
        case KeyCode::Left:
        case KeyCode::Right: to = at + InlineStep(static_cast<KeyCode>(event.KeyCode)); break;
        case KeyCode::Home: to = 0; break;
        case KeyCode::End: to = bar->GetTabCount() - 1; break;
        default: return;
        }
        // The arrows stop at the ends, where the key is left unhandled so navigation leaves the
        // strip. Home and End always land on a tab.
        Tab* target = bar->GetTab(to);
        if (target == nullptr)
            return;
        bar->SelectByUser(to);
        target->RequestFocus();
        event.Handled = true;
    }

    // === Between strips ===

    inline Tab* TabBar::DraggedTab(const DragPayload& payload)
    {
        return payload.Type.AsView() == core::StringView(u8"gui/tab") ? core::Cast<Tab>(Node::FromHandle(payload.Value)) : nullptr;
    }

    inline void TabBar::OnDragQuery(DropQuery& query) { (void)OfferTab(query, InsertionIndex(query.Local)); }

    inline void TabBar::OnDrop(const DragEvent& event) { DropTab(event.Payload); }

    inline void TabBar::DropTab(const DragPayload& payload)
    {
        Tab* tab = DraggedTab(payload);
        TabBar* from = tab != nullptr ? tab->GetBar() : nullptr;
        const i32 index = m_dropIndex;
        SetDropIndex(-1);
        if (from == nullptr || index < 0)
            return;
        const core::RefPtr<Tab> keep(tab);
        if (m_tabTaker)
        {
            m_tabTaker(*tab, index);
            return;
        }
        if (from == this)
        {
            // Within the strip, move before the tab at `index`, not counting the tab's own place.
            const i32 at = IndexOf(tab);
            MoveTab(at, index > at ? index - 1 : index);
            SelectByUser(IndexOf(tab));
            return;
        }
        if (Node* page = tab->GetPage(); page != nullptr && m_pageAdopter)
        {
            // Between tab views the page moves, and the receiving view makes its tab here.
            const core::RefPtr<Node> keepPage(page);
            page->RemoveFromParent(); // the old view removes the tab along with the page
            m_pageAdopter(*page, index);
            SelectByUser(core::Min(index, GetTabCount() - 1));
            return;
        }
        // Between bars the tab itself moves.
        m_strip->AddChildAt(tab, static_cast<usize>(core::Min(index, GetTabCount())));
        SelectByUser(IndexOf(tab));
    }

    // Draws the mark between the tabs where a dropped tab would land.
    inline void TabBar::OnDrawAbove(DrawContext& ctx)
    {
        if (m_dropIndex < 0)
            return;
        const PartStyle& part = GetPartStyle(u8"drop-indicator");
        const Color color = part.HasBackground ? part.Box.BackgroundColor : Color{0.31f, 0.61f, 1.0f, 1.0f};
        // Draw it at the inline start of the tab it would go before, or else after the last tab's end.
        const bool rtl = IsRightToLeft();
        f32 x = 0.0f;
        if (const Tab* at = GetTab(m_dropIndex))
        {
            const Rect r = at->GetScreenBounds();
            x = ConvertToNodeSpace(core::Float2{rtl ? r.x + r.width : r.x, 0.0f}).x;
        }
        else if (const Tab* last = GetTab(GetTabCount() - 1))
        {
            const Rect r = last->GetScreenBounds();
            x = ConvertToNodeSpace(core::Float2{rtl ? r.x : r.x + r.width, 0.0f}).x;
        }
        ctx.VG().FillRect(core::Rectangle{x - 1.0f, 0.0f, 2.0f, GetSize().y}, color);
    }

    // === TabStrip ===

    inline TabStrip::TabStrip(TabBar& bar) : m_bar(bar)
    {
        SetTag(core::StringView(u8"tabstrip"));
        SetFocusGroup(true); // one Tab stop (the selected tab); the arrow keys move between tabs
        StyleWriteScope defaults(*this);
        EditLayoutStyle([](LayoutStyle& s)
        {
            s.Direction = FlexDirection::Row;
            s.AlignItems = Align::Stretch;
            s.ColumnGap = 2.0f;
            s.Grow = 1.0f;
            s.MinWidth = LayoutLength::Px(0.0f); // the strip may be narrower than its tabs, and then it scrolls
            s.OverflowX = Overflow::Scroll;
            s.BarWidth = ScrollbarWidth::None;
        });
    }

    inline void TabStrip::OnChildrenChanged() { m_bar.TabsChanged(); }
}
