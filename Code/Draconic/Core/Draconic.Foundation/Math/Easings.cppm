// Draconic Foundation - :easings partition
// Standard easing functions, ported faithfully from Sedulous.Core.Mathematics.Easings. Each maps an
// interpolation factor t in [0,1] to an eased value (also ~[0,1]). Consumed by animation (EasingType)
// and UI transitions. Pure functions over Foundation math (Sin/Cos/Sqrt/Pow/kPi).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:easings;

import :base;
import :math;

export namespace draconic::foundation
{
    // A function mapping t in [0,1] to an eased interpolation factor.
    using EasingFunction = f32 (*)(f32);

    [[nodiscard]] inline f32 EaseInLinear(f32 t) noexcept { return t; }
    [[nodiscard]] inline f32 EaseOutLinear(f32 t) noexcept { return t; }

    // --- quadratic ---
    [[nodiscard]] inline f32 EaseInQuadratic(f32 t) noexcept { return t * t; }
    [[nodiscard]] inline f32 EaseOutQuadratic(f32 t) noexcept { return -1.0f * t * (t - 2.0f); }
    [[nodiscard]] inline f32 EaseInOutQuadratic(f32 t) noexcept
    {
        t *= 2.0f;
        if (t < 1.0f)
        {
            return 0.5f * t * t;
        }
        t -= 1.0f;
        return -0.5f * (t * (t - 2.0f) - 1.0f);
    }

    // --- cubic ---
    [[nodiscard]] inline f32 EaseInCubic(f32 t) noexcept { return t * t * t; }
    [[nodiscard]] inline f32 EaseOutCubic(f32 t) noexcept
    {
        t -= 1.0f;
        return t * t * t + 1.0f;
    }
    [[nodiscard]] inline f32 EaseInOutCubic(f32 t) noexcept
    {
        t *= 2.0f;
        if (t < 1.0f)
        {
            return 0.5f * t * t * t;
        }
        t -= 2.0f;
        return 0.5f * (t * t * t + 2.0f);
    }

    // --- quartic ---
    [[nodiscard]] inline f32 EaseInQuartic(f32 t) noexcept { return t * t * t * t; }
    [[nodiscard]] inline f32 EaseOutQuartic(f32 t) noexcept
    {
        t -= 1.0f;
        return -1.0f * (t * t * t * t - 1.0f);
    }
    [[nodiscard]] inline f32 EaseInOutQuartic(f32 t) noexcept
    {
        t *= 2.0f;
        if (t < 1.0f)
        {
            return 0.5f * t * t * t * t;
        }
        t -= 2.0f;
        return -0.5f * (t * t * t * t - 2.0f);
    }

    // --- quintic ---
    [[nodiscard]] inline f32 EaseInQuintic(f32 t) noexcept { return t * t * t * t * t; }
    [[nodiscard]] inline f32 EaseOutQuintic(f32 t) noexcept
    {
        t -= 1.0f;
        return t * t * t * t * t + 1.0f;
    }
    [[nodiscard]] inline f32 EaseInOutQuintic(f32 t) noexcept
    {
        t *= 2.0f;
        if (t < 1.0f)
        {
            return 0.5f * t * t * t * t * t;
        }
        t -= 2.0f;
        return 0.5f * (t * t * t * t * t + 2.0f);
    }

    // --- sinusoidal ---
    [[nodiscard]] inline f32 EaseInSin(f32 t) noexcept { return -1.0f * Cos(t * kHalfPi) + 1.0f; }
    [[nodiscard]] inline f32 EaseOutSin(f32 t) noexcept { return Sin(t * kHalfPi); }
    [[nodiscard]] inline f32 EaseInOutSin(f32 t) noexcept { return -0.5f * (Cos(kPi * t) - 1.0f); }

    // --- exponential ---
    [[nodiscard]] inline f32 EaseInExponential(f32 t) noexcept
    {
        if (t == 0.0f)
        {
            return 0.0f;
        }
        return Pow(2.0f, 10.0f * (t - 1.0f));
    }
    [[nodiscard]] inline f32 EaseOutExponential(f32 t) noexcept
    {
        if (t == 1.0f)
        {
            return 1.0f;
        }
        return -Pow(2.0f, -10.0f * t) + 1.0f;
    }
    [[nodiscard]] inline f32 EaseInOutExponential(f32 t) noexcept
    {
        if (t == 0.0f)
        {
            return 0.0f;
        }
        if (t == 1.0f)
        {
            return 1.0f;
        }
        t *= 2.0f;
        if (t < 1.0f)
        {
            return 0.5f * Pow(2.0f, 10.0f * (t - 1.0f));
        }
        t -= 1.0f;
        return 0.5f * (-Pow(2.0f, -10.0f * t) + 2.0f);
    }

