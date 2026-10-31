// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :box_constraints partition
//
// Immutable layout constraints carrying min/max on both axes (clamping math; no modes).

module;
#include "Core/Prelude.h"

export module foundation.ui:box_constraints;

import foundation.core; // Max, Min, kFloatMax
import :thickness;

using namespace foundation::core;

export namespace foundation::ui
{
    struct BoxConstraints
    {
        f32 MinWidth = 0.0f;
        f32 MaxWidth = 0.0f;
        f32 MinHeight = 0.0f;
        f32 MaxHeight = 0.0f;

        constexpr BoxConstraints() noexcept = default;
        constexpr BoxConstraints(f32 minWidth, f32 maxWidth, f32 minHeight, f32 maxHeight) noexcept
            : MinWidth(minWidth), MaxWidth(maxWidth), MinHeight(minHeight), MaxHeight(maxHeight)
        {
        }
        constexpr bool operator==(const BoxConstraints&) const noexcept = default;

        /// Exact size on both axes (min == max).
        [[nodiscard]] static constexpr BoxConstraints Tight(f32 width, f32 height) noexcept
        {
            return {width, width, height, height};
        }
        /// Zero minimum, specified maximum.
        [[nodiscard]] static constexpr BoxConstraints Loose(f32 maxWidth, f32 maxHeight) noexcept
        {
            return {0.0f, maxWidth, 0.0f, maxHeight};
        }
        /// Unconstrained on both axes.
        [[nodiscard]] static constexpr BoxConstraints Expand() noexcept
        {
            return {0.0f, kFloatMax, 0.0f, kFloatMax};
        }

        /// Shrinks constraints by padding/margin on all sides (clamped to >= 0).
        [[nodiscard]] BoxConstraints Deflate(Thickness padding) const noexcept
        {
            const f32 hPad = padding.Left + padding.Right;
            const f32 vPad = padding.Top + padding.Bottom;
            return {Max(0.0f, MinWidth - hPad), Max(0.0f, MaxWidth - hPad),
                    Max(0.0f, MinHeight - vPad), Max(0.0f, MaxHeight - vPad)};
        }

        /// Clamps a width/height to the constraint range.
        [[nodiscard]] f32 ConstrainWidth(f32 width) const noexcept
        {
            return Max(MinWidth, Min(width, MaxWidth));
        }
        [[nodiscard]] f32 ConstrainHeight(f32 height) const noexcept
        {
            return Max(MinHeight, Min(height, MaxHeight));
        }

        /// The bounded test used across layouts and controls, in place of ad hoc
        /// `< kFloatMax` or `< 100000` checks.
        [[nodiscard]] static constexpr bool IsBounded(f32 extent) noexcept
        {
            return extent < kFloatMax;
        }
        /// The available extent on an axis, or `fallback` when the parent gave no bound. What
        /// fill-style leaves (Separator/ProgressBar/Slider/ScrollBar/list views) use instead of
        /// MaxWidth/MaxHeight directly, which would measure to kFloatMax and break layouts under
        /// unbounded parents (a scroll axis, an Expand() container).
        [[nodiscard]] f32 BoundedMaxWidth(f32 fallback) const noexcept
        {
            return IsBounded(MaxWidth) ? MaxWidth : fallback;
        }
        [[nodiscard]] f32 BoundedMaxHeight(f32 fallback) const noexcept
        {
            return IsBounded(MaxHeight) ? MaxHeight : fallback;
        }

        /// Whether both axes are tight (min == max).
        [[nodiscard]] constexpr bool IsTight() const noexcept
        {
            return MinWidth == MaxWidth && MinHeight == MaxHeight;
        }
        /// Whether both axes have zero minimum.
        [[nodiscard]] constexpr bool IsLoose() const noexcept
        {
            return MinWidth == 0.0f && MinHeight == 0.0f;
        }

        /// A loose version (zero min, same max).
        [[nodiscard]] constexpr BoxConstraints Loosen() const noexcept
        {
            return {0.0f, MaxWidth, 0.0f, MaxHeight};
        }
        /// Tight constraints at the maximum size.
        [[nodiscard]] constexpr BoxConstraints TightenToMax() const noexcept
        {
            return {MaxWidth, MaxWidth, MaxHeight, MaxHeight};
        }
    };
}
