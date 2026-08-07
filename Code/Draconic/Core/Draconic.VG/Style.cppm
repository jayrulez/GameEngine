// Draconic::VG - :style partition.
//
// Geometry/stroke style descriptors: CornerRadii (rounded rects) and
// StrokeStyle (width/cap/join/miter/dash). Ported from Sedulous.VG.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:style;

import draconic.foundation;
import :enums;

using namespace draconic::foundation;

export namespace draconic::vg
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
