// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Image - :nine_slice partition.
//
// NineSlice: border insets for 9-slice image scaling. Corners stay fixed,
// edges stretch along one axis, the center stretches both ways.

module;
#include "Core/Prelude.h"

export module foundation.image:nine_slice;

import foundation.core;

using namespace foundation::core;

export namespace foundation::image
{
    /// Border insets (in pixels) for 9-slice image scaling.
    struct NineSlice
    {
        f32 left = 0.0f;   ///< Left border width.
        f32 top = 0.0f;    ///< Top border height.
        f32 right = 0.0f;  ///< Right border width.
        f32 bottom = 0.0f; ///< Bottom border height.

        constexpr NineSlice() noexcept = default;

        constexpr NineSlice(f32 inLeft, f32 inTop, f32 inRight, f32 inBottom) noexcept
            : left(inLeft), top(inTop), right(inRight), bottom(inBottom)
        {
        }

        /// Uniform borders on all sides.
        explicit constexpr NineSlice(f32 all) noexcept
            : left(all), top(all), right(all), bottom(all)
        {
        }

        /// Horizontal and vertical borders.
        constexpr NineSlice(f32 horizontal, f32 vertical) noexcept
            : left(horizontal), top(vertical), right(horizontal), bottom(vertical)
        {
        }

        [[nodiscard]] constexpr f32 HorizontalBorder() const noexcept { return left + right; }
        [[nodiscard]] constexpr f32 VerticalBorder() const noexcept { return top + bottom; }
        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return left > 0.0f || top > 0.0f || right > 0.0f || bottom > 0.0f;
        }
    };
}
