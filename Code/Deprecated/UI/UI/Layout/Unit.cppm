// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :unit partition
//
// A length. Ported from Sedulous.UI/src/Layout/Unit.bf (a tagged value) and grown for the
// style model v2 (ui-layout-and-style-model.md, P1): a length is a SUM of components -
// dp, px, pt, a percentage of a reference box, and em of a font size - so `calc(100% - 20dp)`
// is a plain value with two components, and every unit resolves through one function.
// Dp/Px/Pt need only the DPI scale; Percent and Em need a reference and a font size, which the
// full Resolve takes and the DPI-only Resolve treats as absent (0).

module;
#include "Core/Prelude.h"

export module foundation.ui:unit;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    struct Unit
    {
        /// The dominant component (the one a single-unit value was built from) - kept for
        /// display and for callers that reason about "what unit is this".
        enum class Kind
        {
            Dp,
            Pt,
            Px,
            Percent,
            Em
        };

        Kind kind = Kind::Dp;
        /// Density-independent pixels - the LOGICAL layout unit (1dp = 1 device px at 96dpi).
        f32 dp = 0.0f;
        /// Points (1/72 inch); used for font sizes.
        f32 pt = 0.0f;
        /// PHYSICAL device pixels (divided out of the root draw scale).
        f32 px = 0.0f;
        /// Percent of the reference box on the same axis (100 = the whole box).
        f32 percent = 0.0f;
        /// Multiples of the computed font size.
        f32 em = 0.0f;

        constexpr Unit() noexcept = default;

        [[nodiscard]] static constexpr Unit Dp(f32 v) noexcept
        {
            Unit u;
            u.kind = Kind::Dp;
            u.dp = v;
            return u;
        }
        [[nodiscard]] static constexpr Unit Pt(f32 v) noexcept
        {
            Unit u;
            u.kind = Kind::Pt;
            u.pt = v;
            return u;
        }
        [[nodiscard]] static constexpr Unit Px(f32 v) noexcept
        {
            Unit u;
            u.kind = Kind::Px;
            u.px = v;
            return u;
        }
        [[nodiscard]] static constexpr Unit Percent(f32 v) noexcept
        {
            Unit u;
            u.kind = Kind::Percent;
            u.percent = v;
            return u;
        }
        [[nodiscard]] static constexpr Unit Em(f32 v) noexcept
        {
            Unit u;
            u.kind = Kind::Em;
            u.em = v;
            return u;
        }

        [[nodiscard]] constexpr bool operator==(const Unit&) const noexcept = default;

        /// calc(a + b): component-wise sum; the kind stays the left operand's.
        [[nodiscard]] constexpr Unit operator+(const Unit& other) const noexcept
        {
            Unit u = *this;
            u.dp += other.dp;
            u.pt += other.pt;
            u.px += other.px;
            u.percent += other.percent;
            u.em += other.em;
            return u;
        }
        /// calc(a - b).
        [[nodiscard]] constexpr Unit operator-(const Unit& other) const noexcept
        {
            Unit u = *this;
            u.dp -= other.dp;
            u.pt -= other.pt;
            u.px -= other.px;
            u.percent -= other.percent;
            u.em -= other.em;
            return u;
        }

        /// True when a Percent or Em component is present (the value needs a reference box or
        /// a font size to resolve - the DPI-only Resolve leaves those components out).
        [[nodiscard]] constexpr bool IsRelative() const noexcept
        {
            return percent != 0.0f || em != 0.0f;
        }

        /// Resolves the ABSOLUTE components to LOGICAL units. Layout runs entirely in logical
        /// space and the root applies DpiScale once at draw - so Dp is identity here (a
        /// `value * dpiScale` here would double-scale: once at resolve, again at draw), and Px
        /// divides by the scale so it lands on exact device pixels after the draw scale.
        [[nodiscard]] constexpr f32 Resolve(f32 dpiScale) const noexcept
        {
            return dp + pt * (96.0f / 72.0f) + (dpiScale > 0.0f ? px / dpiScale : px);
        }

        /// Resolves EVERY component: `referenceSize` is the containing box on this axis (logical
        /// units; 0 when unbounded) and `fontSize` the computed font size for em.
        [[nodiscard]] constexpr f32 Resolve(f32 dpiScale, f32 referenceSize, f32 fontSize) const noexcept
        {
            return Resolve(dpiScale) + percent * 0.01f * referenceSize + em * fontSize;
        }

        /// The dominant component's raw value, without DPI conversion.
        [[nodiscard]] constexpr f32 RawValue() const noexcept
        {
            switch (kind)
            {
            case Kind::Dp:
                return dp;
            case Kind::Pt:
                return pt;
            case Kind::Px:
                return px;
            case Kind::Percent:
                return percent;
            case Kind::Em:
                return em;
            }
            return dp;
        }
    };
}
