// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :ui_node partition
//
// UINode: a Node with UI chrome. Ported from eepp's UI::UINode - adds padding (a content
// inset), a state-aware skin, and a ControlState that tracks pointer/focus/enabled so
// StateListDrawable skins/backgrounds react to input automatically. (Base Node already
// carries the background/foreground drawables + clip from the render-seam phase.)

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:ui_node;

import foundation.core;  // RefPtr, Max, Move, Color
import foundation.fonts; // CachedFont
import :rect;
import :thickness;
import :control_state;
import :event;
import :drawable;
import :text; // TextHAlign / TextVAlign (theme text-align hooks)
import :node;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    /// One declaration of a node's inline style: markup's attributes the node does not claim, a
    /// script's style. The cascade's highest origin (E3).
    struct InlineStyleDeclaration
    {
        core::String Name;
        core::String Value;
    };

    class UINode : public Node
    {
        RTTI_OBJECT(UINode, Node)
    public:
        UINode() = default;

        // Content padding (inset for content/children).
        // Skin = a state-aware background (typically a StateListDrawable). Node::Draw draws
        // the background with GetControlState(), so the skin reacts to hover/press/focus.
        void SetSkin(RefPtr<Drawable> skin) { SetBackground(core::Move(skin)); }

        // Theming hooks: text-bearing widgets override these so the CSS `color` and
        // `font-family` properties reach their text (Label/TextField/ComboBox/...). Default
        // no-ops, so a plain node ignores them.
        virtual void SetThemeTextColor(Color) {}
        virtual void SetThemeFont(fonts::CachedFont*) {}

        // CSS text-align / vertical-align reach a text widget's alignment (default no-ops).
        virtual void SetThemeTextAlign(TextHAlign) {}
        virtual void SetThemeTextAlignV(TextVAlign) {}

        // === Inline style (E3) ===
        /// Sets `name: value` in this node's inline style (replacing one of that name): it wins
        /// over every sheet's normal declarations, at the next restyle.
        void SetInlineStyle(core::StringView name, core::StringView value)
        {
            for (InlineStyleDeclaration& d : m_inlineStyle)
                if (d.Name.AsView() == name)
                {
                    if (d.Value.AsView() == value)
                        return;
                    d.Value = core::String(value);
                    MarkStyleDirty(false);
                    return;
                }
            m_inlineStyle.PushBack(InlineStyleDeclaration{core::String(name), core::String(value)});
            MarkStyleDirty(false);
        }
        void RemoveInlineStyle(core::StringView name)
        {
            for (usize i = 0; i < m_inlineStyle.Size(); ++i)
                if (m_inlineStyle[i].Name.AsView() == name)
                {
                    m_inlineStyle.RemoveAt(i);
                    MarkStyleDirty(false);
                    return;
                }
        }
        [[nodiscard]] core::Span<const InlineStyleDeclaration> GetInlineStyle() const noexcept { return m_inlineStyle.AsSpan(); }

        /// A reflected property a sheet set by its CSS name: the node's own value from before, the
        /// value the sheet last wrote (as text; empty when it writes none now), and whether code
        /// has since taken it.
        struct StyledProperty
        {
            core::String Name;
            Variant Own;
            core::String Written;
            bool Code = false;
        };
        [[nodiscard]] core::Array<StyledProperty>& StyledProperties() noexcept { return m_styledProperties; }

        // Markup hook: a widget consumes its own structural XML attributes (e.g. Label "text",
        // LinearLayout "orientation"/"spacing"), returning true if it handled `name`. Attributes
        // it doesn't claim are applied as CSS properties by the markup loader. Default: none.
        virtual bool SetMarkupAttribute(core::StringView /*name*/, core::StringView /*value*/)
        {
            return false;
        }

        // Pseudo-element parts: a widget names the parts it paints (slider "track"/"fill"/
        // "thumb", checkbox "box"/"mark", window "title"/"grip", ...) so CSS `tag::part` can
        // style them. CollectStyleParts lists them; SetThemePartColor receives a part's
        // background-color. Defaults: no parts / no-op.
        virtual void CollectStyleParts(core::Array<core::StringView>&) const {}
        virtual void SetThemePartColor(core::StringView, Color) {}

        // Visual state, driven by pointer/focus/enabled (priority: disabled > pressed >
        // hover > focused > normal).
        [[nodiscard]] ControlState GetControlState() const override
        {
            if (!IsEnabled())
                return ControlState::Disabled;
            if (IsPressed())
                return ControlState::Pressed;
            if (IsHovered())
                return ControlState::Hover;
            if (IsFocused())
                return ControlState::Focused;
            return ControlState::Normal;
        }

    private:
        core::Array<InlineStyleDeclaration> m_inlineStyle;
        core::Array<StyledProperty> m_styledProperties;
    };
}
