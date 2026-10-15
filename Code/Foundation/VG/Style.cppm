// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG - :style partition.
//
// Geometry/stroke style descriptors: CornerRadii (rounded rects) and
// StrokeStyle (width/cap/join/miter/dash). Ported from Sedulous.VG.

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
}
