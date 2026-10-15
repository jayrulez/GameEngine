// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_property partition
//
// Identifies a style property settable in a StyleRule. COUNT sizes property arrays.
// Ported from Sedulous.UI/src/Styling/StyleProperty.bf.

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

        // === P2 box model (appended: the parser's range predicates depend on the order above) ===
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

        // === P3 transitions ===
        /// `transition: <property|all> <duration> [<easing>] [<delay>], ...` | `none`.
        Transition,

        // === P4 wrap / gap / ellipsis ===
        /// Length feeding LayoutStyle::FlexBasis.
        FlexBasis,
        /// `clip` | `ellipsis` (single-line text views truncate with the font's ellipsis).
        TextOverflow,

        // === Filters (VG layers) ===
        /// `filter: blur(<sigma>) | none`: the view and its children blurred, a Gaussian of that
        /// sigma in dp (a Float, 0 for none).
        Filter,
        /// `backdrop-filter: blur(<sigma>) | none`: what is drawn behind the view blurred inside
        /// its corner radius, under its own background (a Float, 0 for none).
        BackdropFilter,

        /// Number of known properties (for array sizing).
        COUNT
    };

    /// Properties whose change never moves geometry: a transition on one of these marks
    /// redraw damage per frame, the others mark layout damage (the property's kind decides,
    /// never the rule's - see the spec's layout-gate gotcha).
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
            return true;
        default:
            return false;
        }
    }
}
