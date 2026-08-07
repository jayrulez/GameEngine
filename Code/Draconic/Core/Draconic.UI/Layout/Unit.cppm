// Draconic UI - :unit partition
//
// Type-safe dimensional value carrying intent (dp/pt/px); resolves to pixels at layout time given
// the DPI scale. Ported from Sedulous.UI/src/Layout/Unit.bf. Beef discriminated union (all cases
// carry a float) -> a kind + value struct (language divergence).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:unit;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    struct Unit
    {
        enum class Kind
        {
            Dp,
            Pt,
            Px
        };

        Kind kind = Kind::Dp;
        f32 value = 0.0f;

        constexpr Unit() noexcept = default;
        constexpr Unit(Kind k, f32 v) noexcept : kind(k), value(v) {}

        /// Density-independent pixels (1dp = 1px at 96dpi).
        [[nodiscard]] static constexpr Unit Dp(f32 v) noexcept { return {Kind::Dp, v}; }
        /// Points (1/72 inch); used for font sizes.
        [[nodiscard]] static constexpr Unit Pt(f32 v) noexcept { return {Kind::Pt, v}; }
        /// Raw pixels (no DPI scaling).
        [[nodiscard]] static constexpr Unit Px(f32 v) noexcept { return {Kind::Px, v}; }

        /// Resolves to pixels given the DPI scale (1.0 at 96dpi, 2.0 at 192dpi).
        [[nodiscard]] constexpr f32 Resolve(f32 dpiScale) const noexcept
        {
            switch (kind)
            {
            case Kind::Dp:
                return value * dpiScale;
            case Kind::Pt:
                return value * dpiScale * (96.0f / 72.0f);
            case Kind::Px:
                return value;
            }
            return value;
        }

        /// Raw value without DPI conversion.
        [[nodiscard]] constexpr f32 RawValue() const noexcept { return value; }
    };
}
