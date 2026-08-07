// Draconic Foundation - :rectangle partition
//
// Rectangle: 2D rectangle (x,y is the min corner) with Contains/Intersects.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:rectangle;

import :base;
import :float2;

export namespace draconic::foundation
{
    // =======================================================================
    // Rectangle - 2D rectangle (x, y is the min corner).
    // =======================================================================
    struct Rectangle
    {
        f32 x;
        f32 y;
        f32 width;
        f32 height;

        [[nodiscard]] Float2 Min() const noexcept { return Float2{x, y}; }
        [[nodiscard]] Float2 Max() const noexcept { return Float2{x + width, y + height}; }
        [[nodiscard]] Float2 Center() const noexcept
        {
            return Float2{x + width * 0.5f, y + height * 0.5f};
        }

        [[nodiscard]] bool Contains(Float2 p) const noexcept
        {
            return p.x >= x && p.x <= x + width && p.y >= y && p.y <= y + height;
        }

        [[nodiscard]] bool Intersects(const Rectangle& other) const noexcept
        {
            return x <= other.x + other.width && x + width >= other.x &&
                   y <= other.y + other.height && y + height >= other.y;
        }

        // The overlapping rectangle of two rects. Empty (zero size) if disjoint.
        // (Uses ternaries rather than the free Min/Max, which the Min()/Max()
        // member accessors above would shadow inside this scope.)
        [[nodiscard]] static Rectangle Intersect(const Rectangle& a, const Rectangle& b) noexcept
        {
            const f32 ax1 = a.x + a.width, bx1 = b.x + b.width;
            const f32 ay1 = a.y + a.height, by1 = b.y + b.height;
            const f32 x0 = a.x > b.x ? a.x : b.x;
            const f32 y0 = a.y > b.y ? a.y : b.y;
            const f32 x1 = ax1 < bx1 ? ax1 : bx1;
            const f32 y1 = ay1 < by1 ? ay1 : by1;
            const f32 w = (x1 - x0) > 0.0f ? (x1 - x0) : 0.0f;
            const f32 h = (y1 - y0) > 0.0f ? (y1 - y0) : 0.0f;
            return Rectangle{x0, y0, w, h};
        }
    };
}
