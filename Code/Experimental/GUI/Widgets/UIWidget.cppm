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

        // === Hover tooltip ===
        void SetTooltip(core::StringView text) { m_tooltip = text; }
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            return m_tooltip.AsView();
        }

        // === Context menu ===
        /// The id of a Menu in its GUI (markup's `<Menu id="...">`) that a context menu asked for
        /// over it opens, at the pointer (markup's `context-menu`).
        void SetContextMenuId(const core::String& id)
        {
            m_contextMenuId = id;
            m_contextMenu = core::RefPtr<Node>{};
        }
        [[nodiscard]] core::String GetContextMenuId() const { return m_contextMenuId; }

    protected:
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
        core::String m_tooltip;
        core::String m_contextMenuId;
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