    // --- circular ---
    [[nodiscard]] inline f32 EaseInCircular(f32 t) noexcept
    {
        return -1.0f * (Sqrt(1.0f - t * t) - 1.0f);
    }
    [[nodiscard]] inline f32 EaseOutCircular(f32 t) noexcept
    {
        t -= 1.0f;
        return Sqrt(1.0f - t * t);
    }
    [[nodiscard]] inline f32 EaseInOutCircular(f32 t) noexcept
    {
        t *= 2.0f;
        if (t < 1.0f)
        {
            return -0.5f * (Sqrt(1.0f - t * t) - 1.0f);
        }
        t -= 2.0f;
        return 0.5f * (Sqrt(1.0f - t * t) + 1.0f);
    }

    // --- back (overshoot) ---
    [[nodiscard]] inline f32 EaseInBack(f32 t) noexcept
    {
        constexpr f32 s = 1.70158f;
        return t * t * ((s + 1.0f) * t - s);
    }
    [[nodiscard]] inline f32 EaseOutBack(f32 t) noexcept
    {
        constexpr f32 s = 1.70158f;
        t -= 1.0f;
        return t * t * ((s + 1.0f) * t + s) + 1.0f;
    }
    [[nodiscard]] inline f32 EaseInOutBack(f32 t) noexcept
    {
        constexpr f32 s = 1.70158f;
        constexpr f32 s2 = s * 1.525f;
        t *= 2.0f;
        if (t < 1.0f)
        {
            return 0.5f * (t * t * ((s2 + 1.0f) * t - s2));
        }
        t -= 2.0f;
        return 0.5f * (t * t * ((s2 + 1.0f) * t + s2) + 2.0f);
    }

    // --- elastic ---
    [[nodiscard]] inline f32 EaseInElastic(f32 t) noexcept
    {
        if (t == 0.0f)
        {
            return 0.0f;
        }
        if (t == 1.0f)
        {
            return 1.0f;
        }
        constexpr f32 p = 0.3f;
        constexpr f32 s = p / 4.0f;
        t -= 1.0f;
        return -(Pow(2.0f, 10.0f * t) * Sin((t - s) * (2.0f * kPi) / p));
    }
    [[nodiscard]] inline f32 EaseOutElastic(f32 t) noexcept
    {
        if (t == 0.0f)
        {
            return 0.0f;
        }
        if (t == 1.0f)
        {
            return 1.0f;
        }
        constexpr f32 p = 0.3f;
        constexpr f32 s = p / 4.0f;
        return Pow(2.0f, -10.0f * t) * Sin((t - s) * (2.0f * kPi) / p) + 1.0f;
    }
    [[nodiscard]] inline f32 EaseInOutElastic(f32 t) noexcept
    {
        if (t == 0.0f)
        {
            return 0.0f;
        }
        if (t == 1.0f)
        {
            return 1.0f;
        }
        t *= 2.0f;
        constexpr f32 p = 0.3f * 1.5f;
        constexpr f32 s = p / 4.0f;
        if (t < 1.0f)
        {
            t -= 1.0f;
            return -0.5f * (Pow(2.0f, 10.0f * t) * Sin((t - s) * (2.0f * kPi) / p));
        }
        t -= 1.0f;
        return Pow(2.0f, -10.0f * t) * Sin((t - s) * (2.0f * kPi) / p) * 0.5f + 1.0f;
    }

    // --- bounce (out defined first; in/inout reference it) ---
    [[nodiscard]] inline f32 EaseOutBounce(f32 t) noexcept
    {
        if (t < 1.0f / 2.75f)
        {
            return 7.5625f * t * t;
        }
        if (t < 2.0f / 2.75f)
        {
            t -= 1.5f / 2.75f;
            return 7.5625f * t * t + 0.75f;
        }
        if (t < 2.5f / 2.75f)
        {
            t -= 2.25f / 2.75f;
            return 7.5625f * t * t + 0.9375f;
        }
        t -= 2.625f / 2.75f;
        return 7.5625f * t * t + 0.984375f;
    }
    [[nodiscard]] inline f32 EaseInBounce(f32 t) noexcept { return 1.0f - EaseOutBounce(1.0f - t); }
    [[nodiscard]] inline f32 EaseInOutBounce(f32 t) noexcept
    {
        if (t < 0.5f)
        {
            return EaseInBounce(t * 2.0f) * 0.5f;
        }
        return EaseOutBounce(t * 2.0f - 1.0f) * 0.5f + 0.5f;
    }
}
