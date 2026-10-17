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

        // === Text style (E3) ===
        // Every node carries the text properties, so they inherit down the tree through nodes
        // that draw no text (a panel's colour reaches the labels in it): the colour, the font
        // (family and size, resolved to one cached font), and the alignment. A widget showing text
        // reads them, told by OnTextStyleChange; from code they are the code's (pinned), and the
        // subtree restyles, as what it inherits changed.
        struct TextStyle
        {
            Color TextColor = Color::White;
            fonts::CachedFont* Font = nullptr;
            core::String FontFamily; ///< the font's family as CSS names it (empty: unknown)
            f32 FontSize = 0.0f;     ///< in pixels (0: unknown)
            TextHAlign HAlign = TextHAlign::Left;
            TextVAlign VAlign = TextVAlign::Top;
            [[nodiscard]] bool operator==(const TextStyle& o) const noexcept
            {
                return TextColor == o.TextColor && Font == o.Font && FontFamily == o.FontFamily && FontSize == o.FontSize &&
                       HAlign == o.HAlign && VAlign == o.VAlign;
            }
        };
        void SetTextColor(Color color)
        {
            PinStyle(SlotBit(SlotColor));
            TextStyle style = m_textStyle;
            style.TextColor = color;
            ChangeTextStyle(style);
        }
        void SetFont(fonts::CachedFont* font)
        {
            PinStyle(SlotBit(SlotFontFamily) | SlotBit(SlotFontSize));
            TextStyle style = m_textStyle;
            style.Font = font;
            style.FontFamily = font != nullptr && font->font != nullptr ? core::String(font->font->FamilyName()) : core::String{};
            style.FontSize = font != nullptr && font->font != nullptr ? font->font->PixelHeight() : 0.0f;
            ChangeTextStyle(style);
        }
        void SetTextAlignment(TextHAlign horizontal, TextVAlign vertical)
        {
            PinStyle(SlotBit(SlotTextAlign) | SlotBit(SlotVerticalAlign));
            TextStyle style = m_textStyle;
            style.HAlign = horizontal;
            style.VAlign = vertical;
            ChangeTextStyle(style);
        }
        [[nodiscard]] const TextStyle& GetTextStyle() const noexcept { return m_textStyle; }
        [[nodiscard]] Color GetTextColor() const noexcept { return m_textStyle.TextColor; }
        [[nodiscard]] fonts::CachedFont* GetFont() const noexcept { return m_textStyle.Font; }
        [[nodiscard]] TextHAlign GetTextAlignH() const noexcept { return m_textStyle.HAlign; }
        [[nodiscard]] TextVAlign GetTextAlignV() const noexcept { return m_textStyle.VAlign; }
        /// The whole text style as a sheet computed it (no pins; inside a StyleWriteScope).
        void ApplyTextStyle(const TextStyle& style) { ChangeTextStyle(style); }
        /// Its own text style, captured when a sheet first styles it (what no rule and no parent gives).
        const TextStyle& CaptureUnstyledText()
        {
            if (!m_unstyledTextCaptured)
            {
                m_unstyledTextCaptured = true;
                m_unstyledText = m_textStyle;
            }
            return m_unstyledText;
        }
        /// Gives `child` this node's font and colour as its own (no pin): a composite widget's
        /// parts, kept in step where no sheet runs; a sheet's inheritance gives them the same.
        void ShareTextStyle(UINode& child, bool color = true) const
        {
            StyleWriteScope scope(child);
            TextStyle style = child.m_textStyle;
            style.Font = m_textStyle.Font;
            style.FontFamily = m_textStyle.FontFamily;
            style.FontSize = m_textStyle.FontSize;
            if (color)
                style.TextColor = m_textStyle.TextColor;
            child.ChangeTextStyle(style);
        }

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

    protected:
        /// The text style changed: a widget showing text refreshes what it draws with.
        virtual void OnTextStyleChange() {}

    private:
        void ChangeTextStyle(const TextStyle& style)
        {
            if (style == m_textStyle)
                return;
            m_textStyle = style;
            if (!IsStyleWriting())
                MarkStyleDirty(true); // what the subtree inherits changed
            OnTextStyleChange();
            RequestLayout();
            Invalidate();
        }

        TextStyle m_textStyle;
        TextStyle m_unstyledText;
        bool m_unstyledTextCaptured = false;
        core::Array<InlineStyleDeclaration> m_inlineStyle;
        core::Array<StyledProperty> m_styledProperties;
    };
}
