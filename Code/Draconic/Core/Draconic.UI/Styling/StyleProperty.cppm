// Draconic UI - :style_property partition
//
// Identifies a style property settable in a StyleRule. COUNT sizes property arrays.
// Ported from Sedulous.UI/src/Styling/StyleProperty.bf.

export module draconic.ui:style_property;

export namespace draconic::ui
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

        /// Number of known properties (for array sizing).
        COUNT
    };
}
