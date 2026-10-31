// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :style partition.
//
// Geometry and stroke style descriptors: CornerRadii (rounded rects), StrokeStyle (width, cap,
// join, miter, dash) and TextEffects (an outline, a shadow and a glow drawn under text).

module;
#include "Core/Prelude.h"

export module foundation.vg:style;

import foundation.core;
import :enums;

using namespace foundation::core;

export namespace foundation::vg
{
    /// Per-corner radii for rounded rectangles.
    struct CornerRadii
    {
        f32 topLeft = 0.0f;
        f32 topRight = 0.0f;
        f32 bottomRight = 0.0f;
        f32 bottomLeft = 0.0f;

        constexpr CornerRadii() noexcept = default;

        /// All corners with the same radius.
        explicit constexpr CornerRadii(f32 uniform) noexcept
            : topLeft(uniform), topRight(uniform), bottomRight(uniform), bottomLeft(uniform)
        {
        }

        /// Each corner with a different radius.
        constexpr CornerRadii(f32 inTopLeft, f32 inTopRight, f32 inBottomRight,
                              f32 inBottomLeft) noexcept
            : topLeft(inTopLeft), topRight(inTopRight), bottomRight(inBottomRight),
              bottomLeft(inBottomLeft)
        {
        }

        /// Whether all corners have the same radius.
        [[nodiscard]] constexpr bool IsUniform() const noexcept
        {
            return topLeft == topRight && topRight == bottomRight && bottomRight == bottomLeft;
        }

        /// Whether all corners have zero radius.
        [[nodiscard]] constexpr bool IsZero() const noexcept
        {
            return topLeft == 0.0f && topRight == 0.0f && bottomRight == 0.0f && bottomLeft == 0.0f;
        }

        [[nodiscard]] constexpr bool operator==(const CornerRadii&) const noexcept = default;
    };

    /// A box's border width on each side (VGContext::DrawBox).
    struct BorderWidths
    {
        f32 top = 0.0f;
        f32 right = 0.0f;
        f32 bottom = 0.0f;
        f32 left = 0.0f;

        constexpr BorderWidths() noexcept = default;

        /// Every side the same width.
        explicit constexpr BorderWidths(f32 uniform) noexcept
            : top(uniform), right(uniform), bottom(uniform), left(uniform)
        {
        }

        constexpr BorderWidths(f32 inTop, f32 inRight, f32 inBottom, f32 inLeft) noexcept
            : top(inTop), right(inRight), bottom(inBottom), left(inLeft)
        {
        }

        [[nodiscard]] constexpr bool IsZero() const noexcept
        {
            return top == 0.0f && right == 0.0f && bottom == 0.0f && left == 0.0f;
        }

        [[nodiscard]] constexpr bool operator==(const BorderWidths&) const noexcept = default;
    };

    /// A box's border colour on each side (VGContext::DrawBox); where two sides meet, the corner
    /// splits along the line from the outer corner to the inner one, as CSS draws it.
    struct BorderColors
    {
        Color top = Color{0.0f, 0.0f, 0.0f, 0.0f};
        Color right = Color{0.0f, 0.0f, 0.0f, 0.0f};
        Color bottom = Color{0.0f, 0.0f, 0.0f, 0.0f};
        Color left = Color{0.0f, 0.0f, 0.0f, 0.0f};

        constexpr BorderColors() noexcept = default;

        /// Every side the same colour.
        explicit constexpr BorderColors(Color uniform) noexcept
            : top(uniform), right(uniform), bottom(uniform), left(uniform)
        {
        }

        constexpr BorderColors(Color inTop, Color inRight, Color inBottom, Color inLeft) noexcept
            : top(inTop), right(inRight), bottom(inBottom), left(inLeft)
        {
        }

        [[nodiscard]] constexpr bool IsUniform() const noexcept
        {
            return top == right && top == bottom && top == left;
        }
        [[nodiscard]] constexpr bool IsTransparent() const noexcept
        {
            return top.a <= 0.0f && right.a <= 0.0f && bottom.a <= 0.0f && left.a <= 0.0f;
        }

        [[nodiscard]] constexpr bool operator==(const BorderColors&) const noexcept = default;
    };

    /// Style parameters for path stroking.
    struct StrokeStyle
    {
        f32 width = 1.0f;                    ///< Stroke width in pixels.
        VGLineCap cap = VGLineCap::Butt;     ///< Line cap style.
        VGLineJoin join = VGLineJoin::Miter; ///< Line join style.
        f32 miterLimit = 4.0f; ///< Miter limit (ratio of miter length to stroke width).
        f32 dashOffset = 0.0f; ///< Dash pattern offset.

        constexpr StrokeStyle() noexcept = default;

        explicit constexpr StrokeStyle(f32 inWidth) noexcept : width(inWidth) {}

        constexpr StrokeStyle(f32 inWidth, VGLineCap inCap, VGLineJoin inJoin,
                              f32 inMiterLimit = 4.0f) noexcept
            : width(inWidth), cap(inCap), join(inJoin), miterLimit(inMiterLimit)
        {
        }
    };

    /// Effects drawn under text (VGContext::SetTextEffects), in the text's own pixels: a shadow,
    /// then a glow, then an outline, then the glyphs. The glow and the outline, and a shadow's
    /// blur, read a distance-field font's true distance; a coverage font draws a shadow's offset
    /// copy only. They reach as far past the edge as the field holds: half the atlas's distance
    /// range, in its bake's pixels (2 px for the default 4-texel range at the bake size), and
    /// what asks for more is scaled down to fit.
    struct TextEffects
    {
        f32 outlineWidth = 0.0f; ///< Pixels past the edge; 0 for none.
        Color outlineColor = Color{0.0f, 0.0f, 0.0f, 1.0f};
        Float2 shadowOffset{};   ///< The shadow's offset from the text, in pixels.
        f32 shadowBlur = 0.0f;   ///< The width of the shadow's soft edge, in pixels; 0 for hard.
        Color shadowColor = Color{0.0f, 0.0f, 0.0f, 0.0f}; ///< Transparent: no shadow.
        f32 glowRadius = 0.0f;   ///< How far the glow fades out past the edge; 0 for none.
        Color glowColor = Color{1.0f, 1.0f, 1.0f, 1.0f};

        [[nodiscard]] constexpr bool HasOutline() const noexcept
        {
            return outlineWidth > 0.0f && outlineColor.a > 0.0f;
        }
        [[nodiscard]] constexpr bool HasShadow() const noexcept
        {
            return shadowColor.a > 0.0f && (shadowOffset.x != 0.0f || shadowOffset.y != 0.0f || shadowBlur > 0.0f);
        }
        [[nodiscard]] constexpr bool HasGlow() const noexcept { return glowRadius > 0.0f && glowColor.a > 0.0f; }
        [[nodiscard]] constexpr bool Any() const noexcept { return HasOutline() || HasShadow() || HasGlow(); }
        [[nodiscard]] constexpr bool operator==(const TextEffects&) const noexcept = default;
    };
}
