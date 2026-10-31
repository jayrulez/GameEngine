// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_property partition
//
// Identifies a style property settable in a StyleRule. COUNT sizes property arrays.

export module foundation.ui:style_property;

export namespace foundation::ui
{
    enum class StyleProperty
    {
        // Drawable properties
        Background,
        CheckedBackground,
        MenuItemHoverDrawable,

        // Color properties
        TextColor,
        TextDimColor,
        PlaceholderColor,
        BorderColor,
        CursorColor,
        SelectionColor,
        AccentColor,
        // Semantic status colors (palette $success/$warning/$error; toasts, validation).
        SuccessColor,
        WarningColor,
        ErrorColor,

        // Float properties
        FontSize,
        FontFamily,
        CornerRadius,
        BorderWidth,
        Spacing,
        Opacity,
        Width,
        Height,

        // Thickness properties
        Padding,
        Margin,

        // Bool properties
        WordWrap,

        // === Box model (appended: the parser's range predicates depend on the order above) ===
        /// `box-shadow: x y blur spread color [inset]` (a Shadow value).
        BoxShadow,
        // Length properties feeding LayoutStyle (see View::RefreshEffectiveLayout).
        MinWidth,
        MinHeight,
        MaxWidth,
        MaxHeight,
        Top,
        Right,
        Bottom,
        Left,
        // Float properties feeding LayoutStyle.
        ZIndex,
        FlexGrow,
        FlexShrink,
        // Keyword (String) properties.
        /// `static` | `absolute`.
        Position,
        /// `visible` | `hidden` (hidden clips children to the border box).
        Overflow,
        /// `start` | `end` | `center` | `stretch` | `baseline`.
        AlignSelf,

        // === Transitions ===
        /// `transition: <property|all> <duration> [<easing>] [<delay>], ...` | `none`.
        Transition,

        // === Wrap, gap and ellipsis ===
        /// Length feeding LayoutStyle::FlexBasis.
        FlexBasis,
        /// `clip` | `ellipsis` (single-line text views truncate with the font's ellipsis).
        TextOverflow,

        // === Filters (VG layers) ===
        /// `filter: blur(<sigma>) grayscale(<a>) saturate(<a>) brightness(<a>) | none` (a Filter, or a
        /// plain number as a blur's sigma): the view and its children drawn as one layer, blurred by
        /// a Gaussian of that sigma in dp and their colour filtered at its composite.
        Filter,
        /// `backdrop-filter: blur(<sigma>) | none` (a Filter, or a plain number): what is drawn behind
        /// the view blurred inside its corners, under its own background. Only the blur is drawn.
        BackdropFilter,

        // === Text effects (VG TextEffects, drawn under the text) ===
        // A Shadow value each. They carry to what the view draws and its children, as CSS's
        // text-shadow does, through the VG's state rather than the cascade; `none` turns one off.
        /// `text-shadow: <x> <y> [blur] [color]`.
        TextShadow,
        /// `text-outline: <width> [color]` (the width is stored as the shadow's spread).
        TextOutline,
        /// `text-glow: <radius> [color]` (the radius is stored as the shadow's blur).
        TextGlow,

        // === Motion ===
        /// `transform: <function> ... | none` (a Transform): translate, scale, rotate and skew, and
        /// their X and Y forms, composed in written order about the transform-origin; drawn and
        /// hit-tested over the view's own Transform, while layout keeps its box.
        Transform,
        /// `transform-origin: <x> [<y>]` (a Point): keywords, lengths or percentages of the view;
        /// the centre when unset.
        TransformOrigin,
        /// `animation: <name> <duration> [timing] [delay] [count | infinite] [direction] [fill], ...
        /// | none` (Animations): plays the sheet's @keyframes of that name on the view; while
        /// one runs, the properties it animates take its values over the cascade's.
        Animation,

        /// Number of known properties (for array sizing).
        COUNT
    };

    /// Properties whose change never moves geometry: a transition on one of these marks
    /// redraw damage per frame, and on any other marks layout damage. The property's kind
    /// decides, not the rule it came from.
    [[nodiscard]] constexpr bool IsVisualOnlyStyleProperty(StyleProperty prop) noexcept
    {
        switch (prop)
        {
        case StyleProperty::Background:
        case StyleProperty::CheckedBackground:
        case StyleProperty::MenuItemHoverDrawable:
        case StyleProperty::TextColor:
        case StyleProperty::TextDimColor:
        case StyleProperty::PlaceholderColor:
        case StyleProperty::BorderColor:
        case StyleProperty::CursorColor:
        case StyleProperty::SelectionColor:
        case StyleProperty::AccentColor:
        case StyleProperty::SuccessColor:
        case StyleProperty::WarningColor:
        case StyleProperty::ErrorColor:
        case StyleProperty::CornerRadius:
        case StyleProperty::Opacity:
        case StyleProperty::BoxShadow:
        case StyleProperty::ZIndex:
        case StyleProperty::Overflow:
        case StyleProperty::Transition:
        case StyleProperty::TextOverflow:
        case StyleProperty::Filter:
        case StyleProperty::BackdropFilter:
        case StyleProperty::TextShadow:
        case StyleProperty::TextOutline:
        case StyleProperty::TextGlow:
        case StyleProperty::Transform:
        case StyleProperty::TransformOrigin:
            return true;
        default:
            return false;
        }
    }

    /// Properties a `transition` can animate: colors, floats, thicknesses, lengths, the box
    /// shadow, and Background (drawables cross-fade rather than interpolate).
    [[nodiscard]] constexpr bool IsAnimatableStyleProperty(StyleProperty prop) noexcept
    {
        switch (prop)
        {
        case StyleProperty::Background:
        case StyleProperty::TextColor:
        case StyleProperty::TextDimColor:
        case StyleProperty::PlaceholderColor:
        case StyleProperty::BorderColor:
        case StyleProperty::CursorColor:
        case StyleProperty::SelectionColor:
        case StyleProperty::AccentColor:
        case StyleProperty::SuccessColor:
        case StyleProperty::WarningColor:
        case StyleProperty::ErrorColor:
        case StyleProperty::FontSize:
        case StyleProperty::CornerRadius:
        case StyleProperty::BorderWidth:
        case StyleProperty::Spacing:
        case StyleProperty::Opacity:
        case StyleProperty::Width:
        case StyleProperty::Height:
        case StyleProperty::Padding:
        case StyleProperty::Margin:
        case StyleProperty::BoxShadow:
        case StyleProperty::MinWidth:
        case StyleProperty::MinHeight:
        case StyleProperty::MaxWidth:
        case StyleProperty::MaxHeight:
        case StyleProperty::Top:
        case StyleProperty::Right:
        case StyleProperty::Bottom:
        case StyleProperty::Left:
        case StyleProperty::FlexGrow:
        case StyleProperty::FlexShrink:
        case StyleProperty::FlexBasis:
        case StyleProperty::Filter:
        case StyleProperty::BackdropFilter:
        case StyleProperty::TextShadow:
        case StyleProperty::TextOutline:
        case StyleProperty::TextGlow:
        case StyleProperty::Transform:
        case StyleProperty::TransformOrigin:
            return true;
        default:
            return false;
        }
    }
}
