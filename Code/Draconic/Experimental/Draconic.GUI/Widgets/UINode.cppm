// Draconic GUI - :ui_node partition
//
// UINode: a Node with UI chrome. Ported from eepp's UI::UINode - adds padding (a content
// inset), a state-aware skin, and a ControlState that tracks pointer/focus/enabled so
// StateListDrawable skins/backgrounds react to input automatically. (Base Node already
// carries the background/foreground drawables + clip from the render-seam phase.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:ui_node;

import draconic.foundation;  // RefPtr, Max, Move, Color
import draconic.fonts; // CachedFont
import :rect;
import :thickness;
import :control_state;
import :event;
import :drawable;
import :text; // TextHAlign / TextVAlign (theme text-align hooks)
import :node;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    class UINode : public Node
    {
        DRACONIC_OBJECT(UINode, Node)
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
                        foundation::Max(0.0f, b.width - m_padding.TotalHorizontal()),
                        foundation::Max(0.0f, b.height - m_padding.TotalVertical())};
        }

        // Skin = a state-aware background (typically a StateListDrawable). Node::Draw draws
        // the background with GetControlState(), so the skin reacts to hover/press/focus.
        void SetSkin(RefPtr<Drawable> skin) { SetBackground(foundation::Move(skin)); }

        [[nodiscard]] bool IsHovered() const noexcept { return m_hovered; }
        [[nodiscard]] bool IsPressed() const noexcept { return m_pressed; }

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
        virtual bool SetMarkupAttribute(foundation::StringView /*name*/, foundation::StringView /*value*/)
        {
            return false;
        }

        // Pseudo-element parts: a widget names the parts it paints (slider "track"/"fill"/
        // "thumb", checkbox "box"/"mark", window "title"/"grip", ...) so CSS `tag::part` can
        // style them. CollectStyleParts lists them; SetThemePartColor receives a part's
        // background-color. Defaults: no parts / no-op.
        virtual void CollectStyleParts(foundation::Array<foundation::StringView>&) const {}
        virtual void SetThemePartColor(foundation::StringView, Color) {}

        // Visual state, driven by pointer/focus/enabled (priority: disabled > pressed >
        // hover > focused > normal).
        [[nodiscard]] ControlState GetControlState() const override
        {
            if (!IsEnabled())
                return ControlState::Disabled;
            if (m_pressed)
                return ControlState::Pressed;
            if (m_hovered)
                return ControlState::Hover;
            if (IsFocused())
                return ControlState::Focused;
            return ControlState::Normal;
        }

    protected:
        void OnMouseEnter(const MouseEvent&) override
        {
            m_hovered = true;
            Invalidate();
        }
        void OnMouseLeave(const MouseEvent&) override
        {
            m_hovered = false;
            m_pressed = false;
            Invalidate();
        }
        void OnMouseDown(const MouseEvent&) override
        {
            m_pressed = true;
            Invalidate();
        }
        void OnMouseUp(const MouseEvent&) override
        {
            m_pressed = false;
            Invalidate();
        }

        Thickness m_padding{};
        bool m_hovered = false;
        bool m_pressed = false;
    };

    DRACONIC_DEFINE_OBJECT(UINode, "draconic::gui")
}
