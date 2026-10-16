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

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
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
        [[nodiscard]] const Array<core::String>& Classes() const noexcept { return m_classes; }

        // === Hover tooltip ===
        void SetTooltip(core::StringView text) { m_tooltip = text; }
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            return m_tooltip.AsView();
        }

    private:
        core::String m_tag;
        core::String m_id;
        Array<core::String> m_classes;
        core::String m_tooltip;
    };
}
