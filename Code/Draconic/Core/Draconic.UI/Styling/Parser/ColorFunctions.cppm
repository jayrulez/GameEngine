// Draconic UI - :color_functions partition
//
// Built-in color manipulation functions for .sss stylesheets:
// lighten($color, 10%), darken($color, 10%), alpha($color, 0.5), mix($a, $b, 0.5).
// Ported from Sedulous.UI/src/Styling/Parser/ColorFunctions.bf.
//
// Divergence (language): Math.Clamp -> Max(lo, Min(v, hi)); Color fields R/G/B/A -> r/g/b/a.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:color_functions;

import draconic.foundation; // Color, Min, Max
import :palette;

using namespace draconic::foundation;

export namespace draconic::ui
{
    struct ColorFunctions
    {
        /// Lighten a color by amount (0-1, specified as percentage in .sss).
        [[nodiscard]] static Color Lighten(Color color, f32 amount)
        {
            return Palette::Lighten(color, amount);
        }

        /// Darken a color by amount (0-1).
        [[nodiscard]] static Color Darken(Color color, f32 amount)
        {
            return Palette::Darken(color, amount);
        }

        /// Set the alpha channel of a color.
        [[nodiscard]] static Color Alpha(Color color, f32 alpha)
        {
            return Color{color.r, color.g, color.b, Max(0.0f, Min(alpha, 1.0f))};
        }

        /// Linear blend between two colors. t=0 returns a, t=1 returns b.
        [[nodiscard]] static Color Mix(Color a, Color b, f32 t)
        {
            const f32 f = Max(0.0f, Min(t, 1.0f));
            return Color{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f,
                         a.a + (b.a - a.a) * f};
        }
    };
}
