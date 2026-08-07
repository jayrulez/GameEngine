// Draconic GUI - :ui_widget partition
//
// UIWidget: a UINode with the CSS identity + layout inputs the styling engine matches on.
// Ported from eepp's UI::UIWidget - element tag, id, and style classes (the selector
// surface for Phase 6's CSS engine), plus a layout margin. Size policies + attribute/markup
// loading arrive with the layout and CSS phases.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:ui_widget;

import draconic.foundation; // String, StringView, Array
import :thickness;
import :ui_node;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class UIWidget : public UINode
    {
        DRACONIC_OBJECT(UIWidget, UINode)
    public:
        UIWidget() = default;

        // === CSS identity (selector surface) ===
        void SetTag(foundation::StringView tag)
        {
            m_tag = tag;
            Invalidate();
        }
        [[nodiscard]] foundation::StringView GetTag() const { return m_tag.AsView(); }

        void SetId(foundation::StringView id)
        {
            m_id = id;
            Invalidate();
        }
        [[nodiscard]] foundation::StringView GetId() const { return m_id.AsView(); }

        void AddClass(foundation::StringView cls)
        {
            if (cls.Size() != 0 && !HasClass(cls))
            {
                m_classes.PushBack(foundation::String(cls));
                Invalidate();
            }
        }
        void RemoveClass(foundation::StringView cls)
        {
            for (usize i = 0; i < m_classes.Size(); ++i)
                if (m_classes[i] == cls)
                {
                    m_classes.RemoveAt(i);
                    Invalidate();
                    return;
                }
        }
        [[nodiscard]] bool HasClass(foundation::StringView cls) const
        {
            for (const foundation::String& c : m_classes)
                if (c == cls)
                    return true;
            return false;
        }
        void ToggleClass(foundation::StringView cls)
        {
            if (HasClass(cls))
                RemoveClass(cls);
            else
                AddClass(cls);
        }
        [[nodiscard]] usize ClassCount() const noexcept { return m_classes.Size(); }
        [[nodiscard]] const Array<foundation::String>& Classes() const noexcept { return m_classes; }

        // === Layout margin ===
        void SetMargin(Thickness margin)
        {
            m_margin = margin;
            Invalidate();
        }
        [[nodiscard]] Thickness GetMargin() const noexcept { return m_margin; }

        // === Hover tooltip ===
        void SetTooltip(foundation::StringView text) { m_tooltip = text; }
        [[nodiscard]] foundation::StringView GetTooltipText() const override
        {
            return m_tooltip.AsView();
        }

    private:
        foundation::String m_tag;
        foundation::String m_id;
        Array<foundation::String> m_classes;
        Thickness m_margin{};
        foundation::String m_tooltip;
    };

    DRACONIC_DEFINE_OBJECT(UIWidget, "draconic::gui")
}
