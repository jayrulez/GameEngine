// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :ui_widget partition
//
// UIWidget: a UINode with the CSS identity + layout inputs the styling engine matches on.
// Ported from eepp's UI::UIWidget - element tag, id, and style classes (the selector
// surface for Phase 6's CSS engine). Size policies + attribute/markup
// loading arrive with the layout and CSS phases.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:ui_widget;

import foundation.core; // String, StringView, Array
import :thickness;
import :ui_node;
import :event; // MouseEvent (a context menu)

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// The states a widget's own pseudo-classes read (E3): :checked, :indeterminate, :read-only
    /// and :read-write, :placeholder-shown.
    enum WidgetState : u32
    {
        WidgetStateNone = 0,
        WidgetStateChecked = 1u << 0,
        WidgetStateIndeterminate = 1u << 1,
        WidgetStateEditable = 1u << 2, ///< the user edits its value (:read-write; else :read-only)
        WidgetStatePlaceholderShown = 1u << 3,
        WidgetStateSelected = 1u << 4, ///< an item view's row in its selection (:selected)
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
            MarkStyleDirty(true); // the selectors it matches change
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
        /// Its type and, when it has one, its id ("Button #ok").
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
        void ToggleClass(core::StringView cls)
        {
            if (HasClass(cls))
                RemoveClass(cls);
            else
                AddClass(cls);
        }
        [[nodiscard]] usize ClassCount() const noexcept { return m_classes.Size(); }

        /// Its WidgetState flags; a widget with such a state reports it, and marks itself for a
        /// restyle when it changes.
        [[nodiscard]] virtual u32 GetWidgetStates() const { return WidgetStateNone; }
        [[nodiscard]] const Array<core::String>& Classes() const noexcept { return m_classes; }

        /// Its 1-based place among its parent's widgets and how many there are (:nth-child and
        /// its kin). The first ask after the children change numbers every widget there at once.
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

        // === Drag and drop in markup (E7) ===
        /// Drags as this payload type (markup's `drag-type`; empty: it does not drag).
        void SetDragType(const core::String& type) { m_dragType = type; }
        [[nodiscard]] core::String GetDragType() const { return m_dragType; }
        /// The payload's value and label (`drag-value`, which may be a `{{name}}`; `drag-label`,
        /// else the value).
        void SetDragValue(const core::String& value) { m_dragValue = value; }
        [[nodiscard]] core::String GetDragValue() const { return m_dragValue; }
        void SetDragLabel(const core::String& label) { m_dragLabel = label; }
        [[nodiscard]] core::String GetDragLabel() const { return m_dragLabel; }
        /// `drag`: "auto" (it drags when it has a type, else passes the press up), "none" (it never
        /// drags; the press passes up), "block" (the walk ends here: nothing above drags from a
        /// press on it; RmlUi's property).
        void SetDragModeName(const core::String& mode)
        {
            const core::StringView m = core::Trim(mode.AsView());
            m_dragMode = m == core::StringView(u8"none") ? 1 : m == core::StringView(u8"block") ? 2 : 0;
        }
        [[nodiscard]] core::String GetDragModeName() const
        {
            return core::String(m_dragMode == 1 ? core::StringView(u8"none") : m_dragMode == 2 ? core::StringView(u8"block") : core::StringView(u8"auto"));
        }
        /// What a drop here says it would do (`drop-reason`), shown by the pointer while it would
        /// take one.
        void SetDropReason(const core::String& reason) { m_dropReason = reason; }
        [[nodiscard]] core::String GetDropReason() const { return m_dropReason; }
        [[nodiscard]] bool OwnsPressDrag(core::Float2 local) const override { return m_dragMode == 2 || UINode::OwnsPressDrag(local); }

        // === Context menu ===
        /// The id of a Menu in its GUI (markup's `<Menu id="...">`) that a context menu asked for
        /// over it opens, at the pointer (markup's `context-menu`).
        void SetContextMenuId(const core::String& id)
        {
            // The style pass writes it again at every restyle: the same id keeps the menu found
            // (closed, it is out of the tree, where it could not be found again).
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
        // Finds the menu (kept once found: open, it leaves the page for the top layer, and
        // closed, the tree) and opens it at the event's position. GUIClusterImpl.cpp, which sees
        // :menu.
        bool OpenContextMenu(const MouseEvent& event);

        core::String m_tag;
        core::String m_id;
        Array<core::String> m_classes;
        mutable const Node* m_positionParent = nullptr; // PositionAmongWidgets's, numbered under
        mutable u32 m_positionGeneration = 0;           // this arrangement of that parent's children
        mutable i32 m_position = 1;
        mutable i32 m_positionCount = 1;
        core::String m_tooltip;
        core::String m_contextMenuId;
        core::String m_dragType;
        core::String m_dragValue;
        core::String m_dragLabel;
        core::String m_dropReason;
        u8 m_dragMode = 0; // auto, none, block
        core::RefPtr<Node> m_contextMenu; // the menu it opens, once found
    };

    /// Where markup's children land for a widget that takes them in its own way (a menu's items,
    /// a tree's items): unseen and out of the tree, each handed to `adopt` as it arrives, which
    /// takes it out.
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
