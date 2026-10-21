// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :gradient partition
// A colour gradient over time: colour stops (sRGB, as an authored colour is) and alpha stops apart,
// so a fade is edited without touching the colours, blended in a chosen space. EvaluateGradient is
// the engine's one gradient: the particles' colour over life samples with it, and an editor draws
// with it, so what is edited is what plays.

module;
#include "Core/Prelude.h"

export module foundation.core:gradient;

import :base;
import :math;
import :span;
import :color;

export namespace foundation::core
{
    // How the colours between two stops are blended.
    enum class GradientInterpolation : u8
    {
        Srgb,        // the sRGB values blended (what a CSS gradient and an image editor do)
        LinearLight, // blended as light: decoded to linear, blended, encoded back
        OkLab,       // blended in OKLab: even in lightness and hue
        Constant,    // a stop's colour held until the next
    };

    // A colour at a time; its alpha is the alpha stops', not its own.
    struct GradientColorStop
    {
        f32 time = 0.0f;
        Color color{1.0f, 1.0f, 1.0f, 1.0f};
    };
    struct GradientAlphaStop
    {
        f32 time = 0.0f;
        f32 alpha = 1.0f;
    };

    namespace detail
    {
        // The two stops either side of `time` and how far between them (stops sorted by time);
        // false for none. Outside the stops, the nearest end both sides.
        template <typename Stop>
        bool Bracket(Span<const Stop> stops, f32 time, usize& a, usize& b, f32& t) noexcept
        {
            const usize count = stops.Size();
            if (count == 0)
                return false;
            if (count == 1 || time <= stops[0].time)
            {
                a = b = 0;
                t = 0.0f;
                return true;
            }
            if (time >= stops[count - 1].time)
            {
                a = b = count - 1;
                t = 0.0f;
                return true;
            }
            usize i = 0;
            while (i + 1 < count && stops[i + 1].time <= time)
                ++i;
            a = i;
            b = i + 1;
            const f32 span = stops[b].time - stops[a].time;
            t = span > 1e-6f ? (time - stops[a].time) / span : 1.0f;
            return true;
        }

        inline f32 SignedCubeRoot(f32 x) noexcept { return x < 0.0f ? -Pow(-x, 1.0f / 3.0f) : Pow(x, 1.0f / 3.0f); }

        // Linear sRGB to OKLab and back (Björn Ottosson's matrices).
        inline void LinearToOkLab(Color c, f32& L, f32& A, f32& B) noexcept
        {
            const f32 l = SignedCubeRoot(0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b);
            const f32 m = SignedCubeRoot(0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b);
            const f32 s = SignedCubeRoot(0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b);
            L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
            A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
            B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
        }
        inline Color OkLabToLinear(f32 L, f32 A, f32 B) noexcept
        {
            const f32 l = L + 0.3963377774f * A + 0.2158037573f * B;
            const f32 m = L - 0.1055613458f * A - 0.0638541728f * B;
            const f32 s = L - 0.0894841775f * A - 1.2914855480f * B;
            const f32 l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
            return Color{4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3,
                         -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3,
                         -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3, 1.0f};
        }
    }

    // The sRGB colour of the gradient at `time`: its colour stops blended in `interpolation`, its
    // alpha from the alpha stops (held, under Constant; else straight between them). No colour
    // stops: white; no alpha stops: opaque. Stops sorted by time.
    [[nodiscard]] inline Color EvaluateGradient(Span<const GradientColorStop> colors, Span<const GradientAlphaStop> alphas,
                                                GradientInterpolation interpolation, f32 time) noexcept
    {
        Color out{1.0f, 1.0f, 1.0f, 1.0f};
        usize a = 0, b = 0;
        f32 t = 0.0f;
        if (detail::Bracket(colors, time, a, b, t))
        {
            const Color from = colors[a].color;
            const Color to = colors[b].color;
            switch (interpolation)
            {
            case GradientInterpolation::Constant:
                out = from;
                break;
            case GradientInterpolation::Srgb:
                out = Color{Lerp(from.r, to.r, t), Lerp(from.g, to.g, t), Lerp(from.b, to.b, t), 1.0f};
                break;
            case GradientInterpolation::LinearLight:
            {
                const Color x = ToLinear(from), y = ToLinear(to);
                out = ToSrgb(Color{Lerp(x.r, y.r, t), Lerp(x.g, y.g, t), Lerp(x.b, y.b, t), 1.0f});
                break;
            }
            case GradientInterpolation::OkLab:
            {
                f32 l0, a0, b0, l1, a1, b1;
                detail::LinearToOkLab(ToLinear(from), l0, a0, b0);
                detail::LinearToOkLab(ToLinear(to), l1, a1, b1);
                const Color mixed = detail::OkLabToLinear(Lerp(l0, l1, t), Lerp(a0, a1, t), Lerp(b0, b1, t));
                out = ToSrgb(Color{Clamp(mixed.r, 0.0f, 1.0f), Clamp(mixed.g, 0.0f, 1.0f), Clamp(mixed.b, 0.0f, 1.0f), 1.0f});
                break;
            }
            }
        }
        out.a = 1.0f;
        if (detail::Bracket(alphas, time, a, b, t))
            out.a = interpolation == GradientInterpolation::Constant ? alphas[a].alpha : Lerp(alphas[a].alpha, alphas[b].alpha, t);
        return out;
    }
}
