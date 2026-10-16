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
    class UINode : public Node
    {
        RTTI_OBJECT(UINode, Node)
    public:
        UINode() = default;

        // Content padding (inset for content/children).
        void SetPadding(Thickness padding)
        {
            m_padding = padding;
            Invalidate();
        }
        [[nodiscard]] Thickness GetPadding() const noexcept { return m_padding; }

        // The local bounds inset by padding - where content/text goes.
        [[nodiscard]] Rect GetContentBounds() const
        {
            const Rect b = GetLocalBounds();
            return Rect{b.x + m_padding.Left, b.y + m_padding.Top,
                        core::Max(0.0f, b.width - m_padding.TotalHorizontal()),
                        core::Max(0.0f, b.height - m_padding.TotalVertical())};
        }

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

    protected:
        Thickness m_padding{};
    };
}
