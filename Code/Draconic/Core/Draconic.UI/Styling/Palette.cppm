// Draconic UI - :palette partition
//
// Generates derived colors (hover/pressed/disabled/focused) from seed colors, and builds
// StateListDrawables from them. Ported from Sedulous.UI/src/Styling/Palette.bf. Static class ->
// struct with static methods; Color fields R/G/B/A -> r/g/b/a; returns RefPtr<StateListDrawable>.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:palette;

import draconic.foundation; // Color, Min, Max, RefPtr
import draconic.vg;   // CornerRadii
import :control_state;
import :color_drawable;
import :rounded_rect_drawable;
import :state_list_drawable;

using namespace draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::ui
{
    struct Palette
    {
        /// Lighten a color by a factor (0-1).
        [[nodiscard]] static Color Lighten(Color color, f32 amount) noexcept
        {
            const f32 a = Max(0.0f, Min(amount, 1.0f));
            return Color{Min(1.0f, color.r + (1.0f - color.r) * a),
                         Min(1.0f, color.g + (1.0f - color.g) * a),
                         Min(1.0f, color.b + (1.0f - color.b) * a), color.a};
        }

        /// Darken a color by a factor (0-1).
        [[nodiscard]] static Color Darken(Color color, f32 amount) noexcept
        {
            const f32 a = Max(0.0f, Min(amount, 1.0f));
            return Color{color.r * (1.0f - a), color.g * (1.0f - a), color.b * (1.0f - a), color.a};
        }

        [[nodiscard]] static Color ComputeHover(Color baseColor) noexcept
        {
            return Lighten(baseColor, 0.15f);
        }
        [[nodiscard]] static Color ComputePressed(Color baseColor) noexcept
        {
            return Darken(baseColor, 0.1f);
        }

        /// Desaturated + faded variant.
        [[nodiscard]] static Color ComputeDisabled(Color baseColor) noexcept
        {
            const f32 gray = baseColor.r * 0.30f + baseColor.g * 0.59f + baseColor.b * 0.11f;
            return Color{(gray + baseColor.r) * 0.5f, (gray + baseColor.g) * 0.5f,
                         (gray + baseColor.b) * 0.5f, baseColor.a * 0.6f};
        }

        /// Tinted toward accent.
        [[nodiscard]] static Color
        ComputeFocused(Color baseColor, Color accentColor = Color{60.0f / 255.0f, 130.0f / 255.0f,
                                                                  220.0f / 255.0f, 1.0f}) noexcept
        {
            return Color{baseColor.r * 0.8f + accentColor.r * 0.2f,
                         baseColor.g * 0.8f + accentColor.g * 0.2f,
                         baseColor.b * 0.8f + accentColor.b * 0.2f, baseColor.a};
        }

        /// StateListDrawable of ColorDrawables, auto-generating hover/pressed/disabled/focused variants.
        [[nodiscard]] static RefPtr<StateListDrawable> CreateStateColors(Color baseColor)
        {
            RefPtr<StateListDrawable> sl = MakeRef<StateListDrawable>(DefaultAllocator());
            sl->Set(ControlState::Normal, MakeRef<ColorDrawable>(DefaultAllocator(), baseColor));
            sl->Set(ControlState::Hover,
                    MakeRef<ColorDrawable>(DefaultAllocator(), ComputeHover(baseColor)));
            sl->Set(ControlState::Pressed,
                    MakeRef<ColorDrawable>(DefaultAllocator(), ComputePressed(baseColor)));
            sl->Set(ControlState::Disabled,
                    MakeRef<ColorDrawable>(DefaultAllocator(), ComputeDisabled(baseColor)));
            sl->Set(ControlState::Focused,
                    MakeRef<ColorDrawable>(DefaultAllocator(), ComputeFocused(baseColor)));
            return sl;
        }

        /// StateListDrawable of RoundedRectDrawables with per-corner radii.
        [[nodiscard]] static RefPtr<StateListDrawable> CreateStateRounded(Color baseColor,
                                                                          vg::CornerRadii radii)
        {
            RefPtr<StateListDrawable> sl = MakeRef<StateListDrawable>(DefaultAllocator());
            sl->Set(ControlState::Normal,
                    MakeRef<RoundedRectDrawable>(DefaultAllocator(), baseColor, radii));
            sl->Set(ControlState::Hover, MakeRef<RoundedRectDrawable>(
                                             DefaultAllocator(), ComputeHover(baseColor), radii));
            sl->Set(
                ControlState::Pressed,
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), ComputePressed(baseColor), radii));
            sl->Set(ControlState::Disabled,
                    MakeRef<RoundedRectDrawable>(DefaultAllocator(), ComputeDisabled(baseColor),
                                                 radii));
            sl->Set(
                ControlState::Focused,
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), ComputeFocused(baseColor), radii));
            return sl;
        }
    };
}
