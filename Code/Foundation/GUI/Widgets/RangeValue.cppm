// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :range_value partition
//
// RangeValue: the value behind a slider, a scroll bar, a progress bar or a number field. It is a
// value between a minimum and a maximum, snapped to a step, with a page. For a scroll bar the page
// is the visible part (PageShown), and the value goes no further than the maximum less the page;
// for a slider or a number field the page is the Page Up and Page Down step. The value can be
// exponential, placed along the track by logarithm (for a frequency or a zoom), and it can be
// allowed past either end (a number typed beyond its range). The controls derive from
// RangeWidget, which holds a RangeValue and reflects its properties; that is why a progress bar
// carries a page it keeps hidden.

module;
#include "Core/Prelude.h"

export module foundation.gui:range_value;

import foundation.core; // Max, Min, Log, Exp, Abs

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    struct RangeValue
    {
        f64 Min = 0.0;
        f64 Max = 1.0;
        f64 Step = 0.0; ///< the increment the value snaps to, counted from Min; 0 means continuous
        f64 Page = 0.0; ///< a scroll bar's visible part, elsewhere the page key step (0 means a tenth of the range)
        f64 Value = 0.0;
        bool Exponential = false; ///< places the value along the track by logarithm (requires Min above 0)
        bool AllowGreater = false;
        bool AllowLesser = false;
        bool PageShown = false; ///< the page is a visible part (as in a scroll bar), not just a key step

        /// Returns the largest value: the maximum, less the page when the page is shown.
        [[nodiscard]] f64 Upper() const noexcept { return core::Max(Min, PageShown ? Max - Page : Max); }

        /// Returns `v` snapped to the step and kept within the range, unless the range allows
        /// values past it.
        [[nodiscard]] f64 Constrain(f64 v) const noexcept
        {
            if (Step > 0.0)
            {
                // Snap, then round to the step's decimals. Otherwise a step that came from a float
                // (0.1f is 0.100000001) leaves its error in the value: 12 snapped to it gives
                // 12.0000001788.
                v = Min + RoundHalfAway((v - Min) / Step) * Step;
                f64 scale = 1.0;
                for (i32 d = Decimals(); d > 0; --d)
                    scale *= 10.0;
                v = RoundHalfAway(v * scale) / scale;
            }
            if (!AllowGreater)
                v = core::Min(v, Upper());
            if (!AllowLesser)
                v = core::Max(v, Min);
            return v;
        }

        /// Returns where the value lies along [Min, Upper], from 0 to 1 (by logarithm when
        /// exponential).
        [[nodiscard]] f64 Ratio() const noexcept { return RatioOf(Value); }
        [[nodiscard]] f64 RatioOf(f64 v) const noexcept
        {
            const f64 lo = Min, hi = Upper();
            if (hi <= lo)
                return 0.0;
            if (UsesLog())
            {
                const f64 l = static_cast<f64>(core::Log(static_cast<f32>(lo)));
                const f64 h = static_cast<f64>(core::Log(static_cast<f32>(hi)));
                const f64 x = static_cast<f64>(core::Log(static_cast<f32>(core::Max(v, lo))));
                return Clamp01((x - l) / (h - l));
            }
            return Clamp01((v - lo) / (hi - lo));
        }
        /// Returns the value at `ratio` along [Min, Upper]. It is not snapped; Constrain does that.
        [[nodiscard]] f64 FromRatio(f64 ratio) const noexcept
        {
            const f64 lo = Min, hi = Upper();
            ratio = Clamp01(ratio);
            if (UsesLog())
            {
                const f64 l = static_cast<f64>(core::Log(static_cast<f32>(lo)));
                const f64 h = static_cast<f64>(core::Log(static_cast<f32>(hi)));
                return static_cast<f64>(core::Exp(static_cast<f32>(l + (h - l) * ratio)));
            }
            return lo + (hi - lo) * ratio;
        }

        /// Returns the arrow key step: the step, or a twentieth of the range when continuous.
        [[nodiscard]] f64 KeyStep() const noexcept { return Step > 0.0 ? Step : (Max - Min) / 20.0; }
        /// Returns the page key step: the page, or a tenth of the range when it has none, but never
        /// less than the arrow key step.
        [[nodiscard]] f64 PageStep() const noexcept { return core::Max(Page > 0.0 ? Page : (Max - Min) / 10.0, KeyStep()); }

        /// Returns how many decimals the step needs: 2 for 0.25, 0 for 1, and 3 when continuous.
        [[nodiscard]] i32 Decimals() const noexcept
        {
            if (Step <= 0.0)
                return 3;
            // Count a value within float precision of a whole number as whole, so a step read from
            // a float (0.1f is 0.100000001) gets one decimal rather than eight.
            i32 decimals = 0;
            f64 s = Step;
            while (decimals < 8 && core::Abs(s - RoundHalfAway(s)) > 1e-6 * core::Max(1.0, core::Abs(s)))
            {
                s *= 10.0;
                ++decimals;
            }
            return decimals;
        }

        [[nodiscard]] bool operator==(const RangeValue&) const noexcept = default;

    private:
        [[nodiscard]] bool UsesLog() const noexcept { return Exponential && Min > 0.0; }
        [[nodiscard]] static f64 Clamp01(f64 r) noexcept { return r < 0.0 ? 0.0 : (r > 1.0 ? 1.0 : r); }
        [[nodiscard]] static f64 RoundHalfAway(f64 x) noexcept
        {
            return static_cast<f64>(static_cast<i64>(x < 0.0 ? x - 0.5 : x + 0.5));
        }
    };
}
