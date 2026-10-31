// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :ui_node partition
//
// UINode: a Node with UI chrome, ported from eepp's UI::UINode. It adds padding (a content inset)
// and the text style. Its look is its box, which style sheets set, and its states are what the
// selectors match.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:ui_node;

import foundation.core;  // RefPtr, Max, Move, Color
import foundation.fonts; // CachedFont
import :rect;
import :thickness;
import :event;
import :drawable;
import :text; // TextHAlign, TextVAlign, for the text-align hooks
import :node;
import :node_box; // PartStyle
import :style_properties; // StyleValue, for MotionValue

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    /// One declaration of a node's inline style, from markup attributes the node does not claim or
    /// from a script's style. Inline style is the cascade's highest origin.
    struct InlineStyleDeclaration
    {
        core::String Name;
        core::String Value;
    };

    /// One property a tween moves. The style pass composes it over the cascade, like CSS's
    /// animation origin, going from `From` (or from the value underneath when there is no `From`)
    /// toward `To`.
    struct MotionValue
    {
        core::String Property;
        StyleValue From;
        bool HasFrom = false;
        StyleValue To;
        f32 Progress = 0.0f; ///< eased progress; a curve that overshoots goes outside [0, 1]
        StyleValue Shown;    ///< the value the last style pass showed
        bool HasShown = false;
        StyleValue Under;    ///< the starting value: From, or the value underneath
        bool HasUnder = false;
        u64 Owner = 0; ///< the tween writing this value
    };

    class UINode : public Node
    {
        RTTI_OBJECT(UINode, Node)
    public:
        UINode() = default;

        // === Text style ===
        // Every node carries the text properties so they inherit down the tree, even through nodes
        // that draw no text (a panel's colour reaches the labels inside it). They are the colour,
        // the font (family and size, resolved to one cached font) and the alignment. A widget that
        // shows text reads them when OnTextStyleChange tells it they changed. Values set from code
        // are pinned as the code's own, and the subtree restyles because what it inherits changed.
        struct TextStyle
        {
            Color TextColor = Color::White;
            fonts::CachedFont* Font = nullptr;
            core::String FontFamily; ///< the font's family as CSS names it; empty when unknown
            /// The rest of the font-family list, the font-weight and the font-style.
            FontSelection Selection;
            f32 FontSize = 0.0f;     ///< in pixels; 0 when unknown
            TextHAlign HAlign = TextHAlign::Start;
            TextVAlign VAlign = TextVAlign::Top;
            TextWhiteSpace WhiteSpace = TextWhiteSpace::NoWrap; ///< a node's default is one line, although CSS's initial value is normal
            f32 LineHeight = 0.0f;        ///< pixels, or a multiple of the font size when LineHeightFactor; 0 means normal
            bool LineHeightFactor = false; ///< a unitless line-height, which each node applies to its own font size
            f32 LetterSpacing = 0.0f;     ///< pixels
            bool BreakWords = false;      ///< overflow-wrap: break-word or anywhere
            [[nodiscard]] bool operator==(const TextStyle& o) const noexcept
            {
                return TextColor == o.TextColor && Font == o.Font && FontFamily == o.FontFamily &&
                       Selection == o.Selection && FontSize == o.FontSize &&
                       HAlign == o.HAlign && VAlign == o.VAlign && WhiteSpace == o.WhiteSpace && LineHeight == o.LineHeight &&
                       LineHeightFactor == o.LineHeightFactor && LetterSpacing == o.LetterSpacing &&
                       BreakWords == o.BreakWords;
            }
            /// Returns the line height in pixels; 0 means the font's own.
            [[nodiscard]] f32 LineHeightPixels() const noexcept { return LineHeightFactor ? LineHeight * FontSize : LineHeight; }
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
            style.Selection.FallbackFamilies.Clear();
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
        /// Sets CSS white-space (inherited): whether spaces collapse and lines wrap.
        void SetWhiteSpace(TextWhiteSpace whiteSpace)
        {
            PinStyle(SlotBit(SlotWhiteSpace));
            TextStyle style = m_textStyle;
            style.WhiteSpace = whiteSpace;
            ChangeTextStyle(style);
        }
        /// Sets CSS line-height (inherited): pixels, or with `factor` a multiple of the font size;
        /// 0 means normal.
        void SetLineHeight(f32 value, bool factor = false)
        {
            PinStyle(SlotBit(SlotLineHeight));
            TextStyle style = m_textStyle;
            style.LineHeight = core::Max(0.0f, value);
            style.LineHeightFactor = factor;
            ChangeTextStyle(style);
        }
        /// Sets CSS letter-spacing (inherited), in pixels.
        void SetLetterSpacing(f32 pixels)
        {
            PinStyle(SlotBit(SlotLetterSpacing));
            TextStyle style = m_textStyle;
            style.LetterSpacing = pixels;
            ChangeTextStyle(style);
        }
        /// Sets CSS overflow-wrap (inherited): a word wider than its line breaks inside the word.
        void SetBreakWords(bool breakWords)
        {
            PinStyle(SlotBit(SlotOverflowWrap));
            TextStyle style = m_textStyle;
            style.BreakWords = breakWords;
            ChangeTextStyle(style);
        }
        /// Sets CSS font-weight (inherited), 100 to 900. The family's face with the nearest weight
        /// is used.
        void SetFontWeight(u16 weight)
        {
            PinStyle(SlotBit(SlotFontWeight));
            TextStyle style = m_textStyle;
            style.Selection.Weight = core::Clamp<u16>(weight, 1, 1000);
            ChangeTextStyle(style);
        }
        /// Sets CSS font-style (inherited). Italic uses the family's italic face, or slants the
        /// regular one.
        void SetFontItalic(bool italic)
        {
            PinStyle(SlotBit(SlotFontStyle));
            TextStyle style = m_textStyle;
            style.Selection.Italic = italic;
            ChangeTextStyle(style);
        }
        [[nodiscard]] TextWhiteSpace GetWhiteSpace() const noexcept { return m_textStyle.WhiteSpace; }
        [[nodiscard]] const TextStyle& GetTextStyle() const noexcept { return m_textStyle; }
        [[nodiscard]] Color GetTextColor() const noexcept { return m_textStyle.TextColor; }
        [[nodiscard]] fonts::CachedFont* GetFont() const noexcept { return m_textStyle.Font; }
        [[nodiscard]] TextHAlign GetTextAlignH() const noexcept { return m_textStyle.HAlign; }
        /// Returns the direction for the node's text paragraphs: the node's own direction, or with
        /// auto, each paragraph's detected direction.
        [[nodiscard]] ParagraphDirection TextDirectionFor() const
        {
            const bool rightToLeft = IsRightToLeft();
            if (IsDirectionAuto())
                return ParagraphDirection{fonts::TextDirection::Auto, rightToLeft};
            return ParagraphDirection{rightToLeft ? fonts::TextDirection::RightToLeft : fonts::TextDirection::LeftToRight, false};
        }
        [[nodiscard]] TextVAlign GetTextAlignV() const noexcept { return m_textStyle.VAlign; }
        /// Draws a modal popup's dimming backdrop: the `::backdrop` part, over the default dim.
        void DrawBackdrop(DrawContext& ctx, const Rect& bounds) const override
        {
            GetPartStyle(u8"backdrop").Draw(ctx, bounds, FilledBox(Color{0.0f, 0.0f, 0.0f, 0.45f}));
        }
        /// Applies the whole text style as a sheet computed it, without pinning; call inside a
        /// StyleWriteScope.
        void ApplyTextStyle(const TextStyle& style) { ChangeTextStyle(style); }
        /// Returns the node's own text style, captured when a sheet first styles it: the values that
        /// apply when no rule and no parent sets them.
        const TextStyle& CaptureUnstyledText()
        {
            if (!m_unstyledTextCaptured)
            {
                m_unstyledTextCaptured = true;
                m_unstyledText = m_textStyle;
            }
            return m_unstyledText;
        }
        /// Gives `child` this node's font and colour, without pinning them. Composite widgets use it
        /// to keep their parts in step where no sheet runs; where a sheet runs, inheritance gives
        /// the parts the same values.
        void ShareTextStyle(UINode& child, bool color = true) const
        {
            StyleWriteScope scope(child);
            TextStyle style = child.m_textStyle;
            style.Font = m_textStyle.Font;
            style.FontFamily = m_textStyle.FontFamily;
            style.Selection = m_textStyle.Selection;
            style.FontSize = m_textStyle.FontSize;
            if (color)
                style.TextColor = m_textStyle.TextColor;
            child.ChangeTextStyle(style);
        }

        // === Inline style ===
        /// Sets `name: value` in this node's inline style, replacing any declaration of that name.
        /// From the next restyle it wins over every sheet's normal declarations.
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

        // === The animated layer ===
        /// Returns the values tweens move on this node, applied over the cascade. A change marks the
        /// node for the style pass to apply them again (MarkStyleMotionDirty) without resolving the
        /// cascade anew.
        [[nodiscard]] core::Span<MotionValue> MotionValues() noexcept { return m_motion.AsSpan(); }
        [[nodiscard]] bool HasMotion() const noexcept { return !m_motion.IsEmpty(); }
        [[nodiscard]] MotionValue* FindMotion(core::StringView property) noexcept
        {
            for (MotionValue& m : m_motion)
                if (m.Property.AsView() == property)
                    return &m;
            return nullptr;
        }
        /// Sets the animated value for its property, replacing any existing one.
        MotionValue& SetMotion(MotionValue value)
        {
            MarkStyleMotionDirty();
            if (MotionValue* m = FindMotion(value.Property.AsView()))
                return *m = core::Move(value);
            m_motion.PushBack(core::Move(value));
            return m_motion[m_motion.Size() - 1];
        }
        /// Returns `property` to the cascade. Returns false when nothing was moving it.
        bool RemoveMotion(core::StringView property)
        {
            for (usize i = 0; i < m_motion.Size(); ++i)
                if (m_motion[i].Property.AsView() == property)
                {
                    m_motion.RemoveAt(i);
                    MarkStyleMotionDirty();
                    return true;
                }
            return false;
        }

        /// A reflected property that a sheet set by its CSS name. It records the node's own value
        /// from before, the value the sheet last wrote (as text; empty when the sheet no longer
        /// writes one), and whether code has since set the property.
        struct StyledProperty
        {
            core::String Name;
            Variant Own;
            core::String Written;
            bool Code = false;
        };
        [[nodiscard]] core::Array<StyledProperty>& StyledProperties() noexcept { return m_styledProperties; }

        // Markup hook: a widget consumes its own structural XML attributes (a number field's
        // "prefix" and "suffix", a table item's "cells") and returns true if it handled `name`.
        // The markup loader applies unclaimed attributes as CSS properties. By default no attribute
        // is claimed.
        virtual bool SetMarkupAttribute(core::StringView /*name*/, core::StringView /*value*/)
        {
            return false;
        }
        /// Markup hook: receives a `<template type="...">` child's markup (its first element, as
        /// text). A widget that makes nodes from templates (an item view's rows) keeps it. Returns
        /// false when the widget takes no templates, and the loader then drops it.
        virtual bool SetMarkupTemplate(core::StringView /*type*/, core::StringView /*markup*/) { return false; }

        // Pseudo-element parts: a widget names the parts it paints (a slider's "track", "fill" and
        // "thumb", a checkbox's "box" and "mark", a window's "title" and "grip") so CSS `tag::part`
        // can style them with the box properties and opacity. CollectStyleParts lists them, the
        // sheet sets each one's PartStyle, and the widget draws a part through GetPartStyle(part).
        virtual void CollectStyleParts(core::Array<core::StringView>&) const {}
        void SetPartStyle(core::StringView part, const PartStyle& style)
        {
            for (NamedPartStyle& p : m_partStyles)
                if (p.Name.AsView() == part)
                {
                    p.Style = style;
                    OnPartStyleChange(part);
                    Invalidate();
                    return;
                }
            m_partStyles.PushBack(NamedPartStyle{core::String(part), style});
            OnPartStyleChange(part);
            Invalidate();
        }
        /// Returns the sheet's style for `part`, or an empty style when the sheet does not style it.
        [[nodiscard]] const PartStyle& GetPartStyle(core::StringView part) const; // defined in GUIClusterImpl.cpp
        [[nodiscard]] const PartStyle* FindPartStyle(core::StringView part) const override { return &GetPartStyle(part); }

    protected:
        /// Called when the text style changed, so a widget showing text refreshes what it draws with.
        virtual void OnTextStyleChange() {}
        /// Called when the direction may have changed; widgets showing text pick it up in
        /// OnTextStyleChange.
        void OnDirectionChange() override
        {
            const ParagraphDirection direction = TextDirectionFor();
            if (direction == m_textDirection)
                return; // unchanged direction, so joining a tree that runs the same way costs nothing
            m_textDirection = direction;
            OnTextStyleChange();
            RequestLayout();
            Invalidate();
        }
        /// Called when a part's style changed; a widget whose part is a separate node passes it on.
        virtual void OnPartStyleChange(core::StringView /*part*/) {}

    private:
        void ChangeTextStyle(const TextStyle& style)
        {
            if (style == m_textStyle)
                return;
            m_textStyle = style;
            if (!IsStyleWriting())
                MarkStyleDirty(true); // the subtree's inherited values changed
            OnTextStyleChange();
            RequestLayout();
            Invalidate();
        }

        TextStyle m_textStyle;
        ParagraphDirection m_textDirection; // the direction last passed to the text
        TextStyle m_unstyledText;
        bool m_unstyledTextCaptured = false;
        core::Array<InlineStyleDeclaration> m_inlineStyle;
        core::Array<MotionValue> m_motion; // values tweens move, applied over the cascade
        core::Array<StyledProperty> m_styledProperties;
        struct NamedPartStyle
        {
            core::String Name;
            PartStyle Style;
        };
        core::Array<NamedPartStyle> m_partStyles;
    };
}
