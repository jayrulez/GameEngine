// Draconic UI - :thickness partition
//
// Thickness for padding, margin, and border - with symmetric constructors.
// Ported faithfully from Sedulous.UI/src/Core/Thickness.bf (PascalCase fields;
// Beef properties -> methods).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:thickness;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    struct Thickness
    {
        f32 Left = 0.0f;
        f32 Top = 0.0f;
        f32 Right = 0.0f;
        f32 Bottom = 0.0f;

        /// Zero thickness.
        constexpr Thickness() noexcept = default;
        /// All sides equal.
        explicit constexpr Thickness(f32 all) noexcept
            : Left(all), Top(all), Right(all), Bottom(all)
        {
        }
        /// Horizontal (left/right) and vertical (top/bottom) pairs.
        constexpr Thickness(f32 horizontal, f32 vertical) noexcept
            : Left(horizontal), Top(vertical), Right(horizontal), Bottom(vertical)
        {
        }
        /// Each side explicit.
        constexpr Thickness(f32 left, f32 top, f32 right, f32 bottom) noexcept
            : Left(left), Top(top), Right(right), Bottom(bottom)
        {
        }

        [[nodiscard]] constexpr f32 TotalHorizontal() const noexcept { return Left + Right; }
        [[nodiscard]] constexpr f32 TotalVertical() const noexcept { return Top + Bottom; }
        [[nodiscard]] constexpr bool IsZero() const noexcept
        {
            return Left == 0.0f && Top == 0.0f && Right == 0.0f && Bottom == 0.0f;
        }
    };
}
