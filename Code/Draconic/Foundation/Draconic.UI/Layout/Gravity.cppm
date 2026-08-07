// Draconic UI - :gravity partition
//
// Alignment flags for positioning a view within its parent's available space (combine H + V with |).
// Ported from Sedulous.UI/src/Layout/Gravity.bf (Beef flags enum -> enum class + bitwise operators).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:gravity;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    enum class Gravity : u32
    {
        None = 0,

        // Horizontal
        Left = 1,
        Right = 2,
        CenterH = 4,
        FillH = 8,

        // Vertical
        Top = 16,
        Bottom = 32,
        CenterV = 64,
        FillV = 128,

        // Combined
        Center = CenterH | CenterV,
        Fill = FillH | FillV,
        TopLeft = Top | Left,
        TopRight = Top | Right,
        BottomLeft = Bottom | Left,
        BottomRight = Bottom | Right,
    };

    [[nodiscard]] constexpr Gravity operator|(Gravity a, Gravity b) noexcept
    {
        return static_cast<Gravity>(static_cast<u32>(a) | static_cast<u32>(b));
    }
    [[nodiscard]] constexpr Gravity operator&(Gravity a, Gravity b) noexcept
    {
        return static_cast<Gravity>(static_cast<u32>(a) & static_cast<u32>(b));
    }
    [[nodiscard]] constexpr bool HasFlag(Gravity value, Gravity flag) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0u;
    }
}
