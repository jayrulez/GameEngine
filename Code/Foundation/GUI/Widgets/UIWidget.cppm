// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :ui_widget partition
//
// UIWidget: a UINode with the CSS identity and layout inputs the styling engine matches on.
// Ported from eepp's UI::UIWidget. It adds the element tag, id and style classes (what selectors
// match), the tooltip, the context menu, drag and drop from markup, and accessibility overrides.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:ui_widget;

import foundation.core; // String, StringView, Array
import :thickness;
import :ui_node;
import :event; // MouseEvent, for the context menu

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// The widget states that pseudo-classes read: :checked, :indeterminate, :read-only and
    /// :read-write, :placeholder-shown, :selected.
    enum WidgetState : u32
    {
        WidgetStateNone = 0,
        WidgetStateChecked = 1u << 0,
        WidgetStateIndeterminate = 1u << 1,
        WidgetStateEditable = 1u << 2, ///< the user can edit the value (:read-write; otherwise :read-only)
        WidgetStatePlaceholderShown = 1u << 3,
        WidgetStateSelected = 1u << 4, ///< an item view row that is in the selection (:selected)
    };

    class UIWidget : public UINode
    {
        RTTI_OBJECT(UIWidget, UINode)
    public:
        UIWidget() = default;

        // === CSS identity (selector surface) ===
        void SetTag(core::StringView tag)
        {
            m_tag = tag;
            MarkStyleDirty(true); // the selectors it matches may change
            Invalidate();
        }
        [[nodiscard]] core::StringView GetTag() const { return m_tag.AsView(); }

        void SetId(core::StringView id)
        {
            m_id = id;
            MarkStyleDirty(true);
            Invalidate();
        }
        [[nodiscard]] core::StringView GetId() const { return m_id.AsView(); }
        /// Returns the type and, when there is one, the id ("Button #ok").
        [[nodiscard]] core::String DebugName() const override
        {
            core::String name = UINode::DebugName();
            if (m_id.Size() != 0)
            {
                name += core::StringView(u8" #");
                name += m_id.AsView();
            }
            return name;
        }

        void AddClass(core::StringView cls)
        {
            if (cls.Size() != 0 && !HasClass(cls))
            {
                m_classes.PushBack(core::String(cls));
                MarkStyleDirty(true);
                Invalidate();
            }
        }
        void RemoveClass(core::StringView cls)
        {
            for (usize i = 0; i < m_classes.Size(); ++i)
                if (m_classes[i] == cls)
                {
                    m_classes.RemoveAt(i);
                    MarkStyleDirty(true);
                    Invalidate();
                    return;
                }
        }
        [[nodiscard]] bool HasClass(core::StringView cls) const
        {
            for (const core::String& c : m_classes)
                if (c == cls)
                    return true;
            return false;
        }
        /// Adds each class in a space-separated list (markup's `class`); RemoveClasses removes them.
        void AddClasses(core::StringView classes)
        {
            ForEachClass(classes, [this](core::StringView cls) { AddClass(cls); });
        }
        void RemoveClasses(core::StringView classes)
        {
            ForEachClass(classes, [this](core::StringView cls) { RemoveClass(cls); });
        }
        /// Calls `each` with every class of a space-separated list.
        template <typename F>
        static void ForEachClass(core::StringView classes, F&& each)
        {
            usize start = 0;
            for (usize i = 0; i <= classes.Size(); ++i)
                if (i == classes.Size() || core::IsWhiteSpace(classes[i]))
                {
                    if (i > start)
                        each(classes.SubStr(start, i - start));
                    start = i + 1;
                }
        }
        void ToggleClass(core::StringView cls)
        {
            if (HasClass(cls))
                RemoveClass(cls);
            else
                AddClass(cls);
        }
        [[nodiscard]] usize ClassCount() const noexcept { return m_classes.Size(); }

        /// Returns the WidgetState flags. A widget with such a state reports it here and marks
        /// itself for a restyle when it changes.
        [[nodiscard]] virtual u32 GetWidgetStates() const { return WidgetStateNone; }
        [[nodiscard]] const Array<core::String>& Classes() const noexcept { return m_classes; }

        /// Returns the widget's 1-based position among its parent's widgets, and their count, for
        /// :nth-child and related selectors. The first call after the children change numbers all
        /// the siblings at once.
        void PositionAmongWidgets(i32& position, i32& count) const
        {
            const Node* parent = GetParent();
            if (parent == nullptr)
            {
                position = count = 1;
                return;
            }
            if (m_positionParent != parent || m_positionGeneration != parent->ChildrenGeneration())
            {
                i32 total = 0;
                for (usize i = 0; i < parent->ChildCount(); ++i)
                    if (core::Cast<UIWidget>(parent->GetChildAt(i)) != nullptr)
                        ++total;
                i32 at = 0;
                for (usize i = 0; i < parent->ChildCount(); ++i)
                    if (const UIWidget* w = core::Cast<UIWidget>(parent->GetChildAt(i)))
                    {
                        w->m_position = ++at;
                        w->m_positionCount = total;
                        w->m_positionParent = parent;
                        w->m_positionGeneration = parent->ChildrenGeneration();
                    }
            }
            position = m_position;
            count = m_positionCount;
        }

        // === Hover tooltip ===
        void SetTooltip(core::StringView text) { m_tooltip = text; }
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            return m_tooltip.AsView();
        }
        void SetTooltipInteractive(bool interactive) noexcept { m_tooltipInteractive = interactive; }
        [[nodiscard]] bool IsTooltipInteractive() const override { return m_tooltipInteractive; }

        // === Accessibility in markup ===
        /// Sets the role a screen reader reports, overriding the type's (markup's `role`). Empty
        /// means the type's role.
        void SetAccessRoleName(const core::String& role)
        {
            m_accessRole = role;
            MarkAccessDirty();
        }
        [[nodiscard]] core::String GetAccessRoleName() const { return m_accessRole; }
        /// Sets the accessible name, overriding what the widget shows (`aria-label`).
        void SetAccessLabel(const core::String& label)
        {
            m_accessLabel = label;
            MarkAccessDirty();
        }
        [[nodiscard]] core::String GetAccessLabel() const { return m_accessLabel; }
        /// Sets the ids of the widgets that name it (`aria-labelledby`). SetAccessDescribedBy takes
        /// those that describe it (`aria-describedby`) and SetAccessControls those it controls
        /// (`aria-controls`). Ids are separated by spaces.
        void SetAccessLabelledBy(const core::String& ids)
        {
            m_accessLabelledBy = ids;
            MarkAccessDirty();
        }
        [[nodiscard]] core::String GetAccessLabelledBy() const { return m_accessLabelledBy; }
        void SetAccessDescribedBy(const core::String& ids)
        {
            m_accessDescribedBy = ids;
            MarkAccessDirty();
        }
        [[nodiscard]] core::String GetAccessDescribedBy() const { return m_accessDescribedBy; }
        void SetAccessControls(const core::String& ids)
        {
            m_accessControls = ids;
            MarkAccessDirty();
        }
        [[nodiscard]] core::String GetAccessControls() const { return m_accessControls; }
        /// Hides the widget and its contents from screen readers (`aria-hidden`), as for a decoration.
        void SetAccessHidden(bool hidden)
        {
            m_accessHidden = hidden;
            MarkAccessDirty();
        }
        [[nodiscard]] bool IsAccessHidden() const noexcept { return m_accessHidden; }

        // === Drag and drop in markup ===
        /// Sets the payload type the widget drags as (markup's `drag-type`); empty means it does
        /// not drag.
        void SetDragType(const core::String& type) { m_dragType = type; }
        [[nodiscard]] core::String GetDragType() const { return m_dragType; }
        /// Sets the payload's value (`drag-value`, which may be a `{{name}}`). SetDragLabel sets its
        /// label (`drag-label`), which defaults to the value.
        void SetDragValue(const core::String& value) { m_dragValue = value; }
        [[nodiscard]] core::String GetDragValue() const { return m_dragValue; }
        void SetDragLabel(const core::String& label) { m_dragLabel = label; }
        [[nodiscard]] core::String GetDragLabel() const { return m_dragLabel; }
        /// Sets `drag` (a property from RmlUi). "auto" drags when the widget has a drag type and
        /// otherwise passes the press up. "none" never drags and passes the press up. "block" stops
        /// the search here, so no ancestor drags from a press on this widget.
        void SetDragModeName(const core::String& mode)
        {
            const core::StringView m = core::Trim(mode.AsView());
            m_dragMode = m == core::StringView(u8"none") ? 1 : m == core::StringView(u8"block") ? 2 : 0;
        }
        [[nodiscard]] core::String GetDragModeName() const
        {
            return core::String(m_dragMode == 1 ? core::StringView(u8"none") : m_dragMode == 2 ? core::StringView(u8"block") : core::StringView(u8"auto"));
        }
        /// Sets the text describing what a drop here would do (`drop-reason`), shown by the pointer
        /// while the widget would accept the drop.
        void SetDropReason(const core::String& reason) { m_dropReason = reason; }
        [[nodiscard]] core::String GetDropReason() const { return m_dropReason; }
        [[nodiscard]] bool OwnsPressDrag(core::Float2 local) const override { return m_dragMode == 2 || UINode::OwnsPressDrag(local); }

        // === Context menu ===
        /// Sets the id of a Menu in the GUI (markup's `<Menu id="...">`) that opens at the pointer
        /// when a context menu is requested over this widget (markup's `context-menu`).
        void SetContextMenuId(const core::String& id)
        {
            // The style pass writes this again at every restyle. The same id keeps the menu already
            // found, because a closed menu is out of the tree and could not be found again.
            if (id == m_contextMenuId)
                return;
            m_contextMenuId = id;
            m_contextMenu = core::RefPtr<Node>{};
        }
        [[nodiscard]] core::String GetContextMenuId() const { return m_contextMenuId; }

    protected:
        bool OnDragRequested(DragRequest& request) override
        {
            if (m_dragMode != 0 || m_dragType.IsEmpty())
                return false;
            request.Payload.Type = m_dragType;
            request.Payload.Value = core::Variant::From<core::String>(m_dragValue);
            request.Payload.Label = m_dragLabel.IsEmpty() ? m_dragValue : m_dragLabel;
            return true;
        }
        void OnDragQuery(DropQuery& query) override
        {
            UINode::OnDragQuery(query);
            if (query.Effect == DropEffect::None && !query.Refused && TakesDropType(query.Payload.Type.AsView()) && !m_dropReason.IsEmpty())
            {
                query.Accept();
                query.Reason = m_dropReason;
            }
        }
        void OnContextMenu(const MouseEvent& event) override
        {
            if (!m_contextMenuId.IsEmpty() && OpenContextMenu(event))
                event.Handled = true;
        }

    private:
        // Finds the menu and opens it at the event's position. The menu is kept once found, since
        // when open it moves from the page to the top layer, and when closed it leaves the tree.
        // Defined in GUIClusterImpl.cpp, which can see :menu.
        bool OpenContextMenu(const MouseEvent& event);

        core::String m_tag;
        core::String m_id;
        Array<core::String> m_classes;
        mutable const Node* m_positionParent = nullptr; // the parent PositionAmongWidgets numbered under
        mutable u32 m_positionGeneration = 0;           // which arrangement of that parent's children it numbered
        mutable i32 m_position = 1;
        mutable i32 m_positionCount = 1;
        core::String m_tooltip;
        bool m_tooltipInteractive = false; // the tooltip takes the pointer
        core::String m_accessRole;
        core::String m_accessLabel;
        core::String m_accessLabelledBy;
        core::String m_accessDescribedBy;
        core::String m_accessControls;
        bool m_accessHidden = false;
        core::String m_contextMenuId;
        core::String m_dragType;
        core::String m_dragValue;
        core::String m_dragLabel;
        core::String m_dropReason;
        u8 m_dragMode = 0; // auto, none, block
        core::RefPtr<Node> m_contextMenu; // the context menu, once found
    };

    /// Receives markup's children for a widget that adopts them in its own way (a menu's items, a
    /// tree's items). The host is hidden and out of the tree. Each child is handed to `adopt` as it
    /// arrives, and `adopt` takes it out.
    class MarkupChildHost : public UIWidget
    {
        RTTI_OBJECT(MarkupChildHost, UIWidget)
    public:
        explicit MarkupChildHost(core::Function<void(Node&)> adopt) : m_adopt(core::Move(adopt)) { SetDisplayed(false); }
        void OnChildrenChanged() override
        {
            if (m_adopt && ChildCount() != 0)
                m_adopt(*this);
        }

    private:
        core::Function<void(Node&)> m_adopt;
    };
}
