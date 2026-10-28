// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :control_state partition
//
// Visual state of a control as bit flags (combinable, e.g. Checked | Hover).
// StateListDrawable uses these for drawable lookup with fallback; .sss selectors match
// compound states. Ported from Sedulous.UI/src/Drawing/ControlState.bf (Beef flags enum
// -> C++ enum class + bitwise operators).

module;
#include "Core/Prelude.h"

export module foundation.ui:control_state;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    enum class ControlState : u32
    {
        Normal = 0,
        Hover = 1,
        Pressed = 2,
        Focused = 4,
        Disabled = 8,
        Checked = 16,
        Indeterminate = 32,
    };

    [[nodiscard]] constexpr ControlState operator|(ControlState a, ControlState b) noexcept
    {
        return static_cast<ControlState>(static_cast<u32>(a) | static_cast<u32>(b));
    }
    [[nodiscard]] constexpr ControlState operator&(ControlState a, ControlState b) noexcept
    {
        return static_cast<ControlState>(static_cast<u32>(a) & static_cast<u32>(b));
    }
    [[nodiscard]] constexpr ControlState operator~(ControlState a) noexcept
    {
        return static_cast<ControlState>(~static_cast<u32>(a));
    }
    constexpr ControlState& operator|=(ControlState& a, ControlState b) noexcept
    {
        a = a | b;
        return a;
    }
    constexpr ControlState& operator&=(ControlState& a, ControlState b) noexcept
    {
        a = a & b;
        return a;
    }

    /// True if `flag` is set in `value`.
    [[nodiscard]] constexpr bool HasFlag(ControlState value, ControlState flag) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0u;
    }
    /// True when EVERY bit of `flags` is set in `value` (a compound selector `:hover:checked`
    /// needs both; HasFlag answers for any one of them).
    [[nodiscard]] constexpr bool HasAllFlags(ControlState value, ControlState flags) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flags)) == static_cast<u32>(flags);
    }
    /// True if any flag is set (i.e. not Normal).
    [[nodiscard]] constexpr bool Any(ControlState value) noexcept
    {
        return static_cast<u32>(value) != 0u;
    }
}
